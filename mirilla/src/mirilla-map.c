/*
 * Mapping subsystem for Mirilla.
 */

#include "asm-generic/errno-base.h"
#include "linux/compiler_attributes.h"
#include "linux/errno.h"
#include "linux/gfp_types.h"
#include "linux/pid.h"
#include "linux/pid_types.h"
#include "linux/sched.h"
#include "linux/sched/signal.h"
#include "linux/sched/task.h"

#include "linux/slab.h"
#include "linux/uaccess.h"

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/rcupdate.h>

#include "mirilla-id.h"
#include "mirilla-log.h"
#include "mirilla-map.h"
#include "mirilla-device.h"
#include "mirilla-command.h"

/*
 * Declare context-specific reference-counting helper functions.
 */
#define X(context_name)                                                              \
    MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(map_##context_name)                         \
    {                                                                                \
        return mirilla_context_reference_get(map_##context_name, context_structure); \
    }
MIRILLA_MAP_CONTEXT_LIST
#undef X

#define X(context_name)                                                       \
    MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(map_##context_name)                  \
    {                                                                         \
        mirilla_context_reference_set(map_##context_name, context_structure); \
    }
MIRILLA_MAP_CONTEXT_LIST
#undef X

MIRILLA_CONTEXT_CONSTRUCTOR(map_target)
{
    int error_code = 0;
    struct mirilla_map_target_context *target_context = NULL;

    if (!(*context_storage = target_context =
              kzalloc(sizeof(struct mirilla_map_target_context), GFP_KERNEL)))
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate target context");

    mirilla_context_initialize(target_context);

    target_context->id = MIRILLA_ID_NONE;

    target_context->process_id = NULL;

    atomic_set(&target_context->peephole_count, 0);

    return error_code;
}

MIRILLA_CONTEXT_DESTRUCTOR(map_target)
{
    MIRILLA_DEBUG(MIRILLA_LOG_PREFIX_LIFETIME "destruct `map_target`");

    if (target_context->process_id) {
        put_pid(target_context->process_id);

        target_context->process_id = NULL;
    }

    MIRILLA_DEBUG(MIRILLA_LOG_PREFIX_LIFETIME "defer `kfree`");

    kfree_rcu(target_context, teardown_callback);
}

MIRILLA_CONTEXT_CONSTRUCTOR(map_peephole)
{
    int error_code = 0;
    struct mirilla_map_peephole_context *target_context = NULL;

    if (!(*context_storage = target_context =
              kzalloc(sizeof(struct mirilla_map_peephole_context), GFP_KERNEL)))
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate peephole context");

    mirilla_context_initialize(target_context);

    target_context->id = MIRILLA_ID_NONE;

    atomic_set_release(&target_context->peephole_state, MIRILLA_PEEPHOLE_STATE_ALIVE);

    target_context->file = NULL;

    target_context->start_address = target_context->end_address = 0;

    target_context->interval_subscribe.ops = &mirilla_peephole_mmu_interval_notifier_operations;

    mutex_init(&target_context->install_lock);

    return error_code;
}

MIRILLA_CONTEXT_DESTRUCTOR(map_peephole)
{
    MIRILLA_DEBUG(MIRILLA_LOG_PREFIX_LIFETIME "destruct `map_peephole`");

    if (target_context->interval_subscribe.mm != NULL)
        mmu_interval_notifier_remove(&target_context->interval_subscribe);

    if (target_context->address_space)
        mmdrop(target_context->address_space);

    MIRILLA_DEBUG(MIRILLA_LOG_PREFIX_LIFETIME "defer `kfree`");

    kfree_rcu(target_context, teardown_callback);
}

bool mirilla_map_target_notificate_invalidate_range(struct mmu_interval_notifier *target_subscribe,
                                                    const struct mmu_notifier_range *range,
                                                    unsigned long sequence_count)
{
    struct mirilla_map_peephole_context *peephole_context =
        container_of(target_subscribe, struct mirilla_map_peephole_context, interval_subscribe);

    /*
	 * NOTE(lifetime): A full-interval release prematurely kills the peephole.
	 * Page teardown rides on the ordinary unmap invalidations `exit_mmap`
	 * emits over the range.
	 */
    if (range->event == MMU_NOTIFY_RELEASE) {
        atomic_set_release(&peephole_context->peephole_state, MIRILLA_PEEPHOLE_STATE_DEAD);

        return true;
    }

    /*
	 * NOTE(lock): The seqcount hand-off and the zap below both need the
	 * sleepable `install_lock`. A non-blockable invalidation can take neither,
	 * so bounce it for a blockable retry.
	 */
    if (!mmu_notifier_range_blockable(range))
        return /* NOTE(lock): had to block, could not */ false;

    mutex_lock(&peephole_context->install_lock);

    /*
	 * NOTE(coherence): Advance the sequence under the same lock the fault path
	 * holds across `mmu_interval_read_retry()`. A racing fault either observes
	 * the bump and retries, or has already installed its PTE for the zap below
	 * to tear down.
	 */
    mmu_interval_set_seq(target_subscribe, sequence_count);

    unsigned long range_start = range->start, range_end = range->end;

    /*
	 * NOTE(bounds): Does this event overlap this peephole?
	 */
    if (peephole_context->start_address >= range_end ||
        peephole_context->end_address <= range_start) {
        mutex_unlock(&peephole_context->install_lock);

        return true;
    }

    unsigned long overlap_start = range_start > peephole_context->start_address ?
                                      range_start :
                                      peephole_context->start_address,
                  overlap_end = range_end < peephole_context->end_address ?
                                    range_end :
                                    peephole_context->end_address;

    unsigned long region_start = overlap_start - peephole_context->start_address,
                  region_length = overlap_end - overlap_start;

    MIRILLA_DEBUG("invalidate: range: [0x%lx, 0x%lx) local range: [0x%lx, 0x%lx "
                  "+ 0x%lx)",
                  range_start, range_end, region_start, region_start,
                  region_start + region_length - 1);

    /*
	 * NOTE(coherence): The invalidate callback runs before the page is uninstalled, therefore, we
	 * have to unmap the respective range in the peephole.
	 */
    if (peephole_context->file && peephole_context->file->f_mapping)
        unmap_mapping_range(peephole_context->file->f_mapping, region_start, region_length, 1);

    mutex_unlock(&peephole_context->install_lock);

    return true;
}

vm_fault_t mirilla_map_peephole_vm_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;

    struct mirilla_map_peephole_context *peephole_context = vma->vm_private_data;

    unsigned long relative_address = vmf->pgoff << PAGE_SHIFT;
    unsigned long target_address = peephole_context->start_address + relative_address;

    int page_count = 0;
    struct page *target_page = NULL;

    int mmap_read_locked = true;

    /*
	 * NOTE(coherence): Sampled from the interval notifier to detect an
	 * invalidation that races this fault before a page is installed.
	 */
    unsigned long notifier_seq;

#define MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN                                \
    MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS,                                \
                             "page fault: peephole is dead: 0x%lx address: " \
                             "0x%lx",                                        \
                             relative_address, target_address);
    if (atomic_read_acquire(&peephole_context->peephole_state) == MIRILLA_PEEPHOLE_STATE_DEAD)
        MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN;

    if (target_address >= peephole_context->end_address)
        MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS,
                                 "page fault: relative address not in-bounds: 0x%lx "
                                 "address: "
                                 "0x%lx",
                                 relative_address, target_address);

    notifier_seq = mmu_interval_read_begin(&peephole_context->interval_subscribe);

    /*
	 * NOTE(lock): Self-peephole. The target is the faulting task's own mm,
	 * whose `mmap_lock` we already hold from the outer fault. Re-acquiring it
	 * would recurse, and the unlockable retry in `pin_user_pages_remote()` can
	 * drop then blocking-reacquire it and self-deadlock against a queued
	 * writer. Reuse the held lock and pin with `locked == NULL`.
	 */
    if (peephole_context->address_space == current->mm) {
        /*
		 * NOTE(lock): Under a per-VMA lock we do not hold `mmap_lock`, breaking
		 * the above (and GUP's `mmap_assert_locked()`). Bounce to the
		 * `mmap_lock` path.
		 */
        if (vmf->flags & FAULT_FLAG_VMA_LOCK)
            return VM_FAULT_RETRY;

        /* NOTE(lifetime): Re-check liveness under the held lock. */
        if (atomic_read_acquire(&peephole_context->peephole_state) == MIRILLA_PEEPHOLE_STATE_DEAD)
            MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN;

        page_count = get_user_pages_remote(current->mm, target_address, 1,
                                           /*gup_flags=*/0, &target_page, NULL);
    } else {
        if (!mmget_not_zero(peephole_context->address_space))
            MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS, "page fault: peephole address space is "
                                                      "in teardown");

        if (!mmap_read_trylock(peephole_context->address_space)) {
            mmput(peephole_context->address_space);

            MIRILLA_ERROR_AND_RETURN(VM_FAULT_RETRY, "page fault: peephole foreign address "
                                                     "space lock is unavailable");
        }

        /* NOTE(lifetime): Re-check liveness. */
        if (atomic_read_acquire(&peephole_context->peephole_state) == MIRILLA_PEEPHOLE_STATE_DEAD) {
            mmap_read_unlock(peephole_context->address_space);

            mmput(peephole_context->address_space);

            MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN;
        }

        page_count = get_user_pages_remote(peephole_context->address_space, target_address, 1,
                                           /*gup_flags=*/0, &target_page, &mmap_read_locked);

        if (mmap_read_locked)
            mmap_read_unlock(peephole_context->address_space);

        mmput(peephole_context->address_space);
    }
#undef MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN

    if (page_count <= 0)
        switch (page_count) {
        case -EBUSY:
            if (!mmap_read_locked)
                MIRILLA_ERROR_AND_RETURN(VM_FAULT_RETRY,
                                         "page fault: page at address 0x%lx is "
                                         "busy",
                                         target_address);
            /*
			 * NOTE(lock): Lock still held after `-EBUSY`, so fall through to
			 * the common failure path.
			 */
            fallthrough;
        default:
            MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGSEGV,
                                     "page fault: failed to access page at address "
                                     "0x%lx",
                                     target_address);
        }

    mutex_lock(&peephole_context->install_lock);

    /*
	 * NOTE(coherence): Atomic against the invalidate callback (see the cached
	 * path). On a collision the fresh pin is already superseded, so drop it
	 * and re-fault.
	 */
    if (mmu_interval_read_retry(&peephole_context->interval_subscribe, notifier_seq)) {
        mutex_unlock(&peephole_context->install_lock);

        put_page(target_page);

        return VM_FAULT_NOPAGE;
    }

    {
        vm_fault_t insert_outcome;

        unsigned int reclaim_flags;

        reclaim_flags = memalloc_noreclaim_save();
        insert_outcome = vmf_insert_mixed(vma, vmf->address, page_to_pfn(target_page));
        memalloc_noreclaim_restore(reclaim_flags);

        /*
		 * NOTE(refcount): Put back the transient page reference used to retrieve the PFN.
		 */
        put_page(target_page);

        mutex_unlock(&peephole_context->install_lock);

        return insert_outcome;
    }
}

/*
 * NOTE(populate): Prefault the whole window at `mmap` time rather than on first
 * touch, so that a later read walks resident PTEs instead of paying a fault and
 * a remote pin per granule. This is requested through
 * `MIRILLA_MAP_PEEPHOLE_INITIALIZE_POPULATE`, because it trades a slower `mmap`
 * for that faster steady state and an observer that never reads the whole window
 * would not want it.
 *
 * This deliberately duplicates the pin-and-install of the demand-fault path
 * rather than sharing it. The fault path resolves one racing granule under the
 * interval-notifier sequence, whereas this pass runs under the mapper's held
 * `mmap_lock` and pins `MIRILLA_MAP_PEEPHOLE_POPULATE_ITERATION_SIZE` granules
 * per remote pin for throughput. There is no notifier sequence here. The install
 * is advisory, so a granule that cannot be pinned now, or that a racing
 * invalidation zaps back out, is simply left for the demand-fault path rather
 * than failing the `mmap`. Coherence still rides on the invalidate callback,
 * which zaps installed PTEs through the peephole file mapping.
 */
static void mirilla_map_peephole_populate(struct mirilla_map_peephole_context *peephole_context,
                                          struct vm_area_struct *vma)
{
    struct mm_struct *address_space = peephole_context->address_space;

    unsigned long page_span = (peephole_context->end_address - peephole_context->start_address) >>
                              PAGE_SHIFT;
    unsigned long populated_count;

    /*
	 * NOTE(lock): A self-peephole observes the mapper's own mm, whose `mmap_lock`
	 * we already hold for write from the outer `mmap`. Reuse it and pin with
	 * `locked == NULL`, exactly as the fault path does, rather than recursively
	 * re-acquire it.
	 */
    bool self_observer = address_space == current->mm;

    struct page *page_list[MIRILLA_MAP_PEEPHOLE_POPULATE_ITERATION_SIZE];

    /*
	 * NOTE(refcount): The peephole pins the observed mm by `mm_count`. A remote
	 * pin needs a live `mm_users`, so upgrade the held reference once for the
	 * whole pass rather than reacquire it per iteration. A self-peephole already
	 * runs on `current->mm`, whose users cannot drop under us.
	 */
    if (!self_observer && !mmget_not_zero(address_space))
        return;

    for (populated_count = 0; populated_count < page_span;) {
        unsigned long batch = min(page_span - populated_count,
                                  (unsigned long)MIRILLA_MAP_PEEPHOLE_POPULATE_ITERATION_SIZE);
        unsigned long target_address =
            peephole_context->start_address + (populated_count << PAGE_SHIFT);

        int pinned_count;

        int page_index;

        int mmap_read_locked = true;

        if (atomic_read_acquire(&peephole_context->peephole_state) == MIRILLA_PEEPHOLE_STATE_DEAD)
            break;

        if (self_observer) {
            pinned_count = get_user_pages_remote(current->mm, target_address, batch,
                                                 /*gup_flags=*/0, page_list, NULL);
        } else {
            /*
			 * NOTE(lock): `trylock` because the observed `mmap_lock` orders under
			 * the mapper's held write lock. A contended observer just leaves those
			 * granules for the demand-fault path.
			 */
            if (!mmap_read_trylock(address_space))
                break;

            pinned_count = get_user_pages_remote(address_space, target_address, batch,
                                                 /*gup_flags=*/0, page_list, &mmap_read_locked);

            if (mmap_read_locked)
                mmap_read_unlock(address_space);
        }

        /*
		 * NOTE(best-effort): Any pin failure ends the pass. The remaining
		 * granules resolve on demand.
		 */
        if (pinned_count <= 0)
            break;

        for (page_index = 0; page_index < pinned_count; page_index++) {
            unsigned long install_address =
                vma->vm_start + ((populated_count + page_index) << PAGE_SHIFT);

            unsigned int reclaim_flags = memalloc_noreclaim_save();
            vmf_insert_mixed(vma, install_address, page_to_pfn(page_list[page_index]));
            memalloc_noreclaim_restore(reclaim_flags);

            /* NOTE(refcount): Drop the transient pin taken to read the PFN. */
            put_page(page_list[page_index]);
        }

        populated_count += pinned_count;
    }

    if (!self_observer)
        mmput(address_space);
}

void mirilla_map_peephole_vm_open(struct vm_area_struct *vma)
{
    struct mirilla_map_peephole_context *peephole_context = vma->vm_private_data;

    mirilla_context_map_peephole_reference_get(peephole_context);
}

void mirilla_map_peephole_vm_close(struct vm_area_struct *vma)
{
    struct mirilla_map_peephole_context *peephole_context = vma->vm_private_data;

    mirilla_context_map_peephole_reference_set(peephole_context);

    vma->vm_private_data = NULL;
}

int mirilla_map_peephole_vm_mremap(struct vm_area_struct *vma)
{
    /* NOTE(invariant): Disallow remapping the peephole VMA. */
    return -EPERM;
}

int mirilla_map_peephole_vm_mprotect(struct vm_area_struct *vma, unsigned long start,
                                     unsigned long end, unsigned long newflags)
{
    /* NOTE(invariant): Disallow changing page protections. */
    return -EPERM;
}

int mirilla_map_peephole_file_release(struct inode *ino, struct file *file)
{
    struct mirilla_map_peephole_context *peephole_context = file->private_data;

    /* NOTE(lifetime): This callback can fire from the error path. */
    if (file->private_data) {
        peephole_context->file = NULL;

        mirilla_context_map_peephole_reference_set(peephole_context);
    }

    return 0;
}

int mirilla_map_peephole_file_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct mirilla_map_peephole_context *peephole_context = file->private_data;

    unsigned long peephole_length = peephole_context->end_address - peephole_context->start_address;
    unsigned long vma_length = vma->vm_end - vma->vm_start;

    /* NOTE(invariant): Disallow executable and shared mappings. */
    if (vma->vm_flags & VM_EXEC)
        return -EACCES;
    if (vma->vm_flags & VM_SHARED)
        return -EINVAL;

    /* NOTE(invariant): Must map the entire peephole, no partial mappings. */
    if (vma_length != peephole_length)
        return -EINVAL;
    /* NOTE(invariant): Reject a non-zero file offset. */
    if (vma->vm_pgoff != 0)
        return -EINVAL;

    /*
	 * NOTE(invariant): Reject a self-observing view that overlaps its own
	 * observed range. Such a view can never resolve (GUP refuses `VM_IO`
	 * mappings), and zapping it from the interval notifier would raise a
	 * nested invalidation over the subscribed interval, re-entering the
	 * callback against the held `install_lock`.
	 */
    if (vma->vm_mm == peephole_context->address_space &&
        vma->vm_start < peephole_context->end_address &&
        peephole_context->start_address < vma->vm_end)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "peephole view overlaps its own observed range");

    /* NOTE(refcount): Each VMA holds a peephole reference. */
    mirilla_context_map_peephole_reference_get(peephole_context);

    vma->vm_ops = &mirilla_map_peephole_vm_operations;
    vma->vm_private_data = peephole_context;

    vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_IO);

    /* NOTE(invariant): Clear writable/executable mapping capability. */
    vm_flags_clear(vma, VM_MAYSHARE | VM_MAYWRITE | VM_MAYEXEC);

    /*
	 * NOTE(populate): Honor the one-shot populate word by prefaulting the whole
	 * window before returning, so that the mapping is resident on first read.
	 * The VMA operations and flags are already installed above, which the
	 * install path relies on to pin and insert frames.
	 */
    if (peephole_context->peephole_word & MIRILLA_MAP_PEEPHOLE_INITIALIZE_POPULATE)
        mirilla_map_peephole_populate(peephole_context, vma);

    return 0;
}

/**
 * Determine whether the task is considered legacy.
 *
 * This is largely due to the associated ABI limitations and the ioctl interface.
 */
static inline bool mirilla_task_is_legacy(struct task_struct *task)
{
#ifdef CONFIG_X86_64
    return test_tsk_thread_flag(task, TIF_ADDR32);
#elif defined(CONFIG_ARM64)
    return test_tsk_thread_flag(task, TIF_32BIT);
#else
#error "unsupported architecture"
#endif
}

mirilla_command_status_t
mirilla_map_handle_command_engage(struct mirilla_device_context *device_context,
                                  union mirilla_map_engage_io *io)
{
    struct mirilla_map_engage_argument *argument = &io->argument;
    struct mirilla_map_engage_result *result = &io->result;

    struct pid *target_pid = NULL;

    struct task_struct *target_task = NULL;

    if (!(target_pid = find_get_pid(argument->process_id)))
        MIRILLA_ERROR_AND_RETURN(-ESRCH, "could not find process with pid %d",
                                 argument->process_id);

    /*
	 * NOTE(self): Allow self-engagement when the target pid is the caller's
	 * thread group.
	 */
    if (target_pid != task_tgid(current))
        if (!capable(MIRILLA_MAP_ENGAGE_CAPABILITIES))
            MIRILLA_ERROR_AND_RETURN(-EPERM, "process engage author is not capable");

    if (!(target_task = get_pid_task(target_pid, PIDTYPE_PID))) {
        put_pid(target_pid);

        MIRILLA_ERROR_AND_RETURN(-ESRCH, "could not find respective task");
    }

    if (mirilla_task_is_legacy(target_task)) {
        put_pid(target_pid);

        put_task_struct(target_task);

        MIRILLA_ERROR_AND_RETURN(-ENOTSUPP, "legacy foreign address spaces are not supported");
    }

    /* NOTE(refcount): Release transient ref to `struct task_struct` for legacy process gating. */
    put_task_struct(target_task);

    struct mirilla_map_target_context *target_context = NULL;

    if (mirilla_context_map_target_construct(&target_context)) {
        put_pid(target_pid);

        put_task_struct(target_task);

        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to construct map target context");
    }

    mirilla_map_target_id_t map_target_id = atomic_inc_return(&device_context->map_target_count);

    target_context->id = result->target_id = map_target_id;

    /*
	 * NOTE(refcount): The context now owns this reference.
	 */
    target_context->process_id = target_pid;

    if (xa_insert(&device_context->map_target_list, map_target_id, target_context, GFP_KERNEL)) {
        mirilla_context_map_target_destruct(target_context);

        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to insert to xarray");
    }

    return MIRILLA_COMMAND_OK;
}

mirilla_command_status_t
mirilla_map_handle_command_disengage(struct mirilla_device_context *device_context,
                                     union mirilla_map_disengage_io *io)
{
    struct mirilla_map_disengage_argument *argument = &io->argument;

    struct mirilla_map_target_context *target_context = NULL;

    mirilla_map_target_id_t target_id = argument->target_id;

    {
        xa_lock(&device_context->map_target_list);

        if (!(target_context = xa_load(&device_context->map_target_list, target_id))) {
            xa_unlock(&device_context->map_target_list);

            MIRILLA_ERROR_AND_RETURN(-ENOENT, "map target id is nonexistent");
        }

        /*
		 * NOTE(lock): XArray lock is held.
		 */
        __xa_erase(&device_context->map_target_list, target_id);

        xa_unlock(&device_context->map_target_list);

        mirilla_context_map_target_reference_set(target_context);
    }

    return MIRILLA_COMMAND_OK;
}

mirilla_command_status_t
mirilla_map_handle_command_peephole(struct mirilla_device_context *device_context,
                                    union mirilla_map_peephole_io *io)
{
    struct mirilla_map_peephole_argument *argument = &io->argument;
    struct mirilla_map_peephole_result *result = &io->result;

    struct mirilla_map_target_context *target_context = NULL;
    struct mirilla_map_peephole_context *peephole_context = NULL;

    mirilla_map_target_id_t target_id = argument->target_id;

    struct task_struct *target_task = NULL;
    struct mm_struct *target_space = NULL;

    if (argument->end_address <= argument->start_address)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad peephole address range");

    if (!PAGE_ALIGNED(argument->start_address) || !PAGE_ALIGNED(argument->end_address))
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad peephole address range: not pagesize aligned");

    rcu_read_lock();
    if (!(target_context = xa_load(&device_context->map_target_list, target_id))) {
        rcu_read_unlock();

        MIRILLA_ERROR_AND_RETURN(-ENOENT, "map target id is nonexistent");
    }

    if (!mirilla_context_map_target_reference_get(target_context))
        target_context = NULL;
    rcu_read_unlock();

    if (target_context == NULL)
        MIRILLA_ERROR_AND_RETURN(-ENOENT, "map target id is nonexistent");

    target_task = get_pid_task(target_context->process_id, PIDTYPE_PID);
    if (!target_task) {
        mirilla_context_map_target_reference_set(target_context);

        MIRILLA_ERROR_AND_RETURN(-ESRCH, "map target id has no associated task");
    }

    target_space = get_task_mm(target_task);
    if (!target_space) {
        put_task_struct(target_task);

        mirilla_context_map_target_reference_set(target_context);

        MIRILLA_ERROR_AND_RETURN(-ESRCH, "map target id task has no associated address "
                                         "space");
    }

    /*
	 * NOTE(refcount): Take an `mm_count` ref from the active `mm_users` one.
	 */
    mmgrab(target_space);

    put_task_struct(target_task);

    if (mirilla_context_map_peephole_construct(&peephole_context)) {
        mirilla_context_map_target_reference_set(target_context);

        /*
		 * NOTE(refcount): Release the transient `mm_users` ref, then the `mm_count` ref.
	     */
        mmput(target_space);
        mmdrop(target_space);

        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate peephole context");
    }

    /* NOTE(refcount): The peephole owns the address-space reference. */
    peephole_context->address_space = target_space;

    /*
	 * NOTE(coherence): Publish the range before registering the notifier. The
	 * invalidate callback gates on these fields, so an invalidation racing
	 * registration would otherwise be dropped.
	 */
    peephole_context->start_address = argument->start_address;
    peephole_context->end_address = argument->end_address;

    /**
     * NOTE(invariant): Assign the provided initialization word to the peephole's word.
     */
    peephole_context->peephole_word = argument->initialize_word;

    /*
	 * NOTE(coherence): A private inode gives each peephole its own
	 * `address_space`. A shared anon-inode mapping would let
	 * `unmap_mapping_range` zap unrelated peepholes at the same page offset.
	 */
    struct file *anonymous_file =
        anon_inode_create_getfile("[mirilla-peephole]", &mirilla_map_peephole_file_operations,
                                  peephole_context, MIRILLA_MAP_FILE_FLAGS, NULL);

    if (IS_ERR(anonymous_file)) {
        /*
		 * NOTE(refcount): The file never took ownership, so destruct directly
		 * and drop the `mm_count` ref. Release the transient `mm_users` ref
		 * here.
		 */
        mirilla_context_map_peephole_destruct(peephole_context);
        mirilla_context_map_target_reference_set(target_context);

        mmput(target_space);

        MIRILLA_ERROR_AND_RETURN(PTR_ERR(anonymous_file), "failed to create anonymous "
                                                          "file");
    }

    peephole_context->file = anonymous_file;

    int register_code;

    if ((register_code = mmu_interval_notifier_insert(
             &peephole_context->interval_subscribe, target_space, argument->start_address,
             argument->end_address - argument->start_address,
             &mirilla_peephole_mmu_interval_notifier_operations))) {
        mirilla_context_map_target_reference_set(target_context);

        /*
		 * NOTE(refcount): The anon file now owns the peephole reference. `fput`
		 * runs the release handler, which drops it and destructs the peephole
		 * and releases `mm_count`. Destructing here too would double-free. The
		 * insert failed, so `interval_subscribe.mm` is NULL and the destructor
		 * skips removal.
		 */
        mmput(target_space);
        fput(anonymous_file);

        MIRILLA_ERROR_AND_RETURN(register_code, "failed to register interval-based "
                                                "subscriber");
    }
    /*
	 * NOTE(refcount): Notifier registration required a held `mm_users`, so drop
	 * it now.
	 */
    mmput(target_space);

    int fd = get_unused_fd_flags(MIRILLA_MAP_FILE_FLAGS);

    if (fd < 0) {
        mirilla_context_map_target_reference_set(target_context);

        /*
		 * NOTE(refcount): The anon file owns the peephole reference. `fput`
		 * destructs it, removing the notifier and dropping `mm_count`. The
		 * `mm_users` ref was already released above.
		 */
        fput(anonymous_file);

        MIRILLA_ERROR_AND_RETURN(fd, "failed to allocate file descriptor");
    }

    fd_install(fd, anonymous_file);

    MIRILLA_DEBUG("created peephole context");

    mirilla_id_t peephole_id = atomic_inc_return(&target_context->peephole_count);

    peephole_context->id = result->id = peephole_id;

    result->fd = fd;

    mirilla_context_map_target_reference_set(target_context);

    return MIRILLA_COMMAND_OK;
}

mirilla_command_status_t
mirilla_map_handle_command_address_space_layout(struct mirilla_device_context *device_context,
                                                union mirilla_map_address_space_layout_io *io)
{
    struct mirilla_map_address_space_layout_argument *argument = &io->argument;
    struct mirilla_map_address_space_layout_result *result = &io->result;

    mirilla_map_target_id_t target_id = argument->target_id;
    struct mirilla_outside_list layout_outside_list = argument->layout_list;
    struct mirilla_outside_list auxiliary_vector_outside_list = argument->auxiliary_vector_list;

    struct mirilla_map_target_context *target_context = NULL;

    struct task_struct *target_task = NULL;
    struct mm_struct *target_space = NULL;
    bool target_is_legacy = false;

    struct mirilla_map_address_space_layout *layout_list = NULL;
    struct mirilla_auxiliary_vector_entry *auxiliary_vector_list = NULL;

    uint32_t layout_count = 0, layout_population_count = 0;
    uint32_t auxiliary_vector_count = 0, auxiliary_vector_population_count = 0;

    mirilla_command_status_t command_status = MIRILLA_COMMAND_OK;

    if (layout_outside_list.element_size != sizeof(struct mirilla_map_address_space_layout))
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad address space layout element size");

    if (auxiliary_vector_outside_list.element_size != sizeof(struct mirilla_auxiliary_vector_entry))
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad auxiliary vector element size");

    if (layout_outside_list.list_attribute & ~MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad address space layout list attributes");

    if (auxiliary_vector_outside_list.list_attribute &
        ~MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "bad auxiliary vector list attributes");

    if (!(layout_outside_list.list_attribute & MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE) &&
        !layout_outside_list.list_address)
        MIRILLA_ERROR_AND_RETURN(-EFAULT, "bad address space layout list address");

    if (!(auxiliary_vector_outside_list.list_attribute &
          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE) &&
        !auxiliary_vector_outside_list.list_address)
        MIRILLA_ERROR_AND_RETURN(-EFAULT, "bad auxiliary vector list address");

    MIRILLA_DEBUG("address_space_layout: target_id=%lu layout=%s auxv=%s", (unsigned long)target_id,
                  (layout_outside_list.list_attribute &
                   MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE) ?
                      "dnp" :
                      "populate",
                  (auxiliary_vector_outside_list.list_attribute &
                   MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE) ?
                      "dnp" :
                      "populate");

    rcu_read_lock();
    if (!(target_context = xa_load(&device_context->map_target_list, target_id))) {
        rcu_read_unlock();

        MIRILLA_ERROR_AND_RETURN(-ENOENT, "map target id is nonexistent");
    }

    if (!mirilla_context_map_target_reference_get(target_context))
        target_context = NULL;
    rcu_read_unlock();

    if (!target_context)
        MIRILLA_ERROR_AND_RETURN(-ENOENT, "map target id is no longer available");

    target_task = get_pid_task(target_context->process_id, PIDTYPE_PID);
    if (!target_task) {
        command_status = -ESRCH;
        MIRILLA_ERROR("map target id has no associated task");

        goto release_target;
    }

    down_read(&target_task->signal->exec_update_lock);
    target_space = get_task_mm(target_task);
    target_is_legacy = mirilla_task_is_legacy(target_task);
    up_read(&target_task->signal->exec_update_lock);

    if (!target_space) {
        command_status = -ESRCH;
        MIRILLA_ERROR("map target id task has no associated address space");

        goto release_task;
    }

    MIRILLA_DEBUG("address_space_layout: engaged target_id=%lu legacy=%d", (unsigned long)target_id,
                  target_is_legacy);

    virtual_address_t argument_start, argument_end;
    virtual_address_t environment_start, environment_end;

    spin_lock(&target_space->arg_lock);
    argument_start = target_space->arg_start, argument_end = target_space->arg_end;
    environment_start = target_space->env_start, environment_end = target_space->env_end;
    spin_unlock(&target_space->arg_lock);

    auxiliary_vector_count = 0;
    do {
        auxiliary_vector_count++;
    } while (auxiliary_vector_count < AT_VECTOR_SIZE / 2 &&
             (target_is_legacy ?
                  ((uint32_t *)target_space->saved_auxv)[(auxiliary_vector_count - 1) * 2] :
                  target_space->saved_auxv[(auxiliary_vector_count - 1) * 2]) != AT_NULL);

    if (!(auxiliary_vector_outside_list.list_attribute &
          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE)) {
        auxiliary_vector_population_count =
            min(auxiliary_vector_outside_list.list_size, auxiliary_vector_count);

        if (auxiliary_vector_population_count &&
            !(auxiliary_vector_list = kcalloc(auxiliary_vector_population_count,
                                              sizeof(struct mirilla_auxiliary_vector_entry),
                                              GFP_KERNEL))) {
            command_status = -ENOMEM;
            MIRILLA_ERROR("failed to allocate auxiliary vector list");

            goto release_space;
        }

        for (uint32_t index = 0; index < auxiliary_vector_population_count; index++) {
            if (target_is_legacy) {
                auxiliary_vector_list[index].entry_type =
                    ((uint32_t *)target_space->saved_auxv)[index * 2];
                auxiliary_vector_list[index].entry_value =
                    ((uint32_t *)target_space->saved_auxv)[index * 2 + 1];
            } else {
                auxiliary_vector_list[index].entry_type = target_space->saved_auxv[index * 2];
                auxiliary_vector_list[index].entry_value = target_space->saved_auxv[index * 2 + 1];
            }
        }

        MIRILLA_DEBUG("address_space_layout: auxv count=%u populated=%u", auxiliary_vector_count,
                      auxiliary_vector_population_count);
    }

    mmap_read_lock(target_space);

    layout_count = target_space->map_count;

    if (!(layout_outside_list.list_attribute & MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE)) {
        layout_population_count = min(layout_outside_list.list_size, layout_count);

        if (layout_population_count &&
            !(layout_list = kvcalloc(layout_population_count,
                                     sizeof(struct mirilla_map_address_space_layout),
                                     GFP_KERNEL))) {
            command_status = -ENOMEM;
            MIRILLA_ERROR("failed to allocate address space layout list");

            goto release_map_lock;
        }
    }

    VMA_ITERATOR(iterator, target_space, 0);
    struct vm_area_struct *area;
    uint32_t layout_index = 0;

    for_each_vma(iterator, area)
    {
        if (layout_index < layout_population_count) {
            mirilla_map_layout_attributes_t attribute_list = 0;

            if (area->vm_flags & VM_READ)
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_READ;
            if (area->vm_flags & VM_WRITE)
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_WRITE;
            if (area->vm_flags & VM_EXEC)
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_EXEC;
            if (vma_is_anonymous(area))
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_ANONYMOUS;
            if (area->vm_flags & VM_SHARED)
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_SHARED;
            if (area->vm_flags & VM_GROWSDOWN)
                attribute_list |= MIRILLA_MAP_LAYOUT_ATTRIBUTE_STACK;

            layout_list[layout_index] = (struct mirilla_map_address_space_layout){ area->vm_start,
                                                                                   area->vm_end,
                                                                                   attribute_list };
        }

        layout_index++;
    }

    layout_count = layout_index;

    MIRILLA_DEBUG("address_space_layout: vma count=%u populated=%u", layout_count,
                  layout_population_count);

release_map_lock:
    mmap_read_unlock(target_space);

    if (!MIRILLA_COMMAND_IS_OK(command_status))
        goto release_lists;

    result->metadata = (struct mirilla_map_address_space_metadata){ environment_start,
                                                                    environment_end, argument_start,
                                                                    argument_end };
    result->layout_outcome.total_count = layout_count;
    result->auxiliary_vector_outcome.total_count = auxiliary_vector_count;

    MIRILLA_DEBUG("address_space_layout: metadata env=[0x%lx, 0x%lx) arg=[0x%lx, 0x%lx)",
                  (unsigned long)environment_start, (unsigned long)environment_end,
                  (unsigned long)argument_start, (unsigned long)argument_end);

    if (layout_population_count &&
        copy_to_user((__user void *)layout_outside_list.list_address, layout_list,
                     layout_population_count * sizeof(struct mirilla_map_address_space_layout))) {
        command_status = -EFAULT;
        MIRILLA_ERROR("failed to copy address space layout list");

        goto release_lists;
    }

    if (auxiliary_vector_population_count &&
        copy_to_user(
            (__user void *)auxiliary_vector_outside_list.list_address, auxiliary_vector_list,
            auxiliary_vector_population_count * sizeof(struct mirilla_auxiliary_vector_entry))) {
        command_status = -EFAULT;
        MIRILLA_ERROR("failed to copy auxiliary vector list");
    }

release_lists:
    kfree(auxiliary_vector_list);
    kvfree(layout_list);

release_space:
    /*
     * NOTE(refcount): Drop acquired transient ref for the engaged address space.
     */
    mmput(target_space);

release_task:
    /*
     * NOTE(refcount): Drop acquired transient ref for the task itself.
     */
    put_task_struct(target_task);

release_target:
    /*
     * NOTE(refcount): Drop acquired transient ref for the target context.
     */
    mirilla_context_map_target_reference_set(target_context);

    return command_status;
}

mirilla_command_status_t mirilla_map_handle_command(struct mirilla_device_context *device_context,
                                                    mirilla_command_t command,
                                                    mirilla_command_argument_t argument)
{
    size_t io_size;

    switch (command) {
    case MIRILLA_COMMAND_MAP_ENGAGE:
        io_size = sizeof(union mirilla_map_engage_io);
        break;
    case MIRILLA_COMMAND_MAP_DISENGAGE:
        io_size = sizeof(union mirilla_map_disengage_io);
        break;
    case MIRILLA_COMMAND_MAP_PEEPHOLE:
        io_size = sizeof(union mirilla_map_peephole_io);
        break;
    case MIRILLA_COMMAND_MAP_ADDRESS_SPACE_LAYOUT:
        io_size = sizeof(union mirilla_map_address_space_layout_io);
        break;
    default:
        return -ENOTTY;
    }

    MIRILLA_DEBUG("handling command: %s", MIRILLA_COMMAND_NAME_MAP(command));

    __kernel void *io = NULL;

    if (!(io = kzalloc(io_size, GFP_KERNEL)))
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate for io buffer for command %s",
                                 MIRILLA_COMMAND_NAME_MAP(command));

    __user void *user_structure = (__user void *)argument;

    if (copy_from_user(io, user_structure, io_size)) {
        MIRILLA_ERROR("failed to copy user structure");

        kfree(io);

        return -EFAULT;
    }

    mirilla_command_status_t command_status = MIRILLA_COMMAND_OK;

    switch (command) {
    case MIRILLA_COMMAND_MAP_ENGAGE:
        if (!MIRILLA_COMMAND_IS_OK(command_status =
                                       mirilla_map_handle_command_engage(device_context, io)))
            MIRILLA_ERROR("failed to engage");

        break;
    case MIRILLA_COMMAND_MAP_DISENGAGE:
        if (!MIRILLA_COMMAND_IS_OK(command_status =
                                       mirilla_map_handle_command_disengage(device_context, io)))
            MIRILLA_ERROR("failed to disengage");

        break;
    case MIRILLA_COMMAND_MAP_PEEPHOLE:
        if (!MIRILLA_COMMAND_IS_OK(command_status =
                                       mirilla_map_handle_command_peephole(device_context, io)))
            MIRILLA_ERROR("failed to peephole");

        break;
    case MIRILLA_COMMAND_MAP_ADDRESS_SPACE_LAYOUT:
        if (!MIRILLA_COMMAND_IS_OK(command_status = mirilla_map_handle_command_address_space_layout(
                                       device_context, io)))
            MIRILLA_ERROR("failed to retrieve address space layout information");
        break;
    default:
        unreachable();
    }

    if (MIRILLA_COMMAND_IS_OK(command_status))
        if (copy_to_user(user_structure, io, io_size)) {
            MIRILLA_ERROR("failed to copy result to user");

            command_status = -EFAULT;
        }

    kfree(io);

    return command_status;
}
