#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mirilla-device.h"
#include "mirilla-except.h"

/* All registrations visible to the exception path. Readers traverse under RCU. */
static LIST_HEAD(mirilla_except_registration_list);

/* Serialize registration publication and session teardown. */
static DEFINE_MUTEX(mirilla_except_registration_lock);

/* Return the exclusive end of an already-validated region. */
static unsigned long mirilla_except_region_end(const struct mirilla_except_region *region)
{
    return (unsigned long)region->region_address + (unsigned long)region->region_size;
}

/* Check that a region describes one complete page-aligned userspace VMA. */
static bool mirilla_except_region_is_valid(const struct mirilla_except_region *region)
{
    unsigned long end_address;

    if (!region->region_address || !region->region_size)
        return false;

    if (!PAGE_ALIGNED(region->region_address) || !PAGE_ALIGNED(region->region_size))
        return false;

    if (check_add_overflow((unsigned long)region->region_address,
                           (unsigned long)region->region_size, &end_address))
        return false;

    return access_ok((void __user *)(unsigned long)region->region_address, region->region_size);
}

/* Match one actual VMA against an exact declared region and flag policy. */
static bool mirilla_except_area_match(const struct vm_area_struct *except_area,
                                      const struct mirilla_except_region *except_region,
                                      vm_flags_t required_flags, vm_flags_t forbidden_flags)
{
    if (!except_area || !except_area->vm_file)
        return false;

    if (except_area->vm_start != except_region->region_address ||
        except_area->vm_end != mirilla_except_region_end(except_region))
        return false;

    if ((except_area->vm_flags & required_flags) != required_flags)
        return false;

    return !(except_area->vm_flags & forbidden_flags);
}

/* Check the executable protected-accessor and rollback region. */
static bool mirilla_except_rollback_area_is_valid(const struct vm_area_struct *except_area,
                                                  const struct mirilla_except_image *except_image)
{
    return mirilla_except_area_match(except_area, &except_image->rollback_region,
                                     MIRILLA_EXCEPT_ROLLBACK_REQUIRED_FLAGS,
                                     MIRILLA_EXCEPT_ROLLBACK_FORBIDDEN_FLAGS);
}

/* Check the read-only exception-table region. */
static bool mirilla_except_table_vma_valid(const struct vm_area_struct *except_area,
                                           const struct mirilla_except_image *except_image)
{
    return mirilla_except_area_match(except_area, &except_image->except_table,
                                     MIRILLA_EXCEPT_TABLE_REQUIRED_FLAGS,
                                     MIRILLA_EXCEPT_TABLE_FORBIDDEN_FLAGS);
}

/* Check that a shmem backing file has the expected size and immutable seal set. */
static bool mirilla_except_file_is_valid(struct file *except_file, unsigned long expected_size)
{
    struct shmem_inode_info *information;
    unsigned int except_seals;

    if (!shmem_file(except_file) || i_size_read(file_inode(except_file)) != expected_size)
        return false;

    information = SHMEM_I(file_inode(except_file));

    spin_lock(&information->lock);
    except_seals = information->seals;
    spin_unlock(&information->lock);

    return (except_seals & MIRILLA_EXCEPT_MEMFD_REQUIRED_FLAGS) ==
           MIRILLA_EXCEPT_MEMFD_REQUIRED_FLAGS;
}

/* Check that both regions are immutable views of the same expected backing object. */
static bool mirilla_except_backing_valid(const struct mirilla_except_image *except_image,
                                         const struct vm_area_struct *rollback_area,
                                         const struct vm_area_struct *table_area)
{
    struct file *file = rollback_area->vm_file;
    unsigned long backing_length;

    if (file != table_area->vm_file)
        return false;

    if (rollback_area->vm_pgoff != 0)
        return false;

    if (table_area->vm_pgoff != except_image->rollback_region.region_size >> PAGE_SHIFT)
        return false;

    if (check_add_overflow((unsigned long)except_image->rollback_region.region_size,
                           (unsigned long)except_image->except_table.region_size, &backing_length))
        return false;

    return mirilla_except_file_is_valid(file, backing_length);
}

/* Validate the complete immutable userspace image. */
static int mirilla_except_validate_image(const struct mirilla_except_image *except_image)
{
    struct vm_area_struct *rollback_area;
    struct vm_area_struct *table_area;

    int validate_status = -EINVAL;

    if (!mirilla_except_region_is_valid(&except_image->rollback_region))
        return -EINVAL;

    if (!mirilla_except_region_is_valid(&except_image->except_table))
        return -EINVAL;

    if (except_image->except_table.region_size % sizeof(struct mirilla_except_record))
        return -EINVAL;

    mmap_read_lock(current->mm);

    rollback_area = find_vma(current->mm, except_image->rollback_region.region_address);
    table_area = find_vma(current->mm, except_image->except_table.region_address);

    if (!mirilla_except_rollback_area_is_valid(rollback_area, except_image))
        goto unlock;

    if (!mirilla_except_table_vma_valid(table_area, except_image))
        goto unlock;

    if (!mirilla_except_backing_valid(except_image, rollback_area, table_area))
        goto unlock;

    validate_status = 0;

unlock:
    mmap_read_unlock(current->mm);

    return validate_status;
}

/* Determine whether one table slot is zero-filled page padding. */
static bool mirilla_except_record_is_empty(const struct mirilla_except_record *record)
{
    return !record->start_address && !record->end_address && !record->rollback_address &&
           !record->except_mask;
}

/* Resolve a field-relative address against the userspace field that stores it. */
static unsigned long mirilla_except_resolve(unsigned long field_address,
                                            virtual_relative_t displacement)
{
    return field_address + (unsigned long)displacement;
}

/* Check whether one address lies inside the executable rollback region. */
static bool mirilla_except_rollback_contains(const struct mirilla_except_image *except_image,
                                             unsigned long target_address)
{
    const virtual_address_t rollback_region_start = except_image->rollback_region.region_address;
    const unsigned long rollback_region_end =
        mirilla_except_region_end(&except_image->rollback_region);

    return target_address >= rollback_region_start && target_address < rollback_region_end;
}

/* Check whether one exception mask contains only classes understood by Mirilla. */
static bool mirilla_except_mask_valid(mirilla_except_mask_t except_mask)
{
    return except_mask && !(except_mask & ~MIRILLA_EXCEPT_X86_SUPPORTED_MASK);
}

/* Validate one resolved userspace exception record. */
static bool mirilla_except_record_valid(const struct mirilla_except_image *image,
                                        unsigned long start_address, unsigned long end_address,
                                        unsigned long rollback_address,
                                        mirilla_except_mask_t except_mask)
{
    if (!mirilla_except_rollback_contains(image, start_address))
        return false;

    if (end_address <= start_address ||
        end_address > mirilla_except_region_end(&image->rollback_region))
        return false;

    if (!mirilla_except_rollback_contains(image, rollback_address))
        return false;

    return mirilla_except_mask_valid(except_mask);
}

/* Validate every nonzero record once before publishing an immutable image. */
static int mirilla_except_validate_records(const struct mirilla_except_image *image)
{
    const struct mirilla_except_record
        __user *table = (const void __user *)(unsigned long)image->except_table.region_address;
    size_t record_capacity = image->except_table.region_size / sizeof(*table);
    size_t index;
    bool record_seen = false;

    for (index = 0; index < record_capacity; ++index) {
        struct mirilla_except_record record;
        unsigned long record_address;
        unsigned long start_address;
        unsigned long end_address;
        unsigned long rollback_address;

        if (copy_from_user(&record, &table[index], sizeof(record)))
            return -EFAULT;

        if (mirilla_except_record_is_empty(&record))
            continue;

        record_address = image->except_table.region_address + index * sizeof(record);
        start_address = mirilla_except_resolve(
            record_address + offsetof(struct mirilla_except_record, start_address),
            record.start_address);
        end_address = mirilla_except_resolve(record_address + offsetof(struct mirilla_except_record,
                                                                       end_address),
                                             record.end_address);
        rollback_address = mirilla_except_resolve(
            record_address + offsetof(struct mirilla_except_record, rollback_address),
            record.rollback_address);

        if (!mirilla_except_record_valid(image, start_address, end_address, rollback_address,
                                         record.except_mask))
            return -EINVAL;

        record_seen = true;
    }

    return record_seen ? 0 : -EINVAL;
}

/* Search the immutable userspace table directly while SMAP is relaxed. */
static bool mirilla_except_registration_lookup(const struct mirilla_except_registration *target,
                                               unsigned long instruction_pointer,
                                               mirilla_except_mask_t except_mask,
                                               unsigned long *rollback_address)
{
    const struct mirilla_except_region *region = &target->image.except_table;
    const struct mirilla_except_record
        __user *table = (const void __user *)(unsigned long)region->region_address;

    size_t record_count = region->region_size / sizeof(struct mirilla_except_record);

    size_t record_index;

    bool found = false;

    if (!user_access_begin(table, region->region_size))
        return false;

    for (record_index = 0; record_index < record_count; ++record_index) {
        const struct mirilla_except_record __user *record = &table[record_index];
        virtual_relative_t start_relative;
        virtual_relative_t end_relative;
        virtual_relative_t rollback_relative;
        mirilla_except_mask_t record_mask;
        unsigned long record_address;
        unsigned long start_address;
        unsigned long end_address;

        unsafe_get_user(start_relative, &record->start_address, access_fault);
        unsafe_get_user(end_relative, &record->end_address, access_fault);
        unsafe_get_user(rollback_relative, &record->rollback_address, access_fault);
        unsafe_get_user(record_mask, &record->except_mask, access_fault);

        if (!start_relative && !end_relative && !rollback_relative && !record_mask)
            continue;

        record_address = region->region_address + record_index * sizeof(*record);
        start_address = mirilla_except_resolve(
            record_address + offsetof(struct mirilla_except_record, start_address), start_relative);
        end_address = mirilla_except_resolve(
            record_address + offsetof(struct mirilla_except_record, end_address), end_relative);

        if (instruction_pointer < start_address || instruction_pointer >= end_address)
            continue;

        if (!(record_mask & except_mask))
            break;

        *rollback_address = mirilla_except_resolve(
            record_address + offsetof(struct mirilla_except_record, rollback_address),
            rollback_relative);
        found = true;
        break;
    }

access_fault:
    user_access_end();
    return found;
}

/* Find the unique registration for an address space. The registration lock must be held. */
static struct mirilla_except_registration *mirilla_except_registration_find(struct mm_struct *mm)
{
    struct mirilla_except_registration *registration;

    list_for_each_entry(registration, &mirilla_except_registration_list,
                        global_node) if (registration->mm == mm) return registration;

    return NULL;
}

/* Find a rollback for the current address space without sleeping or allocating. */
bool mirilla_except_lookup(struct mm_struct *mm, unsigned long instruction_pointer,
                           mirilla_except_mask_t except_mask, unsigned long *rollback_address)
{
    const struct mirilla_except_registration *except_registration;
    bool is_found = false;

    rcu_read_lock();
    list_for_each_entry_rcu(except_registration, &mirilla_except_registration_list, global_node)
    {
        if (except_registration->mm != mm)
            continue;

        if (!mirilla_except_registration_lookup(except_registration, instruction_pointer, except_mask,
                                                rollback_address))
            continue;

        is_found = true;

        break;
    }
    rcu_read_unlock();

    return is_found;
}

/* Register one immutable image for the calling address space and device session. */
static int mirilla_except_register(struct mirilla_device_context *device_context,
                                   const struct mirilla_except_image *image)
{
    struct mirilla_except_registration *registration;
    int status;

    if (!current->mm)
        return -EINVAL;

    status = mirilla_except_validate_image(image);
    if (status)
        return status;

    status = mirilla_except_validate_records(image);
    if (status)
        return status;

    registration = kzalloc(sizeof(*registration), GFP_KERNEL);
    if (!registration)
        return -ENOMEM;

    INIT_LIST_HEAD(&registration->global_node);
    INIT_LIST_HEAD(&registration->device_node);
    registration->mm = current->mm;
    registration->image = *image;

    mutex_lock(&mirilla_except_registration_lock);

    if (mirilla_except_registration_find(current->mm)) {
        mutex_unlock(&mirilla_except_registration_lock);
        kfree(registration);
        return -EEXIST;
    }

    mmgrab(registration->mm);
    list_add_tail(&registration->device_node, &device_context->except_registration_list);
    list_add_tail_rcu(&registration->global_node, &mirilla_except_registration_list);

    mutex_unlock(&mirilla_except_registration_lock);

    return 0;
}

void mirilla_except_remove_device(struct mirilla_device_context *device_context)
{
    struct mirilla_except_registration *registration;
    struct mirilla_except_registration *temporary;
    LIST_HEAD(retired);

    mutex_lock(&mirilla_except_registration_lock);
    list_for_each_entry_safe(registration, temporary, &device_context->except_registration_list,
                             device_node)
    {
        list_del(&registration->device_node);
        list_del_rcu(&registration->global_node);
        list_add_tail(&registration->device_node, &retired);
    }
    mutex_unlock(&mirilla_except_registration_lock);

    synchronize_rcu();

    list_for_each_entry_safe(registration, temporary, &retired, device_node)
    {
        list_del(&registration->device_node);
        mmdrop(registration->mm);
        kfree(registration);
    }
}

mirilla_command_status_t
mirilla_except_handle_command(struct mirilla_device_context *device_context,
                              mirilla_command_t target_command, mirilla_command_argument_t argument)
{
    union mirilla_except_register_io io;

    if (target_command != MIRILLA_COMMAND_EXCEPT_REGISTER)
        return -ENOTTY;

    if (copy_from_user(&io, (void __user *)argument, sizeof(io)))
        return -EFAULT;

    return mirilla_except_register(device_context, &io.argument.image);
}
