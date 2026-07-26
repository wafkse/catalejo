//! The Foreign-Function-Interface module for `catalejo-fault`.
//!
//! This is used to bind the with the *C* side of the crate, which implements signal guarding.

use core::{
    marker,
    mem::{self, MaybeUninit},
    ptr,
    sync::atomic::{AtomicBool, Ordering},
};

use catalejo_memory::primitive::PrimitiveUnion;

use crate::behavior::Faultable;

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

/// Whether a [`Subsystem`] was ever properly constructed as result of proper initialization.
static SUBSYSTEM_INITIALIZED: AtomicBool = AtomicBool::new(false);

/// A token that guarantees that the *catalejo* C-based subsystem has been initialized properly.
#[repr(transparent)]
#[derive(Debug, Copy, Clone)]
pub struct Subsystem(marker::PhantomData<Self>);

impl Subsystem {
    /// Initialize the C-based subsystem if it has not been initialized, providing a token to prove it.
    ///
    /// # Safety
    ///
    /// This is a low-level initialization function for the C-based subsystem of the `catalejo-fault` crate.
    ///
    /// Particularly, this initializes the signal-catching mechanisms to be able to handle synchronous hardware
    /// exceptions properly.
    ///
    /// To guarantee the safety of all posterior `catalejo` operations, the following must be guaranteed.
    ///
    /// * No other thread may register a signal handler for `SIGBUS`, `SIGSEGV`, or `SIGILL` while this function executes.
    ///
    /// * Any posterior signal handler must chain the behavior of the existing `catalejo`-installed signal handlers, preserving exact behavior.
    #[inline]
    pub unsafe fn initialize() -> Option<Subsystem> {
        let target_outcome =
            // SAFETY: The caller asserts that this is not inherently racy with the kernel.
            unsafe { binding::catalejo_fault_initialize() };

        match target_outcome {
            binding::CATALEJO_OUTCOME_SUCCESS => {
                // NOTE: Mark the subsystem as initialized on the Rust side.
                SUBSYSTEM_INITIALIZED.store(true, Ordering::Release);

                Some(Self(marker::PhantomData::<Self>))
            }
            binding::CATALEJO_OUTCOME_ERROR => None::<Self>,
            // NOTE: Any other value is impossible.
            _ => unreachable!(),
        }
    }

    /// Attempt to retrieve a memoized [`Subsystem`] instance if the underlying fault-catching subsystem has been already initialized.
    ///
    /// This is used to sidestep the cascading [`Subsystem`] pass-by-value requirement in many structures dependant on the fault-catching susbsystem.
    ///
    /// # Panics
    ///
    /// This will panic if the underlying subsystem has not been initialized.
    #[inline]
    pub fn memoize() -> Subsystem {
        // NOTE: If the subsystem is noted as initialized, fabricate the token.
        if SUBSYSTEM_INITIALIZED.load(Ordering::Acquire) {
            Self(marker::PhantomData::<Self>)
        } else {
            #[cfg(feature = "stealth-mode")]
            panic!();

            #[cfg(not(feature = "stealth-mode"))]
            panic!("catalejo-fault subsystem is not initialized");
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
pub fn monitor_backend(_: Subsystem) -> Option<MonitorBackend> {
    // SAFETY
    //
    // The subsystem token proves that optional instruction faults can be caught.
    let target_backend = unsafe { binding::catalejo_monitor_select() };

    match target_backend {
        binding::CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR => Some(MonitorBackend::IntelUmonitor),
        binding::CATALEJO_MONITOR_BACKEND_AMD_MONITORX => Some(MonitorBackend::AmdMonitorx),
        binding::CATALEJO_MONITOR_BACKEND_UNSUPPORTED => None,
        _ => unreachable!(),
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
#[inline]
pub unsafe fn monitor_arm(
    _: Subsystem,
    target_address: *const u8,
) -> Result<MonitorBackend, MonitorError> {
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
        _ => unreachable!(),
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
#[inline]
pub unsafe fn monitor_wait(
    _: Subsystem,
    target_backend: MonitorBackend,
) -> Result<(), MonitorError> {
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
        _ => unreachable!(),
    }
}

/// Perform a primitive-level fault-tolerant read of a memory address.
///
/// This function attempts to read from the target address using a memory-coherent hardware fetch.
/// If the underlying physical page is unmapped, the hardware exception is caught by the subsystem
/// and the function gracefully returns `None`.
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
/// * No foreign library has hijacked the synchronous POSIX signal handlers without implementing
///   perfect chaining since the subsystem token was issued.
#[inline]
pub unsafe fn read<F>(_: Subsystem, target_address: *mut F) -> Option<F>
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
    //   accessed by assembly; a resulting fault is caught by the subsystem, never undefined behavior.
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
        _ => unreachable!(),
    }
}

/// Perform a primitive-level fault-tolerant write to a memory address.
///
/// This function attempts to write to the target address using a memory-coherent hardware access.
/// If the underlying physical page is unmapped, the hardware exception is caught by the subsystem
/// and the function gracefully returns `false`. If the write is indeed successful, it returns `true`.
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
pub unsafe fn write<F>(_: Subsystem, target_address: *mut F, target_value: F) -> bool
where
    F: Faultable,
{
    let target_source = &raw const target_value;

    // SAFETY:
    // * The address names a `Faultable`-typed value, so every bit-pattern is valid.
    //
    // * The pointer carries exposed provenance over outside-abstract-machine memory and is
    //   accessed by assembly; a resulting fault is caught by the subsystem, never undefined behavior.
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
        _ => unreachable!(),
    }
}

/// Perform a fault-protected copy from the target source to the target address.
///
/// This function attempts to copy `target_count` bytes as a byte-granular ascending stream. If
/// the underlying physical page of the fault-adjudicated side is unmapped, the hardware exception
/// is caught by the subsystem and the function gracefully returns `Err` carrying the count of
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
/// * The other side names ordinary abstract-machine memory, and the subsystem cannot rescue it:
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
/// * No foreign library has hijacked the synchronous POSIX signal handlers without implementing
///   perfect chaining since the subsystem token was issued.
#[inline]
pub unsafe fn copy(
    _: Subsystem,
    target_address: *mut u8,
    target_source: *const u8,
    target_count: usize,
) -> Result<(), usize> {
    // SAFETY:
    // * The fault-adjudicated side carries exposed provenance over outside-abstract-machine
    //   memory and is accessed by assembly, so a resulting fault is caught by the subsystem, never
    //   undefined behavior.
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
        _ => unreachable!(),
    }
}

#[cfg(test)]
mod test {
    use core::time::Duration;

    use super::{MonitorError, Subsystem, monitor_arm, monitor_backend, monitor_wait};

    /// Acquire the initialized subsystem for monitor tests.
    unsafe fn subsystem() -> Subsystem {
        // SAFETY
        //
        // The test harness does not install competing synchronous signal handlers.
        unsafe { Subsystem::initialize() }.expect("the catalejo subsystem must initialize")
    }

    #[test]
    fn monitor_selection_and_wait_are_runtime_safe() {
        let target_value = 0_u64;

        // SAFETY
        //
        // The address names a live aligned local word throughout the arm and wait sequence.
        let target_subsystem = unsafe { subsystem() };
        let target_arm =
            unsafe { monitor_arm(target_subsystem, (&raw const target_value).cast::<u8>()) };

        match target_arm {
            Ok(target_backend) => {
                assert_eq!(monitor_backend(target_subsystem), Some(target_backend));

                let target_start = std::time::Instant::now();

                // SAFETY
                //
                // The backend was armed on this thread and the local word remains alive.
                let target_wait = unsafe { monitor_wait(target_subsystem, target_backend) };

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
                assert_eq!(monitor_backend(target_subsystem), None);
            }
            Err(MonitorError::Fault) => {
                panic!("arming a live local word must not report a mapping fault");
            }
        }
    }

    /// A signal outside the protected instruction section must reach the saved action.
    #[test]
    fn sigill_outside_the_fault_section_is_chained() {
        use std::os::unix::process::ExitStatusExt;

        const CHILD_VARIABLE: &str = "CATALEJO_SIGILL_CHILD";

        if std::env::var_os(CHILD_VARIABLE).is_some() {
            // SAFETY
            //
            // The child has no competing signal installer and intentionally raises SIGILL.
            let _ = unsafe { subsystem() };
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
