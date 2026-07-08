//! Virtual addresses and ranges.

// FIXME(arch): This assumes 64-bit canonical virtual addresses. Will need to

use core::num::NonZero;

use catalejo_sys::ffi;

/// A virtual address.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct ViAddr(pub ffi::binding::virtual_address_t);

impl ViAddr {
    /// Construct a new [`ViAddr`] wrapper for the target address.
    #[inline]
    pub const fn new(target_address: ffi::binding::virtual_address_t) -> Self {
        Self(target_address)
    }

    /// Unwrap the contained virtual address from the [`ViAddr`].
    #[inline]
    pub const fn unwrap(self) -> ffi::binding::virtual_address_t {
        let Self(target_value) = self;

        target_value
    }
}

/// A virtual address range.
#[derive(Debug, Eq, PartialEq, Default, Hash, Clone, Copy, Ord, PartialOrd)]
pub struct ViRange {
    /// The start address of the range.
    pub start_address: ViAddr,

    /// The end address of the range.
    pub end_address: ViAddr,
}

impl ViRange {
    /// Determine the size of the range.
    #[inline]
    pub fn size(&self) -> Option<NonZero<usize>> {
        let &Self {
            start_address: ViAddr(start_address),
            end_address: ViAddr(end_address),
        } = self;

        end_address
            .checked_sub(start_address)
            .map(usize::try_from)
            .and_then(Result::ok)
            .and_then(NonZero::<usize>::new)
    }
}
