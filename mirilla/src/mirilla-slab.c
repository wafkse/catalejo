#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pkeys.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

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
};

/* Determine whether a slab set supplies every required consumer operation. */
static bool mirilla_slab_operations_valid(const struct mirilla_slab_operations *operation_table)
{
    bool owner_get_present = operation_table->owner_get != NULL;
    bool owner_put_present = operation_table->owner_put != NULL;
    bool publish_present = operation_table->publish != NULL;
    bool revoke_present = operation_table->revoke != NULL;

    return owner_get_present && owner_put_present && publish_present && revoke_present;
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
    vma->vm_ops = &mirilla_slab_vm_operations;
    vma->vm_private_data = slab_state;
    vm_flags_set(vma, VM_IO | VM_MIXEDMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
    vm_flags_clear(vma, VM_MAYEXEC);

    return 0;
}
