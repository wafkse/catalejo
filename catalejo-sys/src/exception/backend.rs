//! PID-aware access to the process singleton for Catalejo's built-in records.

use core::{num::NonZero, ptr::NonNull};
use std::{
    io,
    os::fd::{AsRawFd, BorrowedFd},
};

use crate::ffi::binding;

use super::status;

/// A process-local proof that Catalejo's linked fault routines are published.
///
/// NOTE(invariant): raw is returned only by the C singleton after it has created and published the
/// default slab for process_id. A protected operation compares process_id with the calling process
/// before relying on the singleton, so an inherited handle cannot prove recovery after fork.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Backend(
    /// The process-lifetime C singleton established for the creating PID.
    NonNull<binding::catalejo_fault_backend>,
    /// The nonzero process identifier for which the singleton was published.
    NonZero<u32>,
);

// SAFETY: The pointer names process-lifetime singleton storage. C does not mutate an initialized
// backend within one pid, and a fork leaves only the calling thread in the child before rebuild.
unsafe impl Send for Backend {}

// SAFETY: Every thread in one pid observes the same immutable initialized backend.
unsafe impl Sync for Backend {}

impl Backend {
    /// Initialize or reuse the process backend through a Mirilla descriptor.
    ///
    /// # Safety
    ///
    /// `device` must be a descriptor created by Mirilla.
    ///
    /// # Errors
    ///
    /// This returns creation, mapping, loading, publication, or stale-rebuild failures.
    #[inline]
    pub unsafe fn initialize(device: BorrowedFd<'_>) -> io::Result<Self> {
        let mut target_value = core::ptr::null();

        // SAFETY: The caller supplies the descriptor contract and the output points to local
        // storage. C publishes a process-lifetime singleton pointer only on success.
        let target_state = unsafe {
            binding::catalejo_fault_backend_initialize(device.as_raw_fd(), &raw mut target_value)
        };

        Self::lift(target_state, target_value)
    }

    /// Retrieve the backend initialized for the calling pid.
    ///
    /// # Errors
    ///
    /// This returns not-found before initialization and stale in a fork child.
    #[inline]
    pub fn retrieve() -> io::Result<Self> {
        let mut target_value = core::ptr::null();

        // SAFETY: C writes either null with an error or its process singleton pointer.
        let target_state =
            unsafe { binding::catalejo_fault_backend_retrieve(&raw mut target_value) };

        Self::lift(target_state, target_value)
    }

    /// Lift a successful singleton result.
    fn lift(
        target_status: core::ffi::c_int,
        raw: *const binding::catalejo_fault_backend,
    ) -> io::Result<Self> {
        status(target_status)?;

        let raw = NonNull::new(raw.cast_mut())
            .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;
        let process_id = NonZero::new(std::process::id())
            .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;

        Ok(Self(raw, process_id))
    }

    /// Verify that this backend was published for the calling process.
    ///
    /// # Errors
    ///
    /// This returns stale when the handle was inherited across `fork`.
    #[inline]
    pub fn validate(&self) -> io::Result<()> {
        let &Self(_, process_id) = self;
        let current_process = std::process::id();

        match process_id.get() == current_process {
            true => Ok(()),
            false => Err(io::Error::from_raw_os_error(libc::ESTALE)),
        }
    }
}
