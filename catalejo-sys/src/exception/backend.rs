//! Access to Catalejo's fork-inheritable built-in records.

use core::ptr::NonNull;
use std::{
    io,
    os::fd::{AsRawFd, BorrowedFd},
};

use crate::ffi::binding;

use super::status;

/// A proof that Catalejo's linked fault routines are published in this mm.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Backend(
    /// The process-lifetime C singleton inherited by fork children.
    NonNull<binding::catalejo_fault_backend>,
);

// SAFETY: The pointer names process-lifetime singleton storage. C does not mutate it after
// initialization, and Mirilla clones the published kernel table during fork.
unsafe impl Send for Backend {}

// SAFETY: Every thread sharing an mm observes the same immutable initialized backend.
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
    /// This returns creation, mapping, loading, or publication failures.
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

    /// Retrieve the backend initialized in this process or inherited across fork.
    ///
    /// # Errors
    ///
    /// This returns not-found before initialization.
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
        Ok(Self(raw))
    }
}
