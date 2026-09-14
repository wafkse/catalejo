use std::{fs::OpenOptions, os::fd::AsFd};

use catalejo_sys::exception::{
    Context, InvalidSlabSize, InvalidSoftSlabLimit, SlabAllocationError, SlabSize, SoftSlabLimit,
    backend::Backend,
};

#[test]
fn slab_sizes_preserve_kernel_constraints() {
    assert_eq!(SlabSize::new(0), Err(InvalidSlabSize::Zero));
    assert_eq!(SlabSize::new(4096), Err(InvalidSlabSize::Misaligned));
    assert_eq!(SlabSize::new(12 * 1024), Ok(SlabSize::DEFAULT));
    assert_eq!(SlabSize::DEFAULT.record_capacity(), 256);
    assert_eq!(
        SlabSize::new(2 * 1024 * 1024 + 4096),
        Err(InvalidSlabSize::TooLarge)
    );
}

#[test]
fn soft_limits_preserve_kernel_bound() {
    assert_eq!(SoftSlabLimit::new(0), Err(InvalidSoftSlabLimit::Zero));
    assert_eq!(SoftSlabLimit::new(1), Ok(SoftSlabLimit::DEFAULT));
    assert_eq!(
        SoftSlabLimit::new(17),
        Err(InvalidSoftSlabLimit::AboveKernelLimit)
    );
}

fn device() -> std::fs::File {
    OpenOptions::new()
        .read(true)
        .write(true)
        .open("/dev/mirilla")
        .expect("the Mirilla integration device must open")
}

#[test]
#[ignore = "requires a loaded Mirilla module"]
fn empty_slab_toggles_between_protection_states() {
    let device = device();
    // SAFETY: The descriptor was opened from the Mirilla device.
    let context =
        unsafe { Context::create(device.as_fd(), SlabSize::DEFAULT, SoftSlabLimit::DEFAULT) }
            .expect("the exception context must be created");

    let mut target_slab = context.map().expect("the slab must map");
    assert_eq!(context.allocated(), 1);
    assert!(!target_slab.is_published());
    target_slab.publish().expect("an empty table is valid");
    assert!(target_slab.is_published());
    assert_eq!(target_slab.record_list().len(), 256);
    assert!(target_slab.record_list_mut().is_none());
    target_slab.edit().expect("the slab must return to editing");
    assert!(!target_slab.is_published());
    assert!(target_slab.record_list_mut().is_some());
    drop(target_slab);
    assert_eq!(context.allocated(), 0);
}

#[test]
#[ignore = "requires a loaded Mirilla module"]
fn publication_error_preserves_editable_state() {
    let device = device();
    // SAFETY: The descriptor was opened from the Mirilla device.
    let context =
        unsafe { Context::create(device.as_fd(), SlabSize::DEFAULT, SoftSlabLimit::DEFAULT) }
            .expect("the exception context must be created");
    let mut target_slab = context.map().expect("the slab must map");
    target_slab
        .record_list_mut()
        .expect("a new slab is editable")[0]
        .boundary
        .base_address = 1;

    let error = target_slab
        .publish()
        .expect_err("the incomplete record must fail validation");
    assert_eq!(error.raw_os_error(), Some(libc::EINVAL));
    assert!(!target_slab.is_published());
    target_slab
        .record_list_mut()
        .expect("failed publication preserves editability")[0] =
        // SAFETY: The all-zero record is the ABI-defined unused record value.
        unsafe { core::mem::zeroed() };
    target_slab
        .publish()
        .expect("the repaired empty table is valid");
    drop(target_slab);
    assert_eq!(context.allocated(), 0);
}

#[test]
#[ignore = "requires a loaded Mirilla module"]
fn soft_allocation_limit_is_reusable() {
    let device = device();
    // SAFETY: The descriptor was opened from the Mirilla device.
    let context =
        unsafe { Context::create(device.as_fd(), SlabSize::DEFAULT, SoftSlabLimit::DEFAULT) }
            .expect("the exception context must be created");
    let first = context.map().expect("the first slab must map");
    assert!(matches!(
        context.map(),
        Err(SlabAllocationError::SoftLimitReached)
    ));
    drop(first);
    let replacement = context.map().expect("dropping returns the soft slot");
    drop(replacement);
}

#[test]
#[ignore = "requires a loaded Mirilla module"]
fn sealed_slab_keeps_its_soft_allocation_slot() {
    let device = device();

    // SAFETY: fork creates a child with one calling thread. The child exits without running the
    // inherited Rust destructors after testing a process-permanent sealed mapping.
    let child = unsafe { libc::fork() };
    assert!(child >= 0, "fork must succeed");
    if child == 0 {
        // SAFETY: The descriptor was opened from the Mirilla device.
        let context = match unsafe {
            Context::create(device.as_fd(), SlabSize::DEFAULT, SoftSlabLimit::DEFAULT)
        } {
            Ok(context) => context,
            Err(_) => unsafe { libc::_exit(2) },
        };
        let mut target_slab = match context.map() {
            Ok(target_slab) => target_slab,
            Err(_) => unsafe { libc::_exit(3) },
        };

        if target_slab.publish().is_err() {
            // SAFETY: _exit terminates the isolated child without inherited destructor activity.
            unsafe { libc::_exit(4) };
        }

        let record_pointer = target_slab
            .record_list()
            .as_ptr()
            .cast::<core::ffi::c_void>();
        let slab_size = SlabSize::DEFAULT.get();

        // SAFETY: record_pointer and slab_size identify the exact published slab VMA. A successful
        // seal intentionally makes the mapping process-permanent.
        let seal_status = unsafe { libc::syscall(libc::SYS_mseal, record_pointer, slab_size, 0) };
        if seal_status < 0 {
            let seal_error = std::io::Error::last_os_error();

            match seal_error.raw_os_error() {
                Some(libc::ENOSYS) => unsafe { libc::_exit(0) },
                _ => unsafe { libc::_exit(5) },
            }
        }

        drop(target_slab);
        if context.allocated() != 1 {
            // SAFETY: _exit terminates the isolated child with its sealed mapping.
            unsafe { libc::_exit(6) };
        }

        // SAFETY: _exit releases the process-permanent sealed mapping with the child address space.
        unsafe { libc::_exit(0) };
    }

    let mut child_status = 0;

    // SAFETY: child is the live pid returned by fork and child_status points to local storage.
    assert_eq!(
        unsafe { libc::waitpid(child, &raw mut child_status, 0) },
        child
    );
    assert!(libc::WIFEXITED(child_status));
    assert_eq!(libc::WEXITSTATUS(child_status), 0);
}

#[test]
#[ignore = "requires a loaded Mirilla module"]
fn zz_backend_reuse_and_postfork_rebuild() {
    let device = device();
    // SAFETY: The descriptor was opened from the Mirilla device.
    let left = unsafe { Backend::initialize(device.as_fd()) }.expect("backend initialization");
    let right = Backend::retrieve().expect("backend retrieval");

    assert_eq!(left, right);

    // SAFETY: fork creates a child with one calling thread. The child uses async-signal-safe exit
    // after exercising only the dedicated integration path.
    let child = unsafe { libc::fork() };
    assert!(child >= 0, "fork must succeed");
    if child == 0 {
        let inherited = left
            .validate()
            .expect_err("the inherited proof must be stale");
        if inherited.raw_os_error() != Some(libc::ESTALE) {
            // SAFETY: _exit terminates the fork child without running inherited Rust destructors.
            unsafe { libc::_exit(2) };
        }

        let stale = Backend::retrieve().expect_err("the inherited backend must be stale");
        if stale.raw_os_error() != Some(libc::ESTALE) {
            // SAFETY: _exit terminates the fork child without running inherited Rust destructors.
            unsafe { libc::_exit(3) };
        }

        // SAFETY: The inherited descriptor still belongs to Mirilla and initialization creates a
        // new child context and VM_DONTCOPY slab.
        let child_backend = match unsafe { Backend::initialize(device.as_fd()) } {
            Ok(child_backend) => child_backend,
            Err(_) => {
                // SAFETY: _exit terminates the fork child without running inherited destructors.
                unsafe { libc::_exit(4) };
            }
        };
        if child_backend.validate().is_err() {
            // SAFETY: _exit terminates the fork child without running inherited Rust destructors.
            unsafe { libc::_exit(5) };
        }

        // SAFETY: _exit terminates the fork child without running inherited Rust destructors.
        unsafe { libc::_exit(0) };
    }

    let mut child_status = 0;
    // SAFETY: child is the live pid returned by fork and child_status points to local storage.
    assert_eq!(
        unsafe { libc::waitpid(child, &raw mut child_status, 0) },
        child
    );
    assert!(libc::WIFEXITED(child_status));
    assert_eq!(libc::WEXITSTATUS(child_status), 0);
}
