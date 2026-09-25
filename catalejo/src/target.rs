//! Engagement target module.

use std::{
    fs::OpenOptions,
    io,
    os::fd::{AsFd, BorrowedFd, OwnedFd},
    path::Path,
};

use catalejo_sys::{exception::backend::Backend, ffi, id::TargetId};

/// A handle to an actively targeted process.
#[derive(Debug)]
pub struct Target(
    /// The Mirilla map session that owns the target identifier.
    OwnedFd,
    /// The engaged process identifier within that map session.
    TargetId,
    /// The retained current-process exception-handler capability.
    Backend,
);

impl Target {
    /// Engage a target process through the configured default device path.
    ///
    /// # Failure
    ///
    /// This returns [`io::ErrorKind::NotFound`] when the build does not configure a default device
    /// path. Use [`Self::engage_at`] or [`Self::engage_with`] in that configuration.
    #[inline]
    pub fn engage(process_id: libc::pid_t) -> io::Result<Self> {
        let target_path = match ffi::command::default_device_path() {
            Some(target_path) => target_path,
            None => {
                #[cfg(feature = "stealth-mode")]
                return Err(io::Error::from_raw_os_error(libc::ENODEV));

                #[cfg(not(feature = "stealth-mode"))]
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "default device path is not configured",
                ));
            }
        };

        Self::engage_at(target_path, process_id)
    }

    /// Engage a target process through a caller-supplied device path.
    ///
    /// Mirilla permits one exception context per observer address space. Independent target
    /// engagements reuse the process backend before opening their map session.
    #[inline]
    pub fn engage_at(target_path: impl AsRef<Path>, process_id: libc::pid_t) -> io::Result<Self> {
        let target_device = OwnedFd::from(
            OpenOptions::new()
                .read(true)
                .write(true)
                .open(target_path)?,
        );

        let fault_backend = Self::initialize_backend(target_device.as_fd())?;

        // SAFETY: The provided file descriptor was created by the appropriate kernel module.
        let target_id = unsafe { ffi::command::engage(target_device.as_fd(), process_id)? };

        Ok(Self(target_device, target_id, fault_backend))
    }

    /// Engage a target process using the specified device file descriptor.
    ///
    /// Mirilla permits one exception context per observer address space. The process backend is
    /// reused when it is already active.
    ///
    /// # Safety
    ///
    /// The provided device file descriptor must be `mirilla`-created.
    #[inline]
    pub unsafe fn engage_with(
        target_device: BorrowedFd,
        process_id: libc::pid_t,
    ) -> io::Result<Self> {
        let target_clone = target_device.try_clone_to_owned()?;

        let fault_backend = Self::initialize_backend(target_clone.as_fd())?;

        // SAFETY: The provided file descriptor was created by "mirilla".
        let target_id = unsafe { ffi::command::engage(target_clone.as_fd(), process_id)? };

        Ok(Self(target_clone, target_id, fault_backend))
    }

    /// Engage a target process within the sesion context of an existing [`Target`] acquisition.
    #[inline]
    pub fn engage_within(&self, process_id: libc::pid_t) -> io::Result<Self> {
        let &Self(ref target_device, _, fault_backend) = self;
        let target_clone = OwnedFd::try_clone(target_device)?;

        // SAFETY: The descriptor belongs to the already-registered Mirilla session held by `self`.
        let target_id = unsafe { ffi::command::engage(target_clone.as_fd(), process_id)? };

        Ok(Self(target_clone, target_id, fault_backend))
    }
}

impl Target {
    /// Ensure the linked fault routines have a published table for this address space.
    #[inline]
    fn initialize_backend(target_device: BorrowedFd<'_>) -> io::Result<Backend> {
        // SAFETY: target_device is opened from the configured Mirilla path or supplied under the
        // matching unsafe contract.
        unsafe { Backend::initialize(target_device) }
    }

    /// Determine the device file descriptor that is in-use by the [`Target`].
    #[inline]
    pub fn device(&self) -> BorrowedFd<'_> {
        let Self(target_device, ..) = self;

        target_device.as_fd()
    }

    /// Determine the identifier of the acquired target.
    #[inline]
    pub const fn id(&self) -> TargetId {
        let Self(_, target_id, ..) = self;

        *target_id
    }

    /// Return the retained handle to the current-process fault handlers.
    #[inline]
    pub const fn fault_backend(&self) -> Backend {
        let &Self(_, _, fault_backend) = self;

        fault_backend
    }

    /// Initialize a fresh fault backend for the calling address space when required.
    ///
    /// A child created with `fork` has a distinct address space and must call this before using or
    /// reacquiring inherited protected accessors.
    #[inline]
    pub fn initialize_fault_backend(&mut self) -> io::Result<()> {
        let Self(target_device, _, fault_backend) = self;
        let refreshed_backend = Self::initialize_backend(target_device.as_fd())?;

        *fault_backend = refreshed_backend;

        Ok(())
    }

    /// Duplicate the handle to the active process.
    ///
    /// # Errors
    ///
    /// This may fail if the underlying session file descriptor failed to be duplicated.
    #[inline]
    pub fn duplicate(&self) -> io::Result<Self> {
        let &Self(ref target_device, target_id, fault_backend) = self;
        let target_clone = OwnedFd::try_clone(target_device)?;

        Ok(Self(target_clone, target_id, fault_backend))
    }
}

impl Target {
    /// Disengage with the target process.
    #[inline]
    pub fn disengage(self) -> io::Result<()> {
        let Self(target_device, target_id, _) = self;

        // SAFETY: The provided file descriptor was created by "mirilla".
        unsafe { ffi::command::disengage(target_device, target_id)? };

        Ok(())
    }
}
