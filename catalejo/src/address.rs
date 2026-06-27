//! Virtual addresses and ranges.

// FIXME(arch): This assumes 64-bit canonical virtual addresses. Will need to

use core::num::NonZero;

use catalejo_sys::ffi;

/// A virtual address.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct ViAddr(pub ffi::binding::virtual_address_t);

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
