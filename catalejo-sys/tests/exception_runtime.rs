use core::{mem::size_of, time::Duration};
use std::{
    fs::OpenOptions,
    os::fd::{AsFd, OwnedFd},
};

use catalejo_sys::{
    exception::Image,
    ffi::binding,
    monitor::{self, MonitorError},
};

fn resolve(target_field: *const binding::virtual_relative_t) -> usize {
    let target_field_address = target_field.expose_provenance();

    // SAFETY:
    // The field belongs to a live immutable runtime record.
    let target_displacement = unsafe { target_field.read() } as usize;

    target_field_address.wrapping_add(target_displacement)
}

fn registered_image() -> (OwnedFd, &'static Image) {
    let target_file = OpenOptions::new()
        .read(true)
        .write(true)
        .open("/dev/mirilla")
        .expect("the Mirilla test device must open");
    let target_device = OwnedFd::from(target_file);

    // SAFETY:
    // The descriptor was opened from Mirilla. C retains a dedicated registration session.
    let image = unsafe { Image::register(target_device.as_fd()) }
        .expect("the exception image must initialize and register");

    (target_device, image)
}

#[test]
#[ignore = "requires a Mirilla-registered exception image"]
fn image_records_resolve_inside_the_rollback_region() {
    let (_target_registration, image) = registered_image();
    let rollback_region = image.rollback_region();
    let except_table = image.except_table();
    let rollback_start = rollback_region.address() as usize;
    let rollback_end = rollback_start + rollback_region.size() as usize;
    let record_capacity =
        except_table.size() as usize / size_of::<binding::mirilla_except_record>();
    let table = except_table.address() as *const binding::mirilla_except_record;
    let mut record_seen = 0;

    for target_index in 0..record_capacity {
        // SAFETY:
        // The image proof covers the complete immutable table VMA.
        let target_record = unsafe { &*table.add(target_index) };
        let record_empty = target_record.start_address == 0
            && target_record.end_address == 0
            && target_record.rollback_address == 0
            && target_record.except_mask == 0;

        if record_empty {
            continue;
        }

        record_seen += 1;
        let target_start = resolve(&raw const target_record.start_address);
        let target_end = resolve(&raw const target_record.end_address);
        let target_rollback = resolve(&raw const target_record.rollback_address);

        assert!((rollback_start..rollback_end).contains(&target_start));
        assert!((target_start + 1..=rollback_end).contains(&target_end));
        assert!((rollback_start..rollback_end).contains(&target_rollback));
    }

    assert!(
        record_seen > 0,
        "the image must carry protected instructions"
    );
}

#[test]
#[ignore = "requires a Mirilla-registered exception image"]
fn image_regions_reject_permission_changes() {
    let (_target_registration, image) = registered_image();

    // SAFETY:
    // This attempts to change permissions on the exact sealed rollback region.
    let status = unsafe {
        libc::mprotect(
            image.rollback_region().address() as *mut libc::c_void,
            image.rollback_region().size() as usize,
            libc::PROT_READ,
        )
    };

    assert_eq!(status, -1, "the rollback region must reject mprotect");
    assert_eq!(
        std::io::Error::last_os_error().raw_os_error(),
        Some(libc::EPERM),
    );
}

#[test]
#[ignore = "requires a Mirilla-registered exception image"]
fn monitor_selection_and_wait_are_runtime_safe() {
    let (_target_registration, image) = registered_image();
    let target_value = 0_u64;

    // SAFETY:
    // The address names a live aligned local word and the image registration remains live.
    let target_arm = unsafe { monitor::arm(image, (&raw const target_value).cast::<u8>()) };

    match target_arm {
        Ok(target_backend) => {
            assert_eq!(monitor::backend(), Some(target_backend));
            let target_start = std::time::Instant::now();

            // SAFETY:
            // The backend was armed on this thread and the image registration remains live.
            let target_wait = unsafe { monitor::wait(image, target_backend) };

            assert!(
                target_wait.is_ok() || target_wait == Err(MonitorError::Unsupported),
                "a protected hardware wait must complete or downgrade",
            );
            assert!(
                target_start.elapsed() < Duration::from_secs(1),
                "one hardware wait interval must remain finite",
            );
        }
        Err(MonitorError::Unsupported) => assert_eq!(monitor::backend(), None),
        Err(MonitorError::Fault) => {
            panic!("arming a live local word must not report a mapping fault");
        }
    }
}

#[test]
fn sigill_outside_the_fault_section_is_chained() {
    use std::os::unix::process::ExitStatusExt;

    const CHILD_VARIABLE: &str = "CATALEJO_SIGILL_CHILD";

    if std::env::var_os(CHILD_VARIABLE).is_some() {
        // SAFETY:
        // The child intentionally raises an unregistered SIGILL.
        unsafe { libc::raise(libc::SIGILL) };

        panic!("the default SIGILL action must terminate the child");
    }

    let target_executable =
        std::env::current_exe().expect("the test executable path must be available");
    let target_status = std::process::Command::new(target_executable)
        .args(["--exact", "sigill_outside_the_fault_section_is_chained"])
        .env(CHILD_VARIABLE, "1")
        .status()
        .expect("the SIGILL child must run");

    assert_eq!(
        target_status.signal(),
        Some(libc::SIGILL),
        "SIGILL must retain its default behavior",
    );
}
