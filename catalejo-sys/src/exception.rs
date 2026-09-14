//! Exception contexts, slabs, actions, and the process fault backend.
//!
//! A context owns the kernel exception file descriptor. Each mapped slab is edited while writable
//! and published by changing the whole mapping to read only. Publication installs an immutable
//! kernel snapshot. Returning the mapping to writable removes that snapshot before editing resumes.

use core::{
    num::NonZero,
    ptr::NonNull,
    slice,
    sync::atomic::{AtomicUsize, Ordering},
};
use std::{
    io,
    os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd},
};

use fack::prelude::Error;

use crate::ffi::binding;

pub mod action;
pub mod backend;

/// The reason an exception slab size is invalid.
#[derive(Debug, Clone, Copy, Error, PartialEq, Eq)]
pub enum InvalidSlabSize {
    /// Zero cannot describe a mapping.
    #[error("exception slab size cannot be zero")]
    Zero,

    /// The size is not aligned to both a base page and the record stride.
    #[error("exception slab size is not aligned")]
    Misaligned,

    /// The size exceeds the kernel ABI ceiling.
    #[error("exception slab size exceeds the kernel limit")]
    TooLarge,
}

/// A validated fixed exception slab size in bytes.
///
/// NOTE(invariant): The private value is nonzero, aligned to the x86 Linux base-page size and the
/// 48-byte record stride, and no larger than the kernel slab-size ceiling.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SlabSize(
    /// The validated slab size in bytes.
    NonZero<usize>,
);

impl SlabSize {
    /// The built-in backend slab size.
    pub const DEFAULT: Self = Self(
        NonZero::new(binding::MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE as usize)
            .expect("the default slab size is nonzero"),
    );

    /// Validate a slab size.
    #[inline]
    pub const fn new(value: usize) -> Result<Self, InvalidSlabSize> {
        match NonZero::new(value) {
            None => Err(InvalidSlabSize::Zero),
            Some(target_size) => {
                let size_value = target_size.get();
                let page_aligned = size_value.is_multiple_of(4096);
                let record_aligned = size_value
                    .is_multiple_of(core::mem::size_of::<binding::mirilla_except_record>());
                let within_limit = size_value <= binding::MIRILLA_EXCEPT_SLAB_SIZE_LIMIT as usize;

                match (page_aligned, record_aligned, within_limit) {
                    (true, true, true) => Ok(Self(target_size)),
                    (_, _, false) => Err(InvalidSlabSize::TooLarge),
                    _ => Err(InvalidSlabSize::Misaligned),
                }
            }
        }
    }

    /// Return the byte size.
    #[inline]
    pub const fn get(self) -> usize {
        let Self(value) = self;

        value.get()
    }

    /// Return the number of fixed-stride records in one slab.
    #[inline]
    pub const fn record_capacity(self) -> usize {
        let Self(target_size) = self;

        target_size.get() / core::mem::size_of::<binding::mirilla_except_record>()
    }
}

/// The reason a userspace soft slab limit is invalid.
#[derive(Debug, Clone, Copy, Error, PartialEq, Eq)]
pub enum InvalidSoftSlabLimit {
    /// Zero would prohibit every allocation.
    #[error("exception slab limit cannot be zero")]
    Zero,

    /// The requested limit exceeds the kernel hard limit.
    #[error("exception slab limit exceeds the kernel limit")]
    AboveKernelLimit,
}

/// A userspace allocation limit bounded by the kernel hard limit.
///
/// NOTE(invariant): The private value is in the inclusive range from one through the kernel slab
/// limit.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SoftSlabLimit(
    /// The validated userspace allocation count.
    NonZero<usize>,
);

impl SoftSlabLimit {
    /// The default policy permits one slab.
    pub const DEFAULT: Self = Self(NonZero::<usize>::MIN);

    /// Validate a userspace soft limit.
    #[inline]
    pub const fn new(value: usize) -> Result<Self, InvalidSoftSlabLimit> {
        match NonZero::new(value) {
            None => Err(InvalidSoftSlabLimit::Zero),
            Some(value) => match value.get() <= binding::MIRILLA_EXCEPT_SLAB_LIMIT as usize {
                true => Ok(Self(value)),
                false => Err(InvalidSoftSlabLimit::AboveKernelLimit),
            },
        }
    }

    /// Return the allocation count.
    #[inline]
    pub const fn get(self) -> usize {
        let Self(value) = self;

        value.get()
    }
}

/// A nonzero kernel exception-context identifier.
///
/// NOTE(invariant): Only a successful CREATE result can construct this private nonzero value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ExceptionId(
    /// The nonzero identifier returned by Mirilla CREATE.
    NonZero<binding::mirilla_except_id_t>,
);

impl ExceptionId {
    /// Lift a successful kernel result.
    #[inline]
    const fn from_raw(target_id: binding::mirilla_except_id_t) -> Option<Self> {
        match NonZero::new(target_id) {
            Some(target_id) => Some(Self(target_id)),
            None => None,
        }
    }

    /// Return the kernel identifier.
    #[inline]
    pub const fn get(self) -> binding::mirilla_except_id_t {
        let Self(value) = self;

        value.get()
    }
}

/// A failure while allocating an exception slab.
#[derive(Debug, Error)]
pub enum SlabAllocationError {
    /// The context already owns its configured number of userspace slabs.
    #[error("exception slab allocation limit reached")]
    SoftLimitReached,

    /// The operating system rejected or malformed the slab mapping.
    #[error("exception slab mapping failed with {0}")]
    #[error(source(0))]
    System(
        /// The underlying mapping error.
        io::Error,
    ),
}

/// An fd-owned exception context bound to its creating address space.
///
// NOTE(invariant): The descriptor owns the kernel exception context. The identifier and slab size
// come from the same successful create operation. The allocation count never exceeds the soft
// limit through safe Rust allocation paths.
#[derive(Debug)]
pub struct Context(
    /// The anonymous exception file descriptor capability.
    OwnedFd,
    /// The kernel identifier for the exception context.
    ExceptionId,
    /// The exact byte size required by every slab mapping.
    SlabSize,
    /// The userspace admission control limit for live slabs.
    SoftSlabLimit,
    /// The number of live slab mappings owned through this context.
    AtomicUsize,
);

impl Context {
    /// Create the one exception context allowed for the current address space.
    ///
    /// # Safety
    ///
    /// `device` must be a descriptor created by Mirilla.
    ///
    /// # Errors
    ///
    /// This returns a kernel error or an invalid successful result from the foreign interface.
    #[inline]
    pub unsafe fn create(
        device: BorrowedFd<'_>,
        slab_size: SlabSize,
        soft_limit: SoftSlabLimit,
    ) -> io::Result<Self> {
        let mut target_id = 0 as binding::mirilla_except_id_t;
        let mut target_fd = -1 as RawFd;

        // SAFETY: The caller supplies the Mirilla descriptor contract. Both output pointers name
        // live local storage for the duration of the foreign call.
        let target_status = unsafe {
            binding::catalejo_mirilla_except_create(
                device.as_raw_fd(),
                slab_size.get() as binding::virtual_size_t,
                &raw mut target_id,
                &raw mut target_fd,
            )
        };

        status(target_status)?;

        let target_id = ExceptionId::from_raw(target_id);
        let target_fd = match target_fd {
            0.. => {
                // SAFETY: A successful create transfers ownership of one nonnegative descriptor.
                Some(unsafe { OwnedFd::from_raw_fd(target_fd) })
            }
            _ => None,
        };

        match (target_id, target_fd) {
            (Some(target_id), Some(target_fd)) => {
                let allocated_count = AtomicUsize::new(0);

                Ok(Self(
                    target_fd,
                    target_id,
                    slab_size,
                    soft_limit,
                    allocated_count,
                ))
            }
            (_, Some(target_fd)) => {
                drop(target_fd);

                Err(io::Error::from(io::ErrorKind::InvalidData))
            }
            _ => Err(io::Error::from(io::ErrorKind::InvalidData)),
        }
    }

    /// Return the kernel identifier.
    #[inline]
    pub const fn id(&self) -> ExceptionId {
        let &Self(_, target_id, ..) = self;

        target_id
    }

    /// Return the configured slab size.
    #[inline]
    pub const fn slab_size(&self) -> SlabSize {
        let &Self(_, _, slab_size, ..) = self;

        slab_size
    }

    /// Return the configured userspace slab limit.
    #[inline]
    pub const fn soft_limit(&self) -> SoftSlabLimit {
        let &Self(_, _, _, soft_limit, ..) = self;

        soft_limit
    }

    /// Return the number of currently mapped slabs.
    #[inline]
    pub fn allocated(&self) -> usize {
        let Self(_, _, _, _, allocated_count) = self;

        allocated_count.load(Ordering::Acquire)
    }

    /// Map one editable exception slab.
    ///
    /// # Errors
    ///
    /// This fails when the userspace soft limit is reached or the operating system rejects the
    /// mapping.
    #[inline]
    pub fn map(&self) -> Result<Slab<'_>, SlabAllocationError> {
        Self::reserve_slab(self)?;

        let Self(target_fd, _, slab_size, ..) = self;
        let mut record_list = core::ptr::null_mut();

        // SAFETY: The context descriptor is a live exception descriptor. The output pointer names
        // local storage and the C helper maps exactly one configured slab on success.
        let target_status = unsafe {
            binding::catalejo_except_slab_map(
                target_fd.as_raw_fd(),
                slab_size.get() as binding::virtual_size_t,
                &raw mut record_list,
            )
        };

        let map_result = status(target_status)
            .map_err(SlabAllocationError::System)
            .and_then(|()| {
                NonNull::new(record_list).ok_or_else(|| {
                    SlabAllocationError::System(io::Error::from(io::ErrorKind::InvalidData))
                })
            });

        match map_result {
            Ok(record_list) => Ok(Slab(self, record_list, SlabState::Editable)),
            Err(target_error) => {
                Self::release_slab(self);

                Err(target_error)
            }
        }
    }

    /// Reserve one userspace slab allocation slot.
    fn reserve_slab(&self) -> Result<(), SlabAllocationError> {
        let Self(_, _, _, soft_limit, allocated_count) = self;
        let limit_count = soft_limit.get();
        let update_result =
            allocated_count.fetch_update(Ordering::AcqRel, Ordering::Acquire, |allocated_count| {
                match allocated_count < limit_count {
                    true => Some(allocated_count + 1),
                    false => None,
                }
            });

        match update_result {
            Ok(_) => Ok(()),
            Err(_) => Err(SlabAllocationError::SoftLimitReached),
        }
    }

    /// Return one userspace slab allocation slot.
    fn release_slab(&self) {
        let Self(_, _, _, _, allocated_count) = self;
        let previous_count = allocated_count.fetch_sub(1, Ordering::AcqRel);

        debug_assert!(previous_count != 0);
    }
}

/// The userspace protection state tracked for one slab.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SlabState {
    /// The slab is writable and contributes no immutable kernel table.
    Editable,
    /// The slab is read only and contributes its immutable kernel snapshot.
    Published,
}

/// One complete exception slab mapping owned by a [`Context`].
///
// NOTE(invariant): The pointer names one exact mapping from the borrowed context. The tracked state
// changes only after successful whole-VMA protection transitions. Drop unmaps the mapping before
// returning the context allocation slot.
#[derive(Debug)]
pub struct Slab<'context>(
    /// The context that owns the file descriptor used to create this slab.
    &'context Context,
    /// The first exception record in the complete slab mapping.
    NonNull<binding::mirilla_except_record>,
    /// The last protection state established through this safe wrapper.
    SlabState,
);

impl Slab<'_> {
    /// Return whether the slab currently contributes a published kernel snapshot.
    #[inline]
    pub const fn is_published(&self) -> bool {
        let Self(_, _, slab_state) = self;

        matches!(slab_state, SlabState::Published)
    }

    /// Return read-only access to the complete record list.
    #[inline]
    pub const fn record_list(&self) -> &[binding::mirilla_except_record] {
        let Self(target_context, record_list, _) = self;
        let record_count = target_context.slab_size().record_capacity();

        // SAFETY: The slab invariant owns the complete live mapping for this lifetime. The mapping
        // always contains exactly record_count fixed-size records.
        unsafe { slice::from_raw_parts(record_list.as_ptr(), record_count) }
    }

    /// Return mutable access to the complete record list while the slab is editable.
    #[inline]
    pub const fn record_list_mut(&mut self) -> Option<&mut [binding::mirilla_except_record]> {
        let Self(target_context, record_list, slab_state) = self;
        let record_count = target_context.slab_size().record_capacity();

        match slab_state {
            SlabState::Editable => {
                // SAFETY: Editable state proves the VMA is writable. Exclusive access to the slab
                // prevents a second Rust reference to the returned record list.
                Some(unsafe { slice::from_raw_parts_mut(record_list.as_ptr(), record_count) })
            }
            SlabState::Published => None,
        }
    }

    /// Publish the complete record list as an immutable kernel snapshot.
    ///
    /// A successful call leaves the slab read only. A failed call leaves the slab editable and
    /// owned by the caller.
    ///
    /// # Errors
    ///
    /// This returns the operating system or kernel validation error from the protection change.
    #[inline]
    pub fn publish(&mut self) -> io::Result<()> {
        let Self(target_context, record_list, slab_state) = self;

        match slab_state {
            SlabState::Published => Ok(()),
            SlabState::Editable => {
                // SAFETY: The slab owns this exact complete mapping. No mutable record borrow can
                // coexist with this exclusive slab borrow.
                let target_status = unsafe {
                    binding::catalejo_except_slab_publish(
                        record_list.as_ptr(),
                        target_context.slab_size().get() as binding::virtual_size_t,
                    )
                };

                status(target_status)?;
                *slab_state = SlabState::Published;

                Ok(())
            }
        }
    }

    /// Remove the active kernel snapshot and return the slab to editable memory.
    ///
    /// A successful call leaves the slab writable. A failed call preserves the published state.
    ///
    /// # Errors
    ///
    /// This returns the operating system error from the protection change.
    #[inline]
    pub fn edit(&mut self) -> io::Result<()> {
        let Self(target_context, record_list, slab_state) = self;

        match slab_state {
            SlabState::Editable => Ok(()),
            SlabState::Published => {
                // SAFETY: The slab owns this exact complete mapping and the kernel accepts only the
                // supported whole-VMA read-only to read-write transition.
                let target_status = unsafe {
                    binding::catalejo_except_slab_edit(
                        record_list.as_ptr(),
                        target_context.slab_size().get() as binding::virtual_size_t,
                    )
                };

                status(target_status)?;
                *slab_state = SlabState::Editable;

                Ok(())
            }
        }
    }
}

impl Drop for Slab<'_> {
    #[inline]
    fn drop(&mut self) {
        let &mut Self(target_context, record_list, _) = self;

        // SAFETY: The slab invariant owns this exact complete mapping and Drop is its final Rust
        // owner. Unmapping also detaches any published kernel snapshot.
        let unmap_status = unsafe {
            binding::catalejo_except_slab_unmap(
                record_list.as_ptr(),
                target_context.slab_size().get() as binding::virtual_size_t,
            )
        };

        // NOTE(invariant): A failed unmap leaves the VMA and any publication active. Keep its soft
        // allocation slot charged because Rust can no longer prove that the kernel slab vanished.
        if unmap_status == 0 {
            Context::release_slab(target_context);
        }
    }
}

/// Convert a C negative-errno status into an I/O result.
fn status(target_status: core::ffi::c_int) -> io::Result<()> {
    match target_status {
        0 => Ok(()),
        ..=-1 => Err(io::Error::from_raw_os_error(target_status.saturating_abs())),
        _ => Err(io::Error::from(io::ErrorKind::InvalidData)),
    }
}

const _: () = {
    assert!(core::mem::size_of::<binding::mirilla_except_boundary>() == 16);
    assert!(core::mem::size_of::<binding::mirilla_except_predicate>() == 16);
    assert!(core::mem::size_of::<binding::mirilla_except_action>() == 16);
    assert!(core::mem::size_of::<binding::mirilla_except_record>() == 48);
    assert!(core::mem::align_of::<binding::mirilla_except_record>() == 16);
};
