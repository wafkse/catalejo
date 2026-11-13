//! Protected hardware monitor instruction support.

#[cfg(all(feature = "stealth-mode", not(test)))]
use core::hint;

use crate::{exception::backend::Backend, ffi::binding};

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
/// Selection checks processor support without arming an address. If no
/// supported implementation is available, [`None`] lets callers choose polling
/// before attempting to execute an optional instruction.
#[inline]
pub fn backend() -> Option<MonitorBackend> {
    // SAFETY: CPUID-backed monitor selection does not execute a protected monitor instruction.
    let target_backend = unsafe { binding::catalejo_monitor_select() };

    match target_backend {
        binding::CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR => Some(MonitorBackend::IntelUmonitor),
        binding::CATALEJO_MONITOR_BACKEND_AMD_MONITORX => Some(MonitorBackend::AmdMonitorx),
        binding::CATALEJO_MONITOR_BACKEND_UNSUPPORTED => None,
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: The C implementation returns only declared backend values.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Arm the selected hardware monitor for an address.
///
/// A successful result identifies the backend to pass to [`wait`] on this
/// thread. Since instruction availability can differ from the initial feature
/// check, callers must still handle [`MonitorError::Unsupported`] here.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * The monitored address must identify suitable local memory whose mapping
///   remains live through the matching [`wait`] call.
/// * The recovery capability must belong to the current address space.
#[inline]
pub unsafe fn arm(
    _fault_backend: &Backend,
    target_address: *const u8,
) -> Result<MonitorBackend, MonitorError> {
    // SAFETY:
    // The caller supplies the monitor address contract and an initialized backend.
    let target_outcome = unsafe { binding::catalejo_monitor_arm(target_address) };

    match target_outcome {
        binding::CATALEJO_MONITOR_ARM_INTEL_UMONITOR => Ok(MonitorBackend::IntelUmonitor),
        binding::CATALEJO_MONITOR_ARM_AMD_MONITORX => Ok(MonitorBackend::AmdMonitorx),
        binding::CATALEJO_MONITOR_ARM_FAULT => Err(MonitorError::Fault),
        binding::CATALEJO_MONITOR_ARM_UNSUPPORTED => Err(MonitorError::Unsupported),
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: The C implementation returns only declared monitor-arm outcomes.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}

/// Wait for one bounded interval with an armed hardware monitor.
///
/// # Notes
///
/// A wake does not establish that the monitored value changed. Read it again
/// after waiting, then re-arm if another wait is needed.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * The selected monitor implementation must come from the immediately
///   preceding successful [`arm`] call on this thread.
/// * The monitored mapping must remain live through the wait.
/// * The recovery capability must belong to the current address space.
#[inline]
pub unsafe fn wait(_fault_backend: &Backend, backend: MonitorBackend) -> Result<(), MonitorError> {
    let backend = match backend {
        MonitorBackend::IntelUmonitor => binding::CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR,
        MonitorBackend::AmdMonitorx => binding::CATALEJO_MONITOR_BACKEND_AMD_MONITORX,
    };

    // SAFETY:
    // The caller supplies the same-thread monitor contract and the current process backend.
    let outcome = unsafe { binding::catalejo_monitor_wait(backend) };

    match outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => Ok(()),
        binding::CATALEJO_OUTCOME_ERROR => Err(MonitorError::Fault),
        binding::CATALEJO_OUTCOME_INVALID_VALUE => Err(MonitorError::Unsupported),
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(all(feature = "stealth-mode", test))]
        _ => std::process::abort(),

        #[cfg(all(feature = "stealth-mode", not(test)))]
        // SAFETY: The C implementation returns only declared outcomes.
        _ => unsafe { hint::unreachable_unchecked() },
    }
}
