//! The Foreign-Function-Interface module for `catalejo-fault`.
//!
//! This binds the C side of the crate, which owns the sealed accessor image.

use core::{
    mem::{self, MaybeUninit},
    num::NonZeroI32,
    ptr,
};

use std::sync::OnceLock;

#[cfg(all(feature = "stealth-mode", not(test)))]
use core::hint;

use catalejo_memory::primitive::PrimitiveUnion;

use crate::behavior::Faultable;

pub use catalejo_sys::ffi::Image;

pub mod binding {
    #![allow(
        nonstandard_style,
        missing_docs,
        reason = "bindgen-generated bindings have largely non-standard style and missing documentation"
    )]
    //! Bare automatically-generated bindings to the C-based subsystem.

    // NOTE: Include the `bindgen`-generated bindings for our own crate.
    include!(concat!(env!("OUT_DIR"), "/catalejo-binding.rs"));
}

/// Register the sealed image on the default Mirilla device for a device-backed unit test.
#[cfg(test)]
pub fn register_test_image() -> std::os::fd::OwnedFd {
    use std::os::fd::{BorrowedFd, FromRawFd, OwnedFd};

    let target_image = image().expect("the sealed accessor image must initialize");
    let device_path = b"/dev/mirilla\0";

    // SAFETY: The byte string is nul-terminated and remains live for the call.
    let target_fd = unsafe {
        libc::open(
            device_path.as_ptr().cast::<libc::c_char>(),
            libc::O_RDWR | libc::O_CLOEXEC,
        )
    };
    assert!(target_fd >= 0, "the Mirilla test device must open");

    // SAFETY: The descriptor was just opened from the Mirilla device path and remains live for
    // the registration call. `target_image` carries the immutable image proof.
    let target_status = unsafe {
        catalejo_sys::ffi::command::register_exception_image(
            BorrowedFd::borrow_raw(target_fd),
            target_image,
        )
    };
    if let Err(target_error) = target_status {
        // SAFETY: The descriptor remains owned locally because registration failed.
        unsafe { libc::close(target_fd) };
        panic!("the exception image must register: {target_error}");
    }

    // SAFETY: The successful `open` returned a uniquely owned descriptor.
    unsafe { OwnedFd::from_raw_fd(target_fd) }
}

pub mod lower {
    //! Low-level and plumbing structures and functions towards the Foreign-Function-Interface boundary.

    use catalejo_memory::primitive::{Primitive, PrimitiveUnion};

    use crate::ffi::binding;

    /// Perform a bare-bones read via the C-implemented shims.
    ///
    /// # Safety
    ///
    /// This has the same safety constraints as an individual `binding::catalejo_read_uN` operation, where `N` is the size of the primitive read.
    #[inline]
    pub unsafe fn read(
        target_source: *const PrimitiveUnion,
        target_value: *mut PrimitiveUnion,
        target_type: Primitive,
    ) -> binding::catalejo_faultable_outcome_t {
        macro_rules! implement {
            ($target_type:ident) => {
                tokel::stream!(
                    [< binding::catalejo_read _ $target_type >]:concatenate (target_source.cast::<$target_type>(), target_value.cast::<$target_type>())
                )
            };
        }

        // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
        unsafe {
            match target_type {
                Primitive::U8 => implement!(u8),
                Primitive::U16 => implement!(u16),
                Primitive::U32 => implement!(u32),
                Primitive::U64 => implement!(u64),
            }
        }
    }

    /// Perform a bare-bones write via the C-implemented shims.
    ///
    /// # Safety
    ///
    /// This has the same safety constraints as an individual `binding::catalejo_write_uN` operation, where `N` is the size of the primitive written.
    #[inline]
    pub unsafe fn write(
        target_value: *mut PrimitiveUnion,
        target_source: *const PrimitiveUnion,
        target_type: Primitive,
    ) -> binding::catalejo_faultable_outcome_t {
        macro_rules! implement {
            ($target_type:ident) => {
                tokel::stream!(
                    [< binding::catalejo_write _ $target_type >]:concatenate (target_value.cast::<$target_type>(), target_source.cast::<$target_type>())
                )
            };
        }

        // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
        unsafe {
            match target_type {
                Primitive::U8 => implement!(u8),
                Primitive::U16 => implement!(u16),
                Primitive::U32 => implement!(u32),
                Primitive::U64 => implement!(u64),
            }
        }
    }

    /// Perform a bulk memory copy via the C-implemented shim.
    ///
    /// # Safety
    ///
    /// This has the same safety constraints as [`binding::catalejo_copy`].
    #[inline]
    pub unsafe fn copy(
        target_address: *mut u8,
        target_source: *const u8,
        target_count: usize,
    ) -> binding::catalejo_faultable_copy_outcome_t {
        // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
        unsafe { binding::catalejo_copy(target_address, target_source, target_count) }
    }
}

/// Failure to construct the sealed accessor image.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct ImageError(NonZeroI32);

impl ImageError {
    /// Return the positive platform error number reported during construction.
    #[inline]
    pub const fn code(self) -> NonZeroI32 {
        let Self(code) = self;

        code
    }
}

/// Process-global immutable accessor image.
///
/// The image is published only after the C-side builder has completed relocation, backing-file
/// sealing, final VMA mapping, and `mseal()` on both VMAs.
pub static IMAGE: OnceLock<Image> = OnceLock::new();

/// Construct or retrieve the process-global immutable accessor image.
#[inline]
pub fn image() -> Result<&'static Image, ImageError> {
    if let Some(target_image) = IMAGE.get() {
        return Ok(target_image);
    }

    let target_image = initialize_image()?;
    let _ = IMAGE.set(target_image);

    Ok(IMAGE
        .get()
        .expect("the image was initialized or published concurrently"))
}

/// Construct the immutable accessor image and wrap its ranges in the system-level proof type.
fn initialize_image() -> Result<Image, ImageError> {
    let mut image = MaybeUninit::<binding::mirilla_except_image>::uninit();

    // SAFETY: The C function accepts a writable image output and initializes it on success.
    let status = unsafe { binding::catalejo_fault_image_initialize(image.as_mut_ptr()) };

    match NonZeroI32::new(status) {
        None => {
            // SAFETY: A zero status guarantees that the output image was initialized.
            let image = unsafe { image.assume_init() };
            let image = catalejo_sys::ffi::binding::mirilla_except_image {
                accessor_address: image.accessor_address,
                accessor_length: image.accessor_length,
                table_address: image.table_address,
                table_length: image.table_length,
            };

            // SAFETY: The C image builder publishes these ranges only after completing all
            // backing-object and VMA sealing invariants required by `Image`.
            Ok(unsafe { Image::from_raw(image) })
        }
        Some(status) => {
            let code = NonZeroI32::new(status.get().saturating_abs()).unwrap_or(NonZeroI32::MAX);

            Err(ImageError(code))
        }
    }
}

/// A hardware implementation for monitoring an address.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorBackend {
    /// Intel user monitor and user wait instructions.
    IntelUmonitor,

    /// AMD extended monitor and extended wait instructions.
    AmdMonitorx,
}

/// A failure while arming or waiting with a hardware monitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorError {
    /// The monitored local address faulted while it was being armed.
    Fault,

    /// The selected optional instruction is unavailable at runtime.
    Unsupported,
}

/// Determine the runtime selected hardware monitor implementation.
///
/// A [`None`] result requests a polling fallback from the caller.
#[inline]
pub fn monitor_backend() -> Option<MonitorBackend> {
    // SAFETY
    //
    // The caller's Mirilla session registration protects optional instruction faults.
    let target_backend = unsafe { binding::catalejo_monitor_select() };

    match target_backend {
        binding::CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR => Some(MonitorBackend::IntelUmonitor),
        binding::CATALEJO_MONITOR_BACKEND_AMD_MONITORX => Some(MonitorBackend::AmdMonitorx),
        binding::CATALEJO_MONITOR_BACKEND_UNSUPPORTED => None,
        // NOTE: Any other value is impossible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Arm the runtime selected hardware monitor for an address.
///
/// The returned backend identifies the instruction family that was armed. An unsupported
/// instruction is detected through the protected `SIGILL` path and requests polling.
///
/// # Safety
///
/// * The address must be non-null and must name standard local memory reached under exposed
///   provenance. It must be the downstream address whose cache activity is to be observed.
///
/// * The mapping that contains the address must remain alive until the matching [`monitor_wait`]
///   call has completed. A mapping failure while arming is caught and reported.
///
/// * The caller must invoke [`monitor_wait`] on the same thread if this function succeeds.
///
/// * The current address space and accessor image must be registered on a live Mirilla device
///   session before the instruction begins.
#[inline]
pub unsafe fn monitor_arm(target_address: *const u8) -> Result<MonitorBackend, MonitorError> {
    let target_outcome =
        // SAFETY
        //
        // The address and monitor lifetime requirements are delegated to the caller.
        unsafe { binding::catalejo_monitor_arm(target_address) };

    match target_outcome {
        binding::CATALEJO_MONITOR_ARM_INTEL_UMONITOR => Ok(MonitorBackend::IntelUmonitor),
        binding::CATALEJO_MONITOR_ARM_AMD_MONITORX => Ok(MonitorBackend::AmdMonitorx),
        binding::CATALEJO_MONITOR_ARM_FAULT => Err(MonitorError::Fault),
        binding::CATALEJO_MONITOR_ARM_UNSUPPORTED => Err(MonitorError::Unsupported),
        // NOTE: Any other value is impossible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Wait for one bounded interval with an armed hardware monitor.
///
/// Every successful call has a finite hardware deadline. The caller should repeat arm and wait
/// operations while checking its own application deadline and observed value.
///
/// # Safety
///
/// * The backend must have been returned by the immediately preceding successful [`monitor_arm`]
///   call on this thread.
///
/// * The monitored mapping must remain alive for the duration of this call.
///
/// * The current address space and accessor image must remain registered on a live Mirilla device
///   session for the duration of this call.
#[inline]
pub unsafe fn monitor_wait(target_backend: MonitorBackend) -> Result<(), MonitorError> {
    let target_backend = match target_backend {
        MonitorBackend::IntelUmonitor => binding::CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR,
        MonitorBackend::AmdMonitorx => binding::CATALEJO_MONITOR_BACKEND_AMD_MONITORX,
    };

    let target_outcome =
        // SAFETY
        //
        // The same-thread arm and mapping lifetime requirements are delegated to the caller.
        unsafe { binding::catalejo_monitor_wait(target_backend) };

    match target_outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => Ok(()),
        binding::CATALEJO_OUTCOME_ERROR => Err(MonitorError::Fault),
        binding::CATALEJO_OUTCOME_INVALID_VALUE => Err(MonitorError::Unsupported),
        // NOTE: Any other value is impossible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Perform a primitive-level fault-tolerant read of a memory address.
///
/// This function attempts to read from the target address using a memory-coherent hardware fetch.
/// If the underlying physical page is unmapped, Mirilla redirects the fault to the image recovery
/// path and the function returns `None`.
///
/// # Safety
///
/// * The address names a `Faultable`-typed value, so every possible bit-pattern is valid.
///   If the page is physically mapped but logically freed, this safely returns garbage bytes.
///
/// * The address is non-null and accessible under *exposed* provenance. This covers memory outside
///   the Rust Abstract Machine, such as an MMU-adjudicated foreign mapping disjoint from the
///   abstract machine's stack, heap, and statics, as well as memory whose provenance was previously
///   exposed. The access is carried out by hand-written assembly, never as an abstract-machine load.
///   Its liveness is adjudicated by the hardware MMU, and a fault is caught and reported instead of
///   being undefined behavior.
///
/// * The address is properly aligned to maintain machine-word and snapshot coherence. Misaligned
///   reads crossing cache-lines or pages lose hardware atomicity.
///
/// * The address targets standard memory. It must not point to memory-mapped
///   I/O hardware registers where a speculative read could trigger a device-level side-effect.
///
/// * The current address space and accessor image must be registered on a live Mirilla device
///   session before the access begins.
#[inline]
pub unsafe fn read<F>(target_address: *mut F) -> Option<F>
where
    F: Faultable,
{
    let mut target_value =
        // SAFETY: The `PrimitiveUnion` type is composed of integer primitives, so this is safe.
        unsafe { MaybeUninit::<PrimitiveUnion>::zeroed().assume_init() };

    // SAFETY:
    // * The address names a `Faultable`-typed value, so every bit-pattern is valid.
    //
    // * The pointer carries exposed provenance over outside-abstract-machine memory and is
    //   accessed by assembly; Mirilla redirects a resulting fault to the registered recovery path.
    //
    // * The address is properly aligned to maintain machine-word coherence.
    let target_outcome = unsafe {
        lower::read(
            target_address.cast::<PrimitiveUnion>(),
            ptr::from_mut(&mut target_value),
            F::PRIMITIVE,
        )
    };

    match target_outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => Some(
            // SAFETY: The `F` implements `Faultable`, which guarantees the soundness of this transmute.
            //
            // The transmute is mixed in size, but it is valid as the respective union houses up to the biggest primitive type.
            unsafe { mem::transmute_copy::<PrimitiveUnion, F>(&target_value) },
        ),
        binding::CATALEJO_OUTCOME_ERROR => None,
        // NOTE: No other such result may be possible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Perform a primitive-level fault-tolerant write to a memory address.
///
/// This function attempts to write to the target address using a memory-coherent hardware access.
/// If the underlying physical page is unmapped, Mirilla redirects the fault to the image recovery
/// path and the function returns `false`. A successful write returns `true`.
///
/// # Safety
///
/// This function has the same safety constraints as the [`read`] function, however, there are some additions:
///
/// * The address must also name memory that is permitted to be written; otherwise the store faults
///   and is reported as `false`.
/// * The store must not race, in the Rust sense, with another thread of the *current* process for
///   the same address. A concurrent mutation by a foreign process is not such a race: nothing
///   aliases the address as a reference, and the store is an indivisible hardware access.
#[inline]
pub unsafe fn write<F>(target_address: *mut F, target_value: F) -> bool
where
    F: Faultable,
{
    let target_source = &raw const target_value;

    // SAFETY:
    // * The address names a `Faultable`-typed value, so every bit-pattern is valid.
    //
    // * The pointer carries exposed provenance over outside-abstract-machine memory and is
    //   accessed by assembly; Mirilla redirects a resulting fault to the registered recovery path.
    //
    // * The address is properly aligned to maintain machine-word coherence.
    let target_outcome = unsafe {
        lower::write(
            target_address.cast::<PrimitiveUnion>(),
            target_source.cast_mut().cast::<PrimitiveUnion>(),
            F::PRIMITIVE,
        )
    };

    match target_outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => true,
        binding::CATALEJO_OUTCOME_ERROR => false,
        // NOTE: No other such result may be possible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Perform a fault-protected copy from the target source to the target address.
///
/// This function attempts to copy `target_count` bytes as a byte-granular ascending stream. If
/// the underlying physical page of the fault-adjudicated side is unmapped, the hardware exception
/// is redirected by Mirilla and the function returns `Err` carrying the count of
/// bytes left uncopied at the faulting byte, the target already holds the copied prefix of
/// `target_count` minus that count, and the rest of the target is untouched.
///
/// # Safety
///
/// The copy is dual-use, as it mirrors foreign memory in through a faulting source, or publishes
/// into it through a faulting target, so the constraints below attach to *sides*, not to the
/// call, and swap with the direction.
///
/// * Exactly one side names memory outside the Rust Abstract Machine, such as an MMU-adjudicated
///   foreign mapping. That side must be non-null and accessible under *exposed* provenance. The
///   access is carried out by hand-written assembly, never as an abstract-machine access. Its
///   liveness is adjudicated by the hardware MMU, and a fault is caught and reported instead of
///   being undefined behavior. A copy between two fault-adjudicated ranges is unsupported.
///
/// * The other side names ordinary abstract-machine memory, and Mirilla cannot rescue it.
///   it must be valid for the whole `target_count` range under the ordinary rules, it is readable
///   when it is the source, writable when it is the target. A bogus abstract-machine range does not
///   reliably fault, as it aliases live allocations, and the copy corrupts them.
///
/// * A fault-adjudicated *target* must additionally name memory that is permitted to be written,
///   otherwise the store faults and is reported as `Err`. Its stores must not race, in the Rust
///   sense, with another thread of the *current* process for the same range. A concurrent
///   mutation by a foreign process is not such a race, as nothing aliases the range as a reference.
///   Unlike the primitive [`write()`], the stores are not indivisible, so a foreign reader may
///   observe a torn prefix.
///
/// * The ranges must not overlap. The stream advances byte-wise ascending, so an overlap re-reads
///   bytes the copy itself just wrote, duplicating them instead of moving them.
///
/// * No alignment is demanded, the stream is byte-granular, so it carries no machine-word nor snapshot
///   coherence, and a caught fault leaves the copied prefix visible. However, alignment of the target
///   address is required to avoid slowdown of ERMS (Enhanced REP MOVSB).
///
/// * Neither side may target memory-mapped I/O hardware registers where a speculative access
///   could trigger a device-level side-effect.
///
/// * The current address space and accessor image must be registered on a live Mirilla device
///   session before the copy begins.
#[inline]
pub unsafe fn copy(
    target_address: *mut u8,
    target_source: *const u8,
    target_count: usize,
) -> Result<(), usize> {
    // SAFETY:
    // * The fault-adjudicated side carries exposed provenance over outside-abstract-machine
    //   memory and is accessed by assembly, so Mirilla redirects a resulting fault to recovery.
    //
    // * The abstract-machine side is valid for the whole range, as asserted by the caller.
    let binding::catalejo_faultable_copy_outcome {
        outcome_status,
        byte_count,
    } = unsafe { lower::copy(target_address, target_source, target_count) };

    match outcome_status {
        binding::CATALEJO_OUTCOME_SUCCESS => Ok(()),
        // NOTE: On a fault the remaining count at the faulting byte is reported back.
        binding::CATALEJO_OUTCOME_ERROR => Err(byte_count),
        // NOTE: No other such result may be possible.
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: This is impossible, hence unreachable.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

#[cfg(test)]
mod test {
    use core::time::Duration;

    use super::{
        MonitorError, binding, image, monitor_arm, monitor_backend, monitor_wait,
        register_test_image,
    };

    /// Resolve one signed field-relative address with the ABI's wrapping representation.
    fn resolve(target_field: *const binding::virtual_relative_t) -> usize {
        let target_field_address = target_field.expose_provenance();

        // SAFETY: The field is borrowed from a live immutable runtime record.
        let target_displacement = unsafe { target_field.read() } as usize;

        target_field_address.wrapping_add(target_displacement)
    }

    #[test]
    fn image_records_resolve_inside_the_accessor_mapping() {
        let image = image().expect("the sealed accessor image must initialize");
        let accessor_start = image.accessor_address() as usize;
        let accessor_end = accessor_start + image.accessor_length() as usize;
        let table_header = image.table_address() as *const binding::mirilla_except_table_header;
        // SAFETY: The image proof covers the complete immutable table VMA.
        let record_count = unsafe { (*table_header).record_count as usize };
        let table = unsafe { table_header.add(1) }.cast::<binding::mirilla_except_record>();

        assert!(
            record_count > 0,
            "the image must carry protected instructions"
        );

        for target_index in 0..record_count {
            // SAFETY: The image proves that `record_count` records occupy the immutable table.
            let target_record = unsafe { &*table.add(target_index) };
            let target_start = resolve(&raw const target_record.start_address);
            let target_end = resolve(&raw const target_record.end_address);
            let target_fixup = resolve(&raw const target_record.fixup_address);

            assert!((accessor_start..accessor_end).contains(&target_start));
            assert!((target_start + 1..=accessor_end).contains(&target_end));
            assert!((accessor_start..accessor_end).contains(&target_fixup));
        }
    }

    #[test]
    fn image_vmas_reject_permission_changes() {
        let image = image().expect("the sealed accessor image must initialize");

        // SAFETY: This attempts to reduce permissions on the exact live accessor VMA. `mseal`
        // rejects the operation and therefore leaves the process-global image unchanged.
        let status = unsafe {
            libc::mprotect(
                image.accessor_address() as *mut libc::c_void,
                image.accessor_length() as usize,
                libc::PROT_READ,
            )
        };

        assert_eq!(status, -1, "the accessor VMA must reject mprotect");
        assert_eq!(
            std::io::Error::last_os_error().raw_os_error(),
            Some(libc::EPERM),
        );
    }

    #[test]
    #[ignore = "requires a Mirilla-registered exception image"]
    fn monitor_selection_and_wait_are_runtime_safe() {
        let _target_registration = register_test_image();
        let target_value = 0_u64;

        // SAFETY
        //
        // The address names a live aligned local word throughout the arm and wait sequence.
        let target_arm = unsafe { monitor_arm((&raw const target_value).cast::<u8>()) };

        match target_arm {
            Ok(target_backend) => {
                assert_eq!(monitor_backend(), Some(target_backend));

                let target_start = std::time::Instant::now();

                // SAFETY
                //
                // The backend was armed on this thread and the local word remains alive.
                let target_wait = unsafe { monitor_wait(target_backend) };

                assert!(
                    target_wait.is_ok() || target_wait == Err(MonitorError::Unsupported),
                    "a protected hardware wait must complete or downgrade",
                );
                assert!(
                    target_start.elapsed() < Duration::from_secs(1),
                    "one hardware wait interval must remain finite",
                );
            }
            Err(MonitorError::Unsupported) => {
                assert_eq!(monitor_backend(), None);
            }
            Err(MonitorError::Fault) => {
                panic!("arming a live local word must not report a mapping fault");
            }
        }
    }

    /// A signal outside the registered accessor image must retain its native action.
    #[test]
    fn sigill_outside_the_fault_section_is_chained() {
        use std::os::unix::process::ExitStatusExt;

        const CHILD_VARIABLE: &str = "CATALEJO_SIGILL_CHILD";

        if std::env::var_os(CHILD_VARIABLE).is_some() {
            // SAFETY
            //
            // The child intentionally raises an unregistered SIGILL.
            unsafe { libc::raise(libc::SIGILL) };

            panic!("the default SIGILL action must terminate the child");
        }

        let target_executable =
            std::env::current_exe().expect("the test executable path must be available");
        let target_status = std::process::Command::new(target_executable)
            .args([
                "--exact",
                "ffi::test::sigill_outside_the_fault_section_is_chained",
            ])
            .env(CHILD_VARIABLE, "1")
            .status()
            .expect("the SIGILL child must run");

        assert_eq!(
            target_status.signal(),
            Some(libc::SIGILL),
            "SIGILL must retain its saved default behavior",
        );
    }
}
