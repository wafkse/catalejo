//! Integration tests for the `ADDRESS_SPACE_LAYOUT` command.
//!
//! These tests require the `mirilla` kernel module to be loaded and the
//! current process to have access to `/dev/mirilla`. They are `#[ignore]` by
//! default so `cargo test` does not fail in environments without the module;
//! run them explicitly with `cargo test -- --ignored`.

#![allow(clippy::std_instead_of_core, clippy::std_instead_of_alloc)]

use std::fs::OpenOptions;
use std::io;
use std::os::fd::{AsRawFd, BorrowedFd};

use catalejo_sys::ffi::command;
use catalejo_sys::ffi::lower;
use catalejo_sys::id::{Id, TargetId};

/// Auxiliary vector type constants, as widened by the kernel ABI.
const AT_NULL: u64 = 0;
const AT_PAGESIZE: u64 = 6;

/// Open the `mirilla` device for a test.
fn open_device() -> io::Result<std::fs::File> {
    OpenOptions::new()
        .read(true)
        .write(true)
        .open(&*command::MIRILLA_DEVICE_PATH)
}

/// Self-engage the current process and return the target id.
///
/// # Safety
///
/// The caller must ensure the file descriptor comes from `mirilla`.
unsafe fn self_engage(fd: BorrowedFd) -> io::Result<TargetId> {
    // SAFETY: The caller asserts the descriptor is a `mirilla` device fd.
    unsafe { command::engage(fd, std::process::id() as _) }
}

/// A count-only pass.
///
/// Both lists carry `DO_NOT_POPULATE`, so the kernel reports
/// the full counts without touching any backing buffer.
#[test]
#[ignore = "requires the mirilla kernel module to be loaded"]
fn layout_count_only_reports_nonzero_counts() {
    let device = open_device().expect("failed to open mirilla device");
    let fd = unsafe { BorrowedFd::borrow_raw(device.as_raw_fd()) };

    let target_id = unsafe { self_engage(fd) }.expect("self-engage failed");

    let mut layout_list = catalejo_sys::ffi::binding::mirilla_outside_list {
        list_address: 0,
        list_size: 0,
        element_size: std::mem::size_of::<lower::AddressSpaceLayout>() as u32,
        list_attribute: catalejo_sys::ffi::binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };
    let mut auxiliary_vector_list = catalejo_sys::ffi::binding::mirilla_outside_list {
        list_address: 0,
        list_size: 0,
        element_size: std::mem::size_of::<lower::AuxiliaryVectorEntry>() as u32,
        list_attribute: catalejo_sys::ffi::binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };

    let outcome =
        // SAFETY: The fd is a valid `mirilla` device fd, and both lists carry
        // `DO_NOT_POPULATE`, so no backing buffer is dereferenced.
        unsafe { lower::address_space_layout(fd, target_id, &mut layout_list, &mut auxiliary_vector_list) }
            .expect("count-only layout query failed");

    assert!(outcome.layout_total_count > 0, "layout count is zero");
    assert!(
        outcome.auxiliary_vector_total_count > 0,
        "auxiliary vector count is zero"
    );
    assert!(
        outcome.metadata.argument_end >= outcome.metadata.argument_start,
        "argument range is inverted"
    );
    assert!(
        outcome.metadata.environment_end >= outcome.metadata.environment_start,
        "environment range is inverted"
    );

    // SAFETY: The fd is a valid `mirilla` device fd.
    unsafe { command::disengage(device.into(), target_id) }.expect("disengage failed");
}

/// The high-level wrapper handles the retry mechanism and returns owned vectors
/// holding every kernel-resident entry.
#[test]
#[ignore = "requires the mirilla kernel module to be loaded"]
fn high_level_layout_returns_full_vectors() {
    let device = open_device().expect("failed to open mirilla device");
    let fd = unsafe { BorrowedFd::borrow_raw(device.as_raw_fd()) };

    let target_id = unsafe { self_engage(fd) }.expect("self-engage failed");

    let (metadata, layouts, auxiliary_vector) =
        // SAFETY: The fd is a valid `mirilla` device fd.
        unsafe { command::address_space_layout(fd, target_id) }
            .expect("high-level layout query failed");

    assert!(!layouts.is_empty(), "layout vector is empty");
    assert!(!auxiliary_vector.is_empty(), "auxiliary vector is empty");

    for entry in &layouts {
        assert!(
            entry.start_address < entry.end_address,
            "layout entry has inverted or empty range"
        );
    }

    // The last auxiliary vector entry must be the `AT_NULL` terminator.
    assert_eq!(
        auxiliary_vector.last().unwrap().entry_type,
        AT_NULL,
        "last auxiliary vector entry is not AT_NULL"
    );

    // `AT_PAGESIZE` must be present and match `sysconf(_SC_PAGESIZE)`.
    let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    assert!(page_size > 0, "sysconf(_SC_PAGESIZE) failed");

    let found_page_size = auxiliary_vector
        .iter()
        .any(|entry| entry.entry_type == AT_PAGESIZE && entry.entry_value == page_size as u64);
    assert!(
        found_page_size,
        "AT_PAGESIZE was not found or did not match"
    );

    assert!(
        metadata.argument_end >= metadata.argument_start,
        "argument range is inverted"
    );
    assert!(
        metadata.environment_end >= metadata.environment_start,
        "environment range is inverted"
    );

    // SAFETY: The fd is a valid `mirilla` device fd.
    unsafe { command::disengage(device.into(), target_id) }.expect("disengage failed");
}

/// A nonexistent target must be refused with `ENOENT`.
#[test]
#[ignore = "requires the mirilla kernel module to be loaded"]
fn layout_against_bogus_target_is_rejected() {
    let device = open_device().expect("failed to open mirilla device");
    let fd = unsafe { BorrowedFd::borrow_raw(device.as_raw_fd()) };

    let bogus_target_id: TargetId = Id(std::num::NonZero::new(0xdead_beef).unwrap());

    let mut layout_list = catalejo_sys::ffi::binding::mirilla_outside_list {
        list_address: 0,
        list_size: 0,
        element_size: std::mem::size_of::<lower::AddressSpaceLayout>() as u32,
        list_attribute: catalejo_sys::ffi::binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };
    let mut auxiliary_vector_list = catalejo_sys::ffi::binding::mirilla_outside_list {
        list_address: 0,
        list_size: 0,
        element_size: std::mem::size_of::<lower::AuxiliaryVectorEntry>() as u32,
        list_attribute: catalejo_sys::ffi::binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };

    let outcome =
        // SAFETY: The fd is a valid `mirilla` device fd, and both lists carry
        // `DO_NOT_POPULATE`, so no backing buffer is dereferenced.
        unsafe { lower::address_space_layout(fd, bogus_target_id, &mut layout_list, &mut auxiliary_vector_list) };

    assert!(outcome.is_err(), "layout against a bogus target succeeded");
    let error = outcome.unwrap_err();
    assert_eq!(
        error.raw_os_error(),
        Some(libc::ENOENT),
        "expected ENOENT, got {:?}",
        error
    );
}
