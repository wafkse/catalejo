//! Engagement target module.

use std::{
    fs::OpenOptions,
    io,
    os::fd::{AsFd, BorrowedFd, OwnedFd},
};

use catalejo_fault::ffi::Subsystem;
use catalejo_sys::{ffi, id::TargetId};

/// A handle to an actively targeted process.
#[derive(Debug)]
pub struct Target(OwnedFd, TargetId, Subsystem);

impl Target {
    /// Engage a target process, creating a new `mirilla` session.
    #[inline]
    pub fn engage(target_subsystem: Subsystem, process_id: libc::pid_t) -> io::Result<Self> {
        let target_device = OwnedFd::from(
            OpenOptions::new()
                .read(true)
                .write(true)
                .open(ffi::command::MIRILLA_DEVICE_PATH.as_path())?,
        );

        // SAFETY: The provided file descriptor was created by "mirilla".
        let target_id = unsafe { ffi::command::engage(target_device.as_fd(), process_id)? };

        Ok(Self(target_device, target_id, target_subsystem))
    }

    /// Engage a target process using the specified device file descriptor.
    ///
    /// # Safety
    ///
    /// The provided device file descriptor must be `mirilla`-created.
    #[inline]
    pub unsafe fn engage_with(
        target_subsystem: Subsystem,
        target_device: BorrowedFd,
        process_id: libc::pid_t,
    ) -> io::Result<Self> {
        let target_clone = target_device.try_clone_to_owned()?;

        // SAFETY: The provided file descriptor was created by "mirilla".
        let target_id = unsafe { ffi::command::engage(target_clone.as_fd(), process_id)? };

        Ok(Self(target_clone, target_id, target_subsystem))
    }

    /// Engage a target process within the sesion context of an existing [`Target`] acquisition.
    #[inline]
    pub fn engage_within(&self, process_id: libc::pid_t) -> io::Result<Self> {
        let Self(target_device, _, target_subsystem) = self;

        // SAFETY: The file descriptor was created by the appropriate kernel module.
        unsafe { Self::engage_with(*target_subsystem, target_device.as_fd(), process_id) }
    }
}

impl Target {
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

    /// Determine the [`Subsystem`] initialization token.
    #[inline]
    pub const fn subsystem(&self) -> Subsystem {
        let Self(.., target_subsystem) = self;

        *target_subsystem
    }
}

impl Target {
    /// Duplicate the handle to the active process.
    ///
    /// # Errors
    ///
    /// This may fail if the underlying session file descriptor failed to be duplicated.
    #[inline]
    pub fn duplicate(&self) -> io::Result<Self> {
        let &Self(ref target_left, target_right, target_subsystem) = self;

        Ok(Self(
            OwnedFd::try_clone(target_left)?,
            target_right,
            target_subsystem,
        ))
    }
}

impl Target {
    /// Disengage with the target process.
    #[inline]
    pub fn disengage(self) -> io::Result<()> {
        let Self(target_device, target_id, ..) = self;

        // SAFETY: The provided file descriptor was created by "mirilla".
        unsafe { ffi::command::disengage(target_device, target_id)? };

        Ok(())
    }
}
