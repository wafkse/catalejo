#include <asm/trapnr.h>
#include <asm/vdso.h>

#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>

#include "mirilla-device.h"
#include "mirilla-except.h"
#include "mirilla-log.h"

/*
 * All published registrations. Readers traverse this list under RCU.
 */
static LIST_HEAD(mirilla_except_registration_list);

/*
 * Serializes registration publication and session teardown.
 */
static DEFINE_MUTEX(mirilla_except_registration_lock);

/*
 * Convert an x86 exception number to the record mask and legacy signal value.
 */
static bool mirilla_except_classify(int trap_number, mirilla_except_signal_mask_t *except_mask,
                                    unsigned long *signal_number)
{
    switch (trap_number) {
    case X86_TRAP_UD:
        *except_mask = MIRILLA_EXCEPT_SIGNAL_ILLEGAL_INSTRUCTION;
        *signal_number = SIGILL;
        return true;
    case X86_TRAP_AC:
        *except_mask = MIRILLA_EXCEPT_SIGNAL_BUS_ERROR;
        *signal_number = SIGBUS;
        return true;
    case X86_TRAP_GP:
    case X86_TRAP_PF:
        *except_mask = MIRILLA_EXCEPT_SIGNAL_SEGMENTATION_FAULT;
        *signal_number = SIGSEGV;
        return true;
    default:
        return false;
    }
}

/*
 * Search one sorted registration for an instruction and exception class.
 */
static bool mirilla_except_registration_lookup(const struct mirilla_except_registration *target,
                                               unsigned long instruction_pointer,
                                               mirilla_except_signal_mask_t except_mask,
                                               unsigned long *fixup_address)
{
    size_t left = 0;
    size_t right = target->record_count;

    while (left < right) {
        size_t middle = left + (right - left) / 2;
        const struct mirilla_except_kernel_record *record = &target->records[middle];

        if (instruction_pointer < record->start_address)
            right = middle;
        else if (instruction_pointer >= record->end_address)
            left = middle + 1;
        else if (!(record->except_mask & except_mask))
            return false;
        else {
            *fixup_address = record->fixup_address;
            return true;
        }
    }

    return false;
}

/*
 * Find a fixup for the current address space without sleeping or allocating.
 */
static bool mirilla_except_lookup(struct mm_struct *mm, unsigned long instruction_pointer,
                                  mirilla_except_signal_mask_t except_mask,
                                  unsigned long *fixup_address)
{
    const struct mirilla_except_registration *target;
    bool found = false;

    rcu_read_lock();
    list_for_each_entry_rcu(target, &mirilla_except_registration_list, global_node)
    {
        if (target->mm == mm && mirilla_except_registration_lookup(target, instruction_pointer,
                                                                   except_mask, fixup_address)) {
            found = true;
            break;
        }
    }
    rcu_read_unlock();

    return found;
}

/*
 * Replacement return path used after the callback has repaired userspace RIP.
 */
static notrace bool mirilla_except_fixed(struct pt_regs *registers, int trap_number,
                                         unsigned long error_code, unsigned long fault_address)
{
    return true;
}

/*
 * Intercept the native vDSO fixup choke point for registered accessor instructions.
 */
static notrace void mirilla_except_ftrace(unsigned long instruction_pointer,
                                          unsigned long parent_instruction_pointer,
                                          struct ftrace_ops *operations,
                                          struct ftrace_regs *ftrace_registers)
{
    struct pt_regs *user_registers;
    struct mm_struct *mm;
    mirilla_except_signal_mask_t except_mask;
    unsigned long signal_number;
    unsigned long fixup_address;
    int trap_number;

    if (!ftrace_regs_has_args(ftrace_registers))
        return;

    user_registers = (struct pt_regs *)ftrace_regs_get_argument(ftrace_registers, 0);
    trap_number = (int)ftrace_regs_get_argument(ftrace_registers, 1);
    mm = current->mm;

    if (!user_registers || !user_mode(user_registers) || !mm ||
        !mirilla_except_classify(trap_number, &except_mask, &signal_number) ||
        !mirilla_except_lookup(mm, user_registers->ip, except_mask, &fixup_address))
        return;

    user_registers->ip = fixup_address;
    /*
     * Preserve the existing accessor ABI. Monitor recovery distinguishes an unsupported
     * instruction from a memory fault through the second integer return register. Scalar and copy
     * recovery paths either ignore or replace this value.
     */
    user_registers->dx = signal_number;
    ftrace_regs_set_instruction_pointer(ftrace_registers, (unsigned long)mirilla_except_fixed);
}

static struct ftrace_ops mirilla_except_ftrace_operations = {
    .func = mirilla_except_ftrace,
    .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY | FTRACE_OPS_FL_RECURSION |
             FTRACE_OPS_FL_PERMANENT,
};

/*
 * Compare absolute records by their half-open instruction ranges.
 */
static int mirilla_except_record_compare(const void *left_value, const void *right_value)
{
    const struct mirilla_except_kernel_record *left = left_value;
    const struct mirilla_except_kernel_record *right = right_value;

    if (left->start_address < right->start_address)
        return -1;
    if (left->start_address > right->start_address)
        return 1;
    if (left->end_address < right->end_address)
        return -1;
    if (left->end_address > right->end_address)
        return 1;
    return 0;
}

/*
 * Check one VMA against the exact immutable image layout.
 */
static bool mirilla_except_vma_valid(struct vm_area_struct *area, unsigned long address,
                                     unsigned long length, vm_flags_t required,
                                     vm_flags_t forbidden)
{
    return area && area->vm_start == address && area->vm_end == address + length && area->vm_file &&
           (area->vm_flags & required) == required && !(area->vm_flags & forbidden);
}

/*
 * Read the seal word from a shmem-backed file.
 */
static unsigned int mirilla_except_file_seals(struct file *file)
{
    struct shmem_inode_info *information = SHMEM_I(file_inode(file));
    unsigned int seals;

    spin_lock(&information->lock);
    seals = information->seals;
    spin_unlock(&information->lock);

    return seals;
}

/*
 * Validate both VMAs and their common immutable backing object.
 */
static int mirilla_except_validate_image(const struct mirilla_except_image *image)
{
    const unsigned int required_seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    struct vm_area_struct *accessor_area;
    struct vm_area_struct *table_area;
    struct file *accessor_file;
    unsigned long accessor_address = image->accessor_address;
    unsigned long accessor_length = image->accessor_length;
    unsigned long table_address = image->table_address;
    unsigned long table_length = image->table_length;
    unsigned long backing_length;
    int status = -EINVAL;

    if (!accessor_address || !table_address || !accessor_length || !table_length ||
        !PAGE_ALIGNED(accessor_address) || !PAGE_ALIGNED(table_address) ||
        !PAGE_ALIGNED(accessor_length) || !PAGE_ALIGNED(table_length) ||
        check_add_overflow(accessor_length, table_length, &backing_length) ||
        check_add_overflow(accessor_address, accessor_length, &backing_length) ||
        check_add_overflow(table_address, table_length, &backing_length) ||
        !access_ok((void __user *)accessor_address, accessor_length) ||
        !access_ok((void __user *)table_address, table_length))
        return -EINVAL;

    mmap_read_lock(current->mm);

    accessor_area = find_vma(current->mm, accessor_address);
    table_area = find_vma(current->mm, table_address);

    if (!mirilla_except_vma_valid(accessor_area, accessor_address, accessor_length,
                                  VM_READ | VM_EXEC | VM_SHARED | VM_SEALED,
                                  VM_WRITE | VM_MAYWRITE) ||
        !mirilla_except_vma_valid(table_area, table_address, table_length,
                                  VM_READ | VM_SHARED | VM_SEALED,
                                  VM_WRITE | VM_EXEC | VM_MAYWRITE) ||
        accessor_area->vm_file != table_area->vm_file || accessor_area->vm_pgoff != 0 ||
        table_area->vm_pgoff != accessor_length >> PAGE_SHIFT) {
        goto unlock;
    }

    accessor_file = accessor_area->vm_file;
    backing_length = accessor_length + table_length;

    if (!shmem_file(accessor_file) || i_size_read(file_inode(accessor_file)) != backing_length ||
        (mirilla_except_file_seals(accessor_file) & required_seals) != required_seals)
        goto unlock;

    status = 0;

unlock:
    mmap_read_unlock(current->mm);

    return status;
}

/*
 * Read and validate the self-contained exception-table header.
 */
static int mirilla_except_record_count(const struct mirilla_except_image *image,
                                       size_t *record_count)
{
    struct mirilla_except_table_header header;
    size_t record_bytes;
    size_t table_bytes;

    if (image->table_length < sizeof(header))
        return -EINVAL;

    if (copy_from_user(&header, (void __user *)(unsigned long)image->table_address, sizeof(header)))
        return -EFAULT;

    if (!header.record_count || header.record_count > SIZE_MAX ||
        check_mul_overflow((size_t)header.record_count, sizeof(struct mirilla_except_record),
                           &record_bytes) ||
        check_add_overflow(sizeof(header), record_bytes, &table_bytes) ||
        table_bytes > image->table_length)
        return -EINVAL;

    *record_count = (size_t)header.record_count;

    return 0;
}

/*
 * Resolve a copied field-relative address using the original userspace field location.
 */
static unsigned long mirilla_except_resolve(unsigned long field_address,
                                            virtual_relative_t displacement)
{
    return field_address + (unsigned long)displacement;
}

/*
 * Validate and normalize the immutable userspace record table.
 */
static int mirilla_except_copy_records(struct mirilla_except_registration *registration)
{
    const struct mirilla_except_image *image = &registration->image;
    const size_t record_size = sizeof(struct mirilla_except_record);
    const mirilla_except_signal_mask_t supported_mask = MIRILLA_EXCEPT_SIGNAL_SEGMENTATION_FAULT |
                                                        MIRILLA_EXCEPT_SIGNAL_BUS_ERROR |
                                                        MIRILLA_EXCEPT_SIGNAL_ILLEGAL_INSTRUCTION;
    struct mirilla_except_record *records;
    size_t table_bytes;
    size_t index;
    int status = -EINVAL;

    if (check_mul_overflow(registration->record_count, record_size, &table_bytes) || !table_bytes ||
        table_bytes > image->table_length - sizeof(struct mirilla_except_table_header))
        return -EINVAL;

    records =
        memdup_user((void __user *)(unsigned long)(image->table_address +
                                                   sizeof(struct mirilla_except_table_header)),
                    table_bytes);
    if (IS_ERR(records))
        return PTR_ERR(records);

    for (index = 0; index < registration->record_count; ++index) {
        const struct mirilla_except_record *source = &records[index];
        struct mirilla_except_kernel_record *target = &registration->records[index];
        unsigned long source_address =
            image->table_address + sizeof(struct mirilla_except_table_header) + index * record_size;

        target->start_address = mirilla_except_resolve(
            source_address + offsetof(struct mirilla_except_record, start_address),
            source->start_address);
        target->end_address = mirilla_except_resolve(
            source_address + offsetof(struct mirilla_except_record, end_address),
            source->end_address);
        target->fixup_address = mirilla_except_resolve(
            source_address + offsetof(struct mirilla_except_record, rollback_address),
            source->rollback_address);
        target->except_mask = source->except_mask;

        if (target->start_address < image->accessor_address ||
            target->start_address >= image->accessor_address + image->accessor_length ||
            target->end_address <= target->start_address ||
            target->end_address > image->accessor_address + image->accessor_length ||
            target->fixup_address < image->accessor_address ||
            target->fixup_address >= image->accessor_address + image->accessor_length ||
            !target->except_mask || target->except_mask & ~supported_mask)
            goto free_records;
    }

    sort(registration->records, registration->record_count,
         sizeof(struct mirilla_except_kernel_record), mirilla_except_record_compare, NULL);

    for (index = 1; index < registration->record_count; ++index)
        if (registration->records[index - 1].end_address >
            registration->records[index].start_address)
            goto free_records;

    status = 0;

free_records:
    kfree(records);

    return status;
}

/*
 * Compare registration identity for idempotent command handling.
 */
static bool mirilla_except_same_image(const struct mirilla_except_image *left,
                                      const struct mirilla_except_image *right)
{
    return left->accessor_address == right->accessor_address &&
           left->accessor_length == right->accessor_length &&
           left->table_address == right->table_address && left->table_length == right->table_length;
}

/*
 * Register one immutable image for the calling address space and device session.
 */
static int mirilla_except_register(struct mirilla_device_context *device_context,
                                   const struct mirilla_except_image *image)
{
    struct mirilla_except_registration *registration;
    struct mirilla_except_registration *existing;
    size_t allocation_size;
    size_t record_count;
    int status;

    if (!current->mm)
        return -EINVAL;

    status = mirilla_except_validate_image(image);
    if (status)
        return status;

    status = mirilla_except_record_count(image, &record_count);
    if (status)
        return status;

    if (check_mul_overflow(record_count, sizeof(struct mirilla_except_kernel_record),
                           &allocation_size) ||
        check_add_overflow(sizeof(*registration), allocation_size, &allocation_size))
        return -EOVERFLOW;

    registration = kzalloc(allocation_size, GFP_KERNEL);
    if (!registration)
        return -ENOMEM;

    INIT_LIST_HEAD(&registration->global_node);
    INIT_LIST_HEAD(&registration->device_node);
    registration->mm = current->mm;
    registration->image = *image;
    registration->record_count = record_count;

    status = mirilla_except_copy_records(registration);
    if (status)
        goto free_registration;

    mutex_lock(&mirilla_except_registration_lock);
    list_for_each_entry(existing, &device_context->except_registration_list, device_node)
    {
        if (existing->mm != current->mm)
            continue;

        status = mirilla_except_same_image(&existing->image, image) ? 0 : -EEXIST;
        goto unlock;
    }

    mmgrab(registration->mm);
    list_add_tail(&registration->device_node, &device_context->except_registration_list);
    list_add_tail_rcu(&registration->global_node, &mirilla_except_registration_list);
    registration = NULL;
    status = 0;

unlock:
    mutex_unlock(&mirilla_except_registration_lock);

free_registration:
    kfree(registration);

    return status;
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
                              mirilla_command_t command, mirilla_command_argument_t argument)
{
    union mirilla_except_register_io io;

    if (command != MIRILLA_COMMAND_EXCEPT_REGISTER)
        return -ENOTTY;

    if (copy_from_user(&io, (void __user *)argument, sizeof(io)))
        return -EFAULT;

    return mirilla_except_register(device_context, &io.argument.image);
}

int mirilla_except_initialize(void)
{
    int status;

    status = ftrace_set_filter(&mirilla_except_ftrace_operations, "fixup_vdso_exception",
                               sizeof("fixup_vdso_exception") - 1, 0);
    if (status)
        return status;

    status = register_ftrace_function(&mirilla_except_ftrace_operations);
    if (status)
        ftrace_set_filter(&mirilla_except_ftrace_operations, NULL, 0, 1);

    return status;
}

void mirilla_except_deinitialize(void)
{
    unregister_ftrace_function(&mirilla_except_ftrace_operations);
    ftrace_set_filter(&mirilla_except_ftrace_operations, NULL, 0, 1);
}
