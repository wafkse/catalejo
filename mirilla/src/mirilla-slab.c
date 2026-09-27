#include <linux/errno.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pkeys.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#if defined(MIRILLA_KUNIT)
#include <kunit/test.h>
#endif

#include "mirilla-log.h"
#include "mirilla-slab.h"

/*
 * One VMA-owned mutable slab and its optional consumer publication.
 *
 * NOTE(invariant): slab_set is retained through its owner reference. slab_storage is writable only
 * while publication_handle is null. A publication is visible only while the VMA is read-only.
 */
struct mirilla_slab {
    /** Set defining this slab's size, limit, owner, and operations. */
    struct mirilla_slab_set *slab_set;
    /** Mutable vmalloc-backed userspace storage. */
    void *slab_storage;
    /** Consumer-owned immutable publication, or null while editable. */
    void *publication_handle;
    /** mm that owns the VMA; the live VMA keeps this pointer valid. */
    struct mm_struct *address_space;
    /** Sole owning VMA, stable because split, relocation, and merge are forbidden. */
    struct vm_area_struct *owner_vma;
};

/* Determine whether a slab set supplies every required consumer operation. */
static bool mirilla_slab_operations_valid(const struct mirilla_slab_operations *operation_table)
{
    bool owner_get_present = operation_table->owner_get != NULL;
    bool owner_put_present = operation_table->owner_put != NULL;
    bool publish_present = operation_table->publish != NULL;
    bool revoke_present = operation_table->revoke != NULL;
    bool fork_present = operation_table->fork_set != NULL;

    return owner_get_present && owner_put_present && publish_present && revoke_present &&
           fork_present;
}

/** Validate and initialize an unused slab set with immutable consumer configuration. */
int mirilla_slab_set_initialize(struct mirilla_slab_set *slab_set, void *owner_context,
                                const struct mirilla_slab_operations *operation_table,
                                virtual_size_t slab_size, unsigned int slab_limit)
{
    bool size_present = slab_size != 0;
    bool size_aligned = PAGE_ALIGNED(slab_size);
    bool limit_present = slab_limit != 0;

    if (!slab_set)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab set is missing");

    if (!owner_context)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab set owner is missing");

    if (!operation_table)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab operation table is missing");

    if (!mirilla_slab_operations_valid(operation_table))
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab operation table is incomplete");

    if (!size_present)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab size is zero");

    if (!size_aligned)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab size is not page aligned");

    if (!limit_present)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab limit is zero");

    slab_set->owner_context = owner_context;
    slab_set->operation_table = operation_table;
    slab_set->slab_size = slab_size;
    slab_set->slab_limit = slab_limit;
    atomic_set(&slab_set->slab_count, 0);

    return 0;
}

/** Determine whether a slab set has no admitted or constructing VMAs. */
bool mirilla_slab_set_empty(const struct mirilla_slab_set *slab_set)
{
    return atomic_read(&slab_set->slab_count) == 0;
}

/* Reserve one place below the set's hard VMA limit. */
static int mirilla_slab_reserve(struct mirilla_slab_set *slab_set)
{
    int slab_count = atomic_inc_return(&slab_set->slab_count);

    if (slab_count <= slab_set->slab_limit)
        return 0;

    atomic_dec(&slab_set->slab_count);

    MIRILLA_ERROR_AND_RETURN(-ENOSPC, "slab VMA limit reached");
}

/* Release one place previously reserved from the set's hard VMA limit. */
static void mirilla_slab_release(struct mirilla_slab_set *slab_set)
{
    atomic_dec(&slab_set->slab_count);
}

/* Fault in the vmalloc page corresponding to one slab offset. */
static vm_fault_t mirilla_slab_vm_fault(struct vm_fault *vmf)
{
    struct mirilla_slab *slab_state = vmf->vma->vm_private_data;
    unsigned long slab_offset = vmf->address - vmf->vma->vm_start;
    struct page *slab_page;

    if (!slab_state)
        MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS, "slab fault has no private state");

    if (slab_offset >= slab_state->slab_set->slab_size)
        MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS, "slab fault is outside its VMA");

    slab_page = vmalloc_to_page((char *)slab_state->slab_storage + slab_offset);
    if (!slab_page)
        MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS, "failed to resolve slab page");

    return vmf_insert_page(vmf->vma, vmf->address, slab_page);
}

/* Reject any VMA split that would violate one-slab one-VMA ownership. */
static int mirilla_slab_vm_may_split(struct vm_area_struct *vma, unsigned long address)
{
    MIRILLA_ERROR_AND_RETURN(-EPERM, "slab VMA split is not supported");
}

/* Reject VMA relocation or resizing for a fixed-size slab. */
static int mirilla_slab_vm_mremap(struct vm_area_struct *vma)
{
    MIRILLA_ERROR_AND_RETURN(-EPERM, "slab VMA remap is not supported");
}

/* Freeze mutable PTEs, copy the slab, and ask the consumer to publish it. */
static int mirilla_slab_publish(struct vm_area_struct *vma, struct mirilla_slab *slab_state)
{
    struct mirilla_slab_set *slab_set = slab_state->slab_set;
    void *publication_handle = NULL;
    void *snapshot_data;
    int error_code;

    snapshot_data = kvmalloc(slab_set->slab_size, GFP_KERNEL);
    if (!snapshot_data)
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate slab snapshot");

    /* NOTE(publication): mprotect holds the mmap write lock. Invalidating PTEs before the copy
     * prevents another thread from modifying storage until the read-only transition completes. */
    unmap_mapping_range(vma->vm_file->f_mapping, 0, 0, 1);
    memcpy(snapshot_data, slab_state->slab_storage, slab_set->slab_size);

    error_code = slab_set->operation_table->publish(slab_set->owner_context, snapshot_data,
                                                    slab_set->slab_size, &publication_handle);
    if (error_code) {
        kvfree(snapshot_data);

        MIRILLA_ERROR_AND_RETURN(error_code, "slab consumer rejected snapshot publication");
    }

    slab_state->publication_handle = publication_handle;

    return 0;
}

/* Revoke the consumer publication before restoring writable access. */
static int mirilla_slab_edit(struct mirilla_slab *slab_state)
{
    void *publication_handle = slab_state->publication_handle;

    if (!publication_handle)
        MIRILLA_ERROR_AND_RETURN(-EUCLEAN, "slab has no active publication");

    slab_state->publication_handle = NULL;
    slab_state->slab_set->operation_table->revoke(slab_state->slab_set->owner_context,
                                                  publication_handle);

    return 0;
}

/* Allocate a child-owned slab without exposing any parent-owned state on failure. */
static void mirilla_slab_vm_open(struct vm_area_struct *vma)
{
    struct mirilla_slab *parent = vma->vm_private_data;
    struct mirilla_slab_set *child_set;
    struct mirilla_slab *child;
    void *snapshot;
    int error_code;

    /* .open is void. NULL is the permanent fail-closed state for this VMA. The child-only
     * WIPEONFORK bit also makes dup_mmap skip copy_page_range after this callback. */
    vma->vm_private_data = NULL;
    vm_flags_set(vma, VM_WIPEONFORK);
    if (!parent || parent->address_space == vma->vm_mm)
        return;

    /* dup_mmap holds the parent mmap write lock here. No parent fault can repopulate these
     * PTEs before copy_page_range examines them. Zap even if a later allocation fails, so the
     * child can never inherit a PTE into the parent's vmalloc backing. */
    zap_special_vma_range(parent->owner_vma, parent->owner_vma->vm_start,
                          parent->slab_set->slab_size);

    child_set =
        parent->slab_set->operation_table->fork_set(parent->slab_set->owner_context, vma->vm_mm);
    if (IS_ERR(child_set))
        return;

    /* fork_set returns a temporary owner reference, separate from the VMA reference. */
    error_code = mirilla_slab_reserve(child_set);
    if (error_code)
        goto put_owner;
    if (!child_set->operation_table->owner_get(child_set->owner_context))
        goto release_slot;

    child = kzalloc(sizeof(*child), GFP_KERNEL);
    if (!child)
        goto put_vma_owner;
    child->slab_storage = vmalloc(child_set->slab_size);
    if (!child->slab_storage)
        goto free_child;

    memcpy(child->slab_storage, parent->slab_storage, child_set->slab_size);
    child->slab_set = child_set;
    child->address_space = vma->vm_mm;
    child->owner_vma = vma;

    if ((vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) == VM_READ) {
        snapshot = kvmalloc(child_set->slab_size, GFP_KERNEL);
        if (!snapshot)
            goto free_storage;
        memcpy(snapshot, child->slab_storage, child_set->slab_size);
        error_code = child_set->operation_table->publish(
            child_set->owner_context, snapshot, child_set->slab_size, &child->publication_handle);
        if (error_code) {
            kvfree(snapshot);
            goto free_storage;
        }
    } else if ((vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) != (VM_READ | VM_WRITE)) {
        goto free_storage;
    }

    vma->vm_private_data = child;
    child_set->operation_table->owner_put(child_set->owner_context);
    return;

free_storage:
    vfree(child->slab_storage);
free_child:
    kfree(child);
put_vma_owner:
    child_set->operation_table->owner_put(child_set->owner_context);
release_slot:
    mirilla_slab_release(child_set);
put_owner:
    child_set->operation_table->owner_put(child_set->owner_context);
}

/* Enforce whole-VMA writable and read-only publication transitions. */
static int mirilla_slab_vm_mprotect(struct vm_area_struct *vma, unsigned long start,
                                    unsigned long end, unsigned long newflags)
{
    struct mirilla_slab *slab_state = vma->vm_private_data;
    unsigned long permission_mask = VM_READ | VM_WRITE | VM_EXEC;
    unsigned long current_permissions = vma->vm_flags & permission_mask;
    unsigned long requested_permissions = newflags & permission_mask;
    bool whole_vma = start == vma->vm_start && end == vma->vm_end;
    bool pkey_unchanged = (newflags & ARCH_VM_PKEY_FLAGS) == (vma->vm_flags & ARCH_VM_PKEY_FLAGS);
    bool publish_transition;
    bool edit_transition;

    if (!slab_state)
        MIRILLA_ERROR_AND_RETURN(-EPERM, "slab mprotect has no private state");

    /* Core mprotect_fixup checks this only after the callback. Do it before changing the
     * publication so a sealed VMA cannot revoke a table and then fail with -EPERM. */
    if (vma->vm_flags & VM_SEALED)
        MIRILLA_ERROR_AND_RETURN(-EPERM, "sealed slab cannot change protection");

    if (!whole_vma)
        MIRILLA_ERROR_AND_RETURN(-EPERM, "partial slab mprotect is not supported");

    if (!pkey_unchanged)
        MIRILLA_ERROR_AND_RETURN(-EPERM, "slab pkey transition is not supported");

    if (requested_permissions == current_permissions) {
        bool publication_active = slab_state->publication_handle != NULL;
        bool writable_state = current_permissions == (VM_READ | VM_WRITE);
        bool published_state = current_permissions == VM_READ;
        bool state_consistent = (writable_state && !publication_active) ||
                                (published_state && publication_active);

        if (!state_consistent)
            MIRILLA_ERROR_AND_RETURN(-EUCLEAN, "slab state disagrees with VMA permissions");

        return 0;
    }

    publish_transition = current_permissions == (VM_READ | VM_WRITE) &&
                         requested_permissions == VM_READ;
    edit_transition = current_permissions == VM_READ &&
                      requested_permissions == (VM_READ | VM_WRITE);

    /* NOTE(invariant): The VMA permission transition and publication handle are one state machine.
     * Each callback consumes exactly one direction while the mmap write lock serializes it. */
    if (publish_transition)
        return mirilla_slab_publish(vma, slab_state);

    if (edit_transition)
        return mirilla_slab_edit(slab_state);

    MIRILLA_ERROR_AND_RETURN(-EPERM, "unsupported slab permission transition");
}

/* Revoke publication and release all VMA-owned slab resources. */
static void mirilla_slab_vm_close(struct vm_area_struct *vma)
{
    struct mirilla_slab *slab_state = vma->vm_private_data;
    struct mirilla_slab_set *slab_set;
    void *owner_context;

    if (!slab_state)
        return;

    vma->vm_private_data = NULL;
    slab_set = slab_state->slab_set;
    owner_context = slab_set->owner_context;

    /* NOTE(lifetime): Consumer visibility ends before backing storage and the retained owner are
     * released. Consumer readers may keep the immutable publication alive independently. */
    if (slab_state->publication_handle)
        slab_set->operation_table->revoke(owner_context, slab_state->publication_handle);

    vfree(slab_state->slab_storage);
    kfree(slab_state);
    mirilla_slab_release(slab_set);
    slab_set->operation_table->owner_put(owner_context);
}

/* Define the lifecycle shared by every slab consumer. */
static const struct vm_operations_struct mirilla_slab_vm_operations = {
    .fault = mirilla_slab_vm_fault,
    .open = mirilla_slab_vm_open,
    .close = mirilla_slab_vm_close,
    .may_split = mirilla_slab_vm_may_split,
    .mremap = mirilla_slab_vm_mremap,
    .mprotect = mirilla_slab_vm_mprotect,
};

/* Validate generic fixed-slab mapping shape and initial permissions. */
static int mirilla_slab_mapping_validate(const struct mirilla_slab_set *slab_set,
                                         const struct vm_area_struct *vma)
{
    unsigned long permissions = vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC);
    bool exact_size = vma->vm_end - vma->vm_start == slab_set->slab_size;
    bool zero_offset = vma->vm_pgoff == 0;
    bool shared_mapping = vma->vm_flags & VM_SHARED;
    bool writable_mapping = permissions == (VM_READ | VM_WRITE);
    bool executable_mapping = permissions & VM_EXEC;

    if (executable_mapping)
        MIRILLA_ERROR_AND_RETURN(-EACCES, "executable slab mapping is not supported");

    if (!exact_size)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab mapping has the wrong size");

    if (!zero_offset)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab mapping has a nonzero offset");

    if (!shared_mapping)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab mapping is not shared");

    if (!writable_mapping)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab mapping is not initially writable");

    return 0;
}

/** Validate and attach one complete writable shared VMA to a slab set. */
int mirilla_slab_map(struct mirilla_slab_set *slab_set, struct vm_area_struct *vma)
{
    struct mirilla_slab *slab_state;
    int error_code;

    if (!slab_set)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "slab set is missing");

    error_code = mirilla_slab_mapping_validate(slab_set, vma);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "invalid slab mapping");

    error_code = mirilla_slab_reserve(slab_set);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to reserve slab VMA");

    if (!slab_set->operation_table->owner_get(slab_set->owner_context)) {
        mirilla_slab_release(slab_set);

        MIRILLA_ERROR_AND_RETURN(-ESTALE, "slab owner was released during mapping");
    }

    slab_state = kzalloc(sizeof(*slab_state), GFP_KERNEL);
    if (!slab_state) {
        slab_set->operation_table->owner_put(slab_set->owner_context);
        mirilla_slab_release(slab_set);

        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate slab state");
    }

    slab_state->slab_storage = vzalloc(slab_set->slab_size);
    if (!slab_state->slab_storage) {
        kfree(slab_state);
        slab_set->operation_table->owner_put(slab_set->owner_context);
        mirilla_slab_release(slab_set);

        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate slab storage");
    }

    slab_state->slab_set = slab_set;
    slab_state->address_space = vma->vm_mm;
    slab_state->owner_vma = vma;
    vma->vm_ops = &mirilla_slab_vm_operations;
    vma->vm_private_data = slab_state;
    vm_flags_set(vma, VM_IO | VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP);
    vm_flags_clear(vma, VM_MAYEXEC);

    return 0;
}

#if defined(MIRILLA_KUNIT)
static struct mirilla_slab_set *mirilla_slab_test_failed_fork_set(void *owner_context,
                                                                  struct mm_struct *child_mm)
{
    (void)owner_context;
    (void)child_mm;
    return ERR_PTR(-ENOMEM);
}

void mirilla_slab_test_fork_failure(struct kunit *test)
{
    const struct mirilla_slab_operations operations = {
        .fork_set = mirilla_slab_test_failed_fork_set,
    };
    struct mm_struct *parent_mm = kunit_kzalloc(test, sizeof(*parent_mm), GFP_KERNEL);
    struct mm_struct *child_mm = kunit_kzalloc(test, sizeof(*child_mm), GFP_KERNEL);
    struct mirilla_slab_set slab_set = {
        .operation_table = &operations,
        .slab_size = PAGE_SIZE,
    };
    struct mirilla_slab parent = {
        .slab_set = &slab_set,
        .address_space = parent_mm,
    };
    /* The synthetic VMA lacks VM_MIXEDMAP, so zap_special_vma_range is a no-op. */
    struct vm_area_struct parent_vma = {
        .vm_mm = parent_mm,
        .vm_end = PAGE_SIZE,
    };
    struct vm_area_struct child_vma = {
        .vm_mm = child_mm,
        .vm_private_data = &parent,
    };
    struct vm_fault fault = {
        .vma = &child_vma,
    };

    KUNIT_ASSERT_NOT_NULL(test, parent_mm);
    KUNIT_ASSERT_NOT_NULL(test, child_mm);
    parent.owner_vma = &parent_vma;
    mirilla_slab_vm_open(&child_vma);
    KUNIT_EXPECT_PTR_EQ(test, child_vma.vm_private_data, NULL);
    KUNIT_EXPECT_PTR_EQ(test, parent.slab_set, &slab_set);
    KUNIT_EXPECT_EQ(test, mirilla_slab_vm_fault(&fault), VM_FAULT_SIGBUS);
    KUNIT_EXPECT_EQ(test, mirilla_slab_vm_mprotect(&child_vma, 0, 0, VM_READ), -EPERM);
}

struct mirilla_slab_test_fork_owner {
    struct mirilla_slab_set *child_set;
    int references;
    bool reject_publication;
    unsigned int publications;
    unsigned int revocations;
};

static struct mirilla_slab_set *mirilla_slab_test_reserved_fork_set(void *owner_context,
                                                                    struct mm_struct *child_mm)
{
    struct mirilla_slab_test_fork_owner *owner = owner_context;

    (void)child_mm;
    owner->references++;
    return owner->child_set;
}

static void mirilla_slab_test_reserved_owner_put(void *owner_context)
{
    struct mirilla_slab_test_fork_owner *owner = owner_context;

    owner->references--;
}

static bool mirilla_slab_test_reserved_owner_get(void *owner_context)
{
    struct mirilla_slab_test_fork_owner *owner = owner_context;

    owner->references++;
    return true;
}

static int mirilla_slab_test_clone_publish(void *owner_context, void *snapshot_data,
                                           virtual_size_t snapshot_size, void **publication_handle)
{
    struct mirilla_slab_test_fork_owner *owner = owner_context;

    (void)snapshot_size;
    if (owner->reject_publication)
        return -ENOMEM;
    owner->publications++;
    *publication_handle = snapshot_data;
    return 0;
}

static void mirilla_slab_test_clone_revoke(void *owner_context, void *publication_handle)
{
    struct mirilla_slab_test_fork_owner *owner = owner_context;

    owner->revocations++;
    kvfree(publication_handle);
}

void mirilla_slab_test_fork_reservation_failure(struct kunit *test)
{
    const struct mirilla_slab_operations operations = {
        .fork_set = mirilla_slab_test_reserved_fork_set,
        .owner_put = mirilla_slab_test_reserved_owner_put,
    };
    struct mirilla_slab_set parent_set = { .operation_table = &operations, .slab_size = PAGE_SIZE };
    struct mirilla_slab_set child_set = {
        .operation_table = &operations,
        .slab_size = PAGE_SIZE,
        .slab_limit = 1,
    };
    struct mirilla_slab_test_fork_owner owner = { .child_set = &child_set };
    struct mm_struct *parent_mm = kunit_kzalloc(test, sizeof(*parent_mm), GFP_KERNEL);
    struct mm_struct *child_mm = kunit_kzalloc(test, sizeof(*child_mm), GFP_KERNEL);
    struct vm_area_struct parent_vma = { .vm_mm = parent_mm, .vm_end = PAGE_SIZE };
    struct mirilla_slab parent = {
        .slab_set = &parent_set,
        .address_space = parent_mm,
        .owner_vma = &parent_vma,
    };
    struct vm_area_struct child_vma = { .vm_mm = child_mm, .vm_private_data = &parent };

    KUNIT_ASSERT_NOT_NULL(test, parent_mm);
    KUNIT_ASSERT_NOT_NULL(test, child_mm);
    parent_set.owner_context = &owner;
    child_set.owner_context = &owner;
    atomic_set(&child_set.slab_count, 1);
    mirilla_slab_vm_open(&child_vma);
    KUNIT_EXPECT_PTR_EQ(test, child_vma.vm_private_data, NULL);
    KUNIT_EXPECT_EQ(test, atomic_read(&child_set.slab_count), 1);
    KUNIT_EXPECT_EQ(test, owner.references, 0);
    KUNIT_EXPECT_PTR_EQ(test, parent.owner_vma, &parent_vma);
}

void mirilla_slab_test_fork_publication_lifecycle(struct kunit *test)
{
    const struct mirilla_slab_operations operations = {
        .owner_get = mirilla_slab_test_reserved_owner_get,
        .owner_put = mirilla_slab_test_reserved_owner_put,
        .fork_set = mirilla_slab_test_reserved_fork_set,
        .publish = mirilla_slab_test_clone_publish,
        .revoke = mirilla_slab_test_clone_revoke,
    };
    struct mirilla_slab_test_fork_owner owner = { .reject_publication = true };
    struct mirilla_slab_set parent_set = { .operation_table = &operations,
                                           .slab_size = PAGE_SIZE,
                                           .owner_context = &owner };
    struct mirilla_slab_set child_set = { .operation_table = &operations,
                                          .slab_size = PAGE_SIZE,
                                          .slab_limit = 1,
                                          .owner_context = &owner };
    struct mm_struct *parent_mm = kunit_kzalloc(test, sizeof(*parent_mm), GFP_KERNEL);
    struct mm_struct *child_mm = kunit_kzalloc(test, sizeof(*child_mm), GFP_KERNEL);
    void *parent_storage = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
    struct vm_area_struct parent_vma = { .vm_mm = parent_mm, .vm_end = PAGE_SIZE };
    struct mirilla_slab parent = { .slab_set = &parent_set,
                                   .slab_storage = parent_storage,
                                   .address_space = parent_mm,
                                   .owner_vma = &parent_vma };
    struct vm_area_struct child_vma = {
        .vm_mm = child_mm, .vm_end = PAGE_SIZE, .vm_flags = VM_READ, .vm_private_data = &parent
    };
    struct mirilla_slab *child;

    KUNIT_ASSERT_NOT_NULL(test, parent_mm);
    KUNIT_ASSERT_NOT_NULL(test, child_mm);
    KUNIT_ASSERT_NOT_NULL(test, parent_storage);
    memset(parent_storage, 0xa5, PAGE_SIZE);
    owner.child_set = &child_set;
    atomic_set(&child_set.slab_count, 0);

    mirilla_slab_vm_open(&child_vma);
    KUNIT_EXPECT_PTR_EQ(test, child_vma.vm_private_data, NULL);
    KUNIT_EXPECT_EQ(test, atomic_read(&child_set.slab_count), 0);
    KUNIT_EXPECT_EQ(test, owner.references, 0);
    KUNIT_EXPECT_EQ(test, owner.publications, 0U);

    owner.reject_publication = false;
    child_vma.vm_private_data = &parent;
    mirilla_slab_vm_open(&child_vma);
    child = child_vma.vm_private_data;
    KUNIT_ASSERT_NOT_NULL(test, child);
    KUNIT_EXPECT_PTR_NE(test, child->slab_storage, parent_storage);
    KUNIT_EXPECT_EQ(test, memcmp(child->slab_storage, parent_storage, PAGE_SIZE), 0);
    KUNIT_EXPECT_PTR_NE(test, child->publication_handle, NULL);
    KUNIT_EXPECT_EQ(test, atomic_read(&child_set.slab_count), 1);
    KUNIT_EXPECT_EQ(test, owner.references, 1);
    KUNIT_EXPECT_EQ(test, owner.publications, 1U);

    mirilla_slab_vm_close(&child_vma);
    KUNIT_EXPECT_EQ(test, atomic_read(&child_set.slab_count), 0);
    KUNIT_EXPECT_EQ(test, owner.references, 0);
    KUNIT_EXPECT_EQ(test, owner.revocations, 1U);
    KUNIT_EXPECT_EQ(test, ((unsigned char *)parent_storage)[0], (unsigned char)0xa5);
}
#endif
