//! Virtual addresses and ranges.

// FIXME(arch): This assumes 64-bit canonical virtual addresses. Will need to

use core::num::NonZero;
use std::mem;

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
            .map(Result::ok)
            .flatten()
            .map(NonZero::<usize>::new)
            .flatten()
    }
}

/// A local offset applied to a virtual base.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct Offset(usize);

impl Offset {
    /// Construct a byte-level offset from the target value.
    #[inline]
    pub const fn byte(target_value: usize) -> Self {
        Self(target_value)
    }

    /// Construct a type-level offset from the target value.
    ///
    /// This determines the offset using the size of the parametric type.
    #[inline]
    pub const fn typed<T>(target_value: usize) -> Self {
        Self::byte(mem::size_of::<T>().wrapping_mul(target_value))
    }

    /// Stack two disjoint offsets onto a singular one.
    ///
    /// This is equivalent to add the two byte-level together via wrapping arithmetic.
    #[inline]
    pub const fn stack(self, target_value: Self) -> Self {
        let Self(target_left) = self;
        let Self(target_right) = target_value;

        Self(target_left.wrapping_add(target_right))
    }
}

impl Offset {
    /// Determine the encapsulated offset value.
    #[inline]
    pub const fn value(&self) -> usize {
        let &Self(target_value) = self;

        target_value
    }

    /// Borrow the encapsulated offset value.
    #[inline]
    pub const fn value_mut(&mut self) -> &mut usize {
        let Self(target_value) = self;

        target_value
    }
}
