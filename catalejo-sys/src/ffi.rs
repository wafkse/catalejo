//! Foreign Function Interface module for the `catalejo-sys` crate.

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

pub mod command {
    //! Commands for userspace-kernel device ioctls.

    use core::ffi::CStr;

    use std::{
        io::{self, ErrorKind},
        os::fd::{AsRawFd, BorrowedFd, OwnedFd, RawFd},
        path::PathBuf,
        sync::LazyLock,
    };

    use core::ptr;

    use crate::{
        ffi::binding,
        id::{PeepholeId, TargetId},
    };

    /// The canonical name of the device exposed by the kernel module.
    ///
    /// This is used for identifying and interfacing with the appropiate character device.
    pub const MIRILLA_DEVICE_NAME: &str = const {
        // SAFETY: The `CStr` is obtained from a `bindgen`-generated C string literal, so it always properly nul-delimited.
        let target_value =
            unsafe { CStr::from_bytes_with_nul_unchecked(binding::MIRILLA_DEVICE_NAME) };

        match target_value.to_str() {
            Ok(target_value) => target_value,
            Err(..) => unreachable!(),
        }
    };

    /// A memoized path to the character device exposed by the kernel module.
    pub static MIRILLA_DEVICE_PATH: LazyLock<PathBuf> =
        LazyLock::new(|| PathBuf::from("/dev/").join(self::MIRILLA_DEVICE_NAME));

    /// Engage with the target process.
    ///
    /// # Failure
    ///
    /// This can fail if the:
    ///
    /// * Process does not exist.
    /// * The calling process does not have the required privileges to engage.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn engage(fd: BorrowedFd, process_id: binding::pid_t) -> io::Result<TargetId> {
        let mut target_engagement = None::<TargetId>;

        let target_outcome =
            // SAFETY:
            //
            // * The caller has asserted that the provided file descriptor comes from `mirilla`.
            // * `mirilla_map_target_id_t` is identical ABI-wise to `Option<TargetId>`.
            unsafe { binding::catalejo_mirilla_engage(fd.as_raw_fd(), process_id, ptr::from_mut(&mut target_engagement).cast::<binding::mirilla_map_target_id_t>()) };

        match (target_outcome, target_engagement) {
            (binding::MIRILLA_COMMAND_OK, Some(target_id)) => Ok(target_id),
            (binding::MIRILLA_COMMAND_OK, ..) => Err(io::Error::from(ErrorKind::InvalidInput)),
            (
                target_errno
                @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK,
                ..,
            ) => Err(io::Error::from_raw_os_error(target_errno.abs())),
            // NOTE: This is impossible, hence unreachable.
            _ => unreachable!(),
        }
    }

    /// Disengage from the target process.
    ///
    /// # Failure
    ///
    /// This can fail if the:
    ///
    /// * Target was not previously engaged.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn disengage(fd: OwnedFd, target_id: TargetId) -> io::Result<()> {
        let target_outcome =
            // SAFETY: The caller has asserted that the provided file descriptor comes from `mirilla`.
            unsafe { binding::catalejo_mirilla_disengage(fd.as_raw_fd(), target_id.get()) };

        match target_outcome {
            binding::MIRILLA_COMMAND_OK => Ok(()),
            target_errno
            @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK => {
                Err(io::Error::from_raw_os_error(target_errno.abs()))
            }
            // NOTE: This is impossible, hence unreachable.
            _ => unreachable!(),
        }
    }

    /// For an engaged target process, create a peephole over the specified virtual memory range.
    ///
    /// # Failure
    ///
    /// This can fail if the:
    ///
    /// * Target was not previously engaged.
    /// * Provided memory range is malformed.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn peephole(
        fd: BorrowedFd,
        target_id: TargetId,
        start_address: u64,
        end_address: u64,
    ) -> io::Result<(PeepholeId, OwnedFd)> {
        let mut peephole_id = None::<PeepholeId>;
        let mut peephole_fd = None::<OwnedFd>;

        let target_outcome =
            // SAFETY:
            //
            // * The caller has asserted that the provided file descriptor comes from `mirilla`.
            // * `mirilla_map_peephole_id_t` is identical ABI-wise to `Option<PeepholeId>`.
            // * `OwnedFd/RawFd` is identical ABI-wise to a host file descriptor.
            unsafe { binding::catalejo_mirilla_peephole(fd.as_raw_fd(), target_id.get(), start_address, end_address, ptr::from_mut(&mut peephole_id).cast::<binding::mirilla_map_target_id_t>(), ptr::from_mut(&mut peephole_fd).cast::<RawFd>()) };

        match (target_outcome, (peephole_id, peephole_fd)) {
            (binding::MIRILLA_COMMAND_OK, (Some(target_left), Some(target_right))) => {
                Ok((target_left, target_right))
            }
            (binding::MIRILLA_COMMAND_OK, (..)) => Err(io::Error::from(ErrorKind::InvalidInput)),
            (
                target_errno
                @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK,
                (..),
            ) => Err(io::Error::from_raw_os_error(target_errno.abs())),
            // NOTE: This is impossible, hence unreachable.
            _ => unreachable!(),
        }
    }
}
