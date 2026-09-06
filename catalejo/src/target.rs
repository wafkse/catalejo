//! Engagement target module.

use std::{
    fs::OpenOptions,
    io,
    os::fd::{AsFd, BorrowedFd, OwnedFd},
    path::Path,
};

use catalejo_fault::ffi::image;
use catalejo_sys::{ffi, id::TargetId};

/// A handle to an actively targeted process.
#[derive(Debug)]
// NOTE(invariant): The owned Mirilla session keeps the current address space's immutable exception
// image registration alive for at least as long as this target handle.
pub struct Target(OwnedFd, TargetId);

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
    #[inline]
    pub fn engage_at(target_path: impl AsRef<Path>, process_id: libc::pid_t) -> io::Result<Self> {
        let target_device = OwnedFd::from(
            OpenOptions::new()
                .read(true)
                .write(true)
                .open(target_path)?,
        );

        Self::register_exception_image(target_device.as_fd())?;

        // SAFETY: The provided file descriptor was created by the appropriate kernel module.
        let target_id = unsafe { ffi::command::engage(target_device.as_fd(), process_id)? };

        Ok(Self(target_device, target_id))
    }

    /// Engage a target process using the specified device file descriptor.
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

        Self::register_exception_image(target_clone.as_fd())?;

        // SAFETY: The provided file descriptor was created by "mirilla".
        let target_id = unsafe { ffi::command::engage(target_clone.as_fd(), process_id)? };

        Ok(Self(target_clone, target_id))
    }

    /// Engage a target process within the sesion context of an existing [`Target`] acquisition.
    #[inline]
    pub fn engage_within(&self, process_id: libc::pid_t) -> io::Result<Self> {
        let Self(target_device, ..) = self;

        // SAFETY: The file descriptor was created by the appropriate kernel module.
        unsafe { Self::engage_with(target_device.as_fd(), process_id) }
    }
}

impl Target {
    /// Ensure the sealed accessor image is registered for this session and observer address space.
    #[inline]
    fn register_exception_image(target_device: BorrowedFd<'_>) -> io::Result<()> {
        let image = image()
            .map_err(|target_error| io::Error::from_raw_os_error(target_error.code().get()))?;

        // SAFETY: `target_device` is opened from the configured Mirilla path or supplied under the
        // matching unsafe contract. `Image` proves the immutable VMA construction invariant.
        unsafe { ffi::command::register_exception_image(target_device, image) }
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

    /// Register the immutable exception image for the calling address space.
    ///
    /// A child created with `fork` has a distinct address space and must call this before using
    /// inherited protected accessors. Repeated registration in the same address space is harmless.
    #[inline]
    pub fn register_current_address_space(&self) -> io::Result<()> {
        let Self(target_device, ..) = self;

        Self::register_exception_image(target_device.as_fd())
    }

    /// Duplicate the handle to the active process.
    ///
    /// # Errors
    ///
    /// This may fail if the underlying session file descriptor failed to be duplicated.
    #[inline]
    pub fn duplicate(&self) -> io::Result<Self> {
        let &Self(ref target_left, target_right) = self;
        Self::register_exception_image(target_left.as_fd())?;

        Ok(Self(OwnedFd::try_clone(target_left)?, target_right))
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
