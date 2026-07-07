/*
 * Mapping subsystem for Mirilla.
 */

#include "asm-generic/errno-base.h"
#include "linux/compiler_attributes.h"
#include "linux/pid.h"
#include "linux/pid_types.h"
#include "linux/sched.h"
#include "linux/sched/signal.h"
#include "linux/sched/task.h"
#include "mirilla-id.h"
#include "mirilla-log.h"
#include "mirilla-map.h"
#include "mirilla-device.h"
#include "mirilla-command.h"

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/rcupdate.h>

/*
 * Declare context-specific reference-counting helper functions.
 */
#define X(context_name)                                                                       \
	MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(map_##context_name)                             \
	{                                                                                     \
		return mirilla_context_reference_get(map_##context_name, context_structure); \
	}
MIRILLA_MAP_CONTEXT_LIST
#undef X

#define X(context_name)                                                                \
	MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(map_##context_name)                      \
	{                                                                              \
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

	atomic64_set(&target_context->peephole_count, 0);

	return error_code;
}

MIRILLA_CONTEXT_DESTRUCTOR(map_target)
{
    MIRILLA_LOG(MIRILLA_LOG_PREFIX_LIFETIME "destruct `map_target`");

    if (target_context->process_id) {
		put_pid(target_context->process_id);

		target_context->process_id = NULL;
	}

	MIRILLA_LOG(MIRILLA_LOG_PREFIX_LIFETIME "defer `kfree`");

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

	target_context->interval_subscribe.ops =
		&mirilla_peephole_mmu_interval_notifier_operations;

	xa_init(&target_context->page_list);

	mutex_init(&target_context->install_lock);

	return error_code;
}

MIRILLA_CONTEXT_DESTRUCTOR(map_peephole)
{
	struct page *pinned_page = NULL;

	unsigned long page_offset;

	MIRILLA_LOG(MIRILLA_LOG_PREFIX_LIFETIME "destruct `map_peephole`");

	if (target_context->interval_subscribe.mm != NULL)
	    mmu_interval_notifier_remove(&target_context->interval_subscribe);

	xa_lock(&target_context->page_list);

	xa_for_each(&target_context->page_list, page_offset, pinned_page)
	{
		__xa_erase(&target_context->page_list, page_offset);

		MIRILLA_LOG(MIRILLA_LOG_PREFIX_LIFETIME "removed cached page at relative address: %lx", page_offset);

		unpin_user_page(pinned_page);
	}

	xa_unlock(&target_context->page_list);

	xa_destroy(&target_context->page_list);

	if (target_context->address_space)
		mmdrop(target_context->address_space);

	MIRILLA_LOG(MIRILLA_LOG_PREFIX_LIFETIME "defer `kfree`");

	kfree_rcu(target_context, teardown_callback);
}

bool mirilla_map_target_notificate_invalidate_range(struct mmu_interval_notifier *target_subscribe,
						     const struct mmu_notifier_range *range,
						     unsigned long sequence_count)
{
	struct mirilla_map_peephole_context *peephole_context = container_of(
		target_subscribe, struct mirilla_map_peephole_context, interval_subscribe);

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
		      region_length = overlap_end - overlap_start,
		      region_end = region_start + region_length - 1;

	MIRILLA_LOG("invalidate: range: [0x%lx, 0x%lx) local range: [0x%lx, 0x%lx "
		     "+ 0x%lx)",
		     range_start, range_end, region_start, region_start, region_end);

	{
		unsigned long page_offset;

		struct page *target_page = NULL;

		xa_lock(&peephole_context->page_list);

		xa_for_each_range(&peephole_context->page_list, page_offset, target_page,
				  region_start, region_end)
		{
			__xa_erase(&peephole_context->page_list, page_offset);

			unpin_user_page(target_page);
		}

		xa_unlock(&peephole_context->page_list);
	}

	if (peephole_context->file && peephole_context->file->f_mapping)
		unmap_mapping_range(peephole_context->file->f_mapping, region_start, region_length,
				    1);

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

#define MIRILLA_VMFAULT_DEAD_ERROR_AND_RETURN                                    \
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

	{
		rcu_read_lock();

		target_page = xa_load(&peephole_context->page_list, relative_address);
		if (target_page)
			if (!get_page_unless_zero(target_page))
				target_page = NULL;

		rcu_read_unlock();
	}

	/* NOTE(fault): Cached hit, insert the page into the VMA. */
	if (target_page) {
		unsigned long pfn = page_to_pfn(target_page);

		vm_fault_t insert_outcome;
		unsigned int reclaim_flags;

		mutex_lock(&peephole_context->install_lock);

		/*
		 * NOTE(coherence): Under `install_lock` this retry is atomic against
		 * the invalidate callback's `mmu_interval_set_seq()`. A collision means
		 * the cached pin is being torn down, so drop the ref and re-fault.
		 */
		if (mmu_interval_read_retry(&peephole_context->interval_subscribe, notifier_seq)) {
			mutex_unlock(&peephole_context->install_lock);

			put_page(target_page);

			return VM_FAULT_NOPAGE;
		}

		/*
		 * NOTE(reclaim): `vmf_insert_mixed()` may allocate a page table under a
		 * reclaiming GFP. Reclaim in our own interval re-enters the invalidate
		 * callback, which blocks on the held lock and self-deadlocks. Fence it.
		 */
		reclaim_flags = memalloc_noreclaim_save();
		insert_outcome = vmf_insert_mixed(vma, vmf->address, pfn);
		memalloc_noreclaim_restore(reclaim_flags);

		mutex_unlock(&peephole_context->install_lock);

		put_page(target_page);

		return insert_outcome;
	}

	if (!mmget_not_zero(peephole_context->address_space))
		MIRILLA_ERROR_AND_RETURN(VM_FAULT_SIGBUS, "page fault: peephole address space is "
							   "in teardown");

	if (!mmap_read_trylock(peephole_context->address_space)) {
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

		unpin_user_page(target_page);

		return VM_FAULT_NOPAGE;
	}

	{
		void *target_value;

		vm_fault_t insert_outcome;
		unsigned int reclaim_flags;

		/*
		 * NOTE(coherence): Publish the pin and install the PTE under the lock.
		 * The invalidate callback either misses both (caught by the retry
		 * above) or, ordered after us, finds the xarray entry to unpin and the
		 * PTE to zap. `GFP_NOWAIT` and the reclaim fence keep the store out of
		 * reclaim, which would re-enter our invalidate callback on `install_lock`.
		 */
		target_value = xa_store(&peephole_context->page_list, relative_address, target_page,
					GFP_NOWAIT);

		if (xa_is_err(target_value)) {
			mutex_unlock(&peephole_context->install_lock);

			unpin_user_page(target_page);

			MIRILLA_ERROR_AND_RETURN(VM_FAULT_OOM,
						  "page fault: failed to track page at address "
						  "0x%lx",
						  target_address);
		}

		if (target_value)
			unpin_user_page(target_value);

		reclaim_flags = memalloc_noreclaim_save();
		insert_outcome = vmf_insert_mixed(vma, vmf->address, page_to_pfn(target_page));
		memalloc_noreclaim_restore(reclaim_flags);

		mutex_unlock(&peephole_context->install_lock);

		return insert_outcome;
	}
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

	unsigned long peephole_length =
		peephole_context->end_address - peephole_context->start_address;
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


	/* NOTE(refcount): Each VMA holds a peephole reference. */
	mirilla_context_map_peephole_reference_get(peephole_context);

	vma->vm_ops = &mirilla_map_peephole_vm_operations;
	vma->vm_private_data = peephole_context;

	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_IO);

	/* NOTE(invariant): Clear writable/executable mapping capability. */
	vm_flags_clear(vma, VM_MAYSHARE | VM_MAYWRITE | VM_MAYEXEC);

	return 0;
}
mirilla_command_status_t
mirilla_map_handle_command_engage(struct mirilla_device_context *device_context,
				   union mirilla_map_engage_io *io)
{
	struct mirilla_map_engage_argument *argument = &io->argument;
	struct mirilla_map_engage_result *result = &io->result;

	struct pid *target_pid = NULL;

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

	struct mirilla_map_target_context *target_context = NULL;

	if (mirilla_context_map_target_construct(&target_context)) {
		put_pid(target_pid);

		MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to construct map target context");
	}

	mirilla_map_target_id_t map_target_id =
		atomic64_inc_return(&device_context->map_target_count);

	target_context->id = result->target_id = map_target_id;

	/*
	 * NOTE(refcount): The context now owns this reference.
	 */
	target_context->process_id = target_pid;

	if (xa_insert(&device_context->map_target_list, map_target_id, target_context,
		      GFP_KERNEL)) {
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

	/*
	 * NOTE(coherence): A private inode gives each peephole its own
	 * `address_space`. A shared anon-inode mapping would let
	 * `unmap_mapping_range` zap unrelated peepholes at the same page offset.
	 */
	struct file *anonymous_file = anon_inode_create_getfile(
		"[mirilla-peephole]", &mirilla_map_peephole_file_operations,
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
		     &peephole_context->interval_subscribe, target_space,
		     argument->start_address,
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

	MIRILLA_LOG("created peephole context");

	mirilla_id_t peephole_id = atomic64_inc_return(&target_context->peephole_count);

	peephole_context->id = result->id = peephole_id;

	result->fd = fd;

	mirilla_context_map_target_reference_set(target_context);

	return MIRILLA_COMMAND_OK;
}

mirilla_command_status_t
mirilla_map_handle_command(struct mirilla_device_context *device_context,
			    mirilla_command_t command, mirilla_command_argument_t argument)
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
	default:
		return -ENOTTY;
	}

	MIRILLA_LOG("handling command: %s", MIRILLA_COMMAND_NAME_MAP(command));

	__kernel void *io = NULL;

	if (!(io = kzalloc(io_size, GFP_KERNEL)))
		MIRILLA_ERROR_AND_RETURN(-ENOMEM,
					  "failed to allocate for io buffer for command %s",
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
		if (!MIRILLA_COMMAND_IS_OK(command_status = mirilla_map_handle_command_engage(
						    device_context, io)))
			MIRILLA_ERROR("failed to engage");

		break;
	case MIRILLA_COMMAND_MAP_DISENGAGE:
		if (!MIRILLA_COMMAND_IS_OK(command_status = mirilla_map_handle_command_disengage(
						    device_context, io)))
			MIRILLA_ERROR("failed to disengage");

		break;
	case MIRILLA_COMMAND_MAP_PEEPHOLE:
		if (!MIRILLA_COMMAND_IS_OK(command_status = mirilla_map_handle_command_peephole(
						    device_context, io)))
			MIRILLA_ERROR("failed to peephole");

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
