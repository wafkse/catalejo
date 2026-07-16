//! Virtual addresses and ranges.

use core::{fmt, marker, num::NonZero};

use catalejo_fault::behavior::Faultable;
use catalejo_memory::{behavior::Unassociated, primitive::Primitive};
use catalejo_sys::ffi;

/// A virtual address.
#[derive(Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
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

impl fmt::Debug for ViAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self(target_value) = self;

        write!(f, "#{target_value:p}")
    }
}

/// A typed pointer encoded for a 32 bit foreign address space.
///
/// The type parameter preserves the complete foreign raw pointer identity.
#[repr(transparent)]
// NOTE(invariant): The encoded address always occupies one 32 bit foreign pointer word.
pub struct Pointer32<T>
where
    T: 'static,
{
    /// The encoded foreign address.
    target_address: u32,

    /// The complete foreign raw pointer identity.
    target_type: marker::PhantomData<T>,
}

impl<T> Pointer32<T>
where
    T: 'static,
{
    /// Construct a typed 32 bit foreign pointer.
    #[inline]
    pub const fn new(target_address: u32) -> Self {
        let target_type = marker::PhantomData;

        Self {
            target_address,
            target_type,
        }
    }

    /// Determine the encoded foreign address.
    #[inline]
    pub const fn address(self) -> u32 {
        let Self { target_address, .. } = self;

        target_address
    }
}

impl<T> Copy for Pointer32<T> where T: 'static {}

impl<T> Clone for Pointer32<T>
where
    T: 'static,
{
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> PartialEq for Pointer32<T>
where
    T: 'static,
{
    #[inline]
    fn eq(&self, target_other: &Self) -> bool {
        let Self {
            target_address: target_left,
            ..
        } = self;
        let Self {
            target_address: target_right,
            ..
        } = target_other;

        target_left == target_right
    }
}

impl<T> Eq for Pointer32<T> where T: 'static {}

impl<T> fmt::Debug for Pointer32<T>
where
    T: 'static,
{
    fn fmt(&self, target_formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self { target_address, .. } = self;

        write!(target_formatter, "#{target_address:p}")
    }
}

// SAFETY
//
// Every 32 bit pattern is a valid foreign address and the pointee marker stores no bytes.
unsafe impl<T> Unassociated for Pointer32<T> where T: 'static {}

// SAFETY
//
// The representation is exactly one 32 bit foreign pointer word.
unsafe impl<T> Faultable for Pointer32<T>
where
    T: 'static,
{
    const PRIMITIVE: Primitive = Primitive::U32;
}

/// A typed pointer encoded for a 64 bit foreign address space.
///
/// The type parameter preserves the complete foreign raw pointer identity.
#[repr(transparent)]
// NOTE(invariant) The encoded address always occupies one 64 bit foreign pointer word.
pub struct Pointer64<T>
where
    T: 'static,
{
    /// The encoded foreign address.
    target_address: u64,

    /// The complete foreign raw pointer identity.
    target_type: marker::PhantomData<T>,
}

impl<T> Pointer64<T>
where
    T: 'static,
{
    /// Construct a typed 64 bit foreign pointer.
    #[inline]
    pub const fn new(target_address: u64) -> Self {
        let target_type = marker::PhantomData;

        Self {
            target_address,
            target_type,
        }
    }

    /// Determine the encoded foreign address.
    #[inline]
    pub const fn address(self) -> u64 {
        let Self { target_address, .. } = self;

        target_address
    }
}

impl<T> Copy for Pointer64<T> where T: 'static {}

impl<T> Clone for Pointer64<T>
where
    T: 'static,
{
    #[inline]

    fn clone(&self) -> Self {
        *self
    }
}

impl<T> PartialEq for Pointer64<T>
where
    T: 'static,
{
    #[inline]
    fn eq(&self, target_other: &Self) -> bool {
        let Self {
            target_address: target_left,
            ..
        } = self;
        let Self {
            target_address: target_right,
            ..
        } = target_other;

        target_left == target_right
    }
}

impl<T> Eq for Pointer64<T> where T: 'static {}

impl<T> fmt::Debug for Pointer64<T>
where
    T: 'static,
{
    fn fmt(&self, target_formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self { target_address, .. } = self;

        write!(target_formatter, "#{target_address:p}")
    }
}

// SAFETY:
//
// Every 64 bit pattern is a valid foreign address and the pointee marker stores no bytes.
unsafe impl<T> Unassociated for Pointer64<T> where T: 'static {}

// SAFETY:
//
// The representation is exactly one 64 bit foreign pointer word.
unsafe impl<T> Faultable for Pointer64<T>
where
    T: 'static,
{
    const PRIMITIVE: Primitive = Primitive::U64;
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
    /// Construct a [`ViRange`] spanning the half-open interval `[start-address, end-address)`.
    #[inline]
    pub const fn new(start_address: ViAddr, end_address: ViAddr) -> Self {
        Self {
            start_address,
            end_address,
        }
    }

    /// Construct a [`ViRange`] from a [`ffi::binding::virtual_address_t`] `[start-address, end-address)` pair.
    #[inline]
    pub const fn pair(
        start_address: ffi::binding::virtual_address_t,
        end_address: ffi::binding::virtual_address_t,
    ) -> Self {
        Self::new(ViAddr(start_address), ViAddr(end_address))
    }

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

/// An ordered, gap-separated set of non-overlapping [`ViRange`]s.
///
/// A scanner sweeps many disjoint stretches of a foreign address space, so it needs the stretches
/// presented in a canonical form. A [`ViSparse`] holds that form as an invariant. The contained
/// ranges are sorted ascending by start address, each range is non-empty, and no range overlaps or
/// abuts another, so consecutive ranges are separated by a real gap of unmapped or excluded space.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ViSparse(alloc::boxed::Box<[ViRange]>);

impl ViSparse {
    /// Construct a [`ViSparse`] from an iterator of individual [`ViRange`], validating the ordering and disjointness invariant.
    ///
    /// The ranges are sorted by start address first, so the caller need not pre-sort. Validation then
    /// rejects the set when any range is empty or when two ranges overlap or abut, because an abutting
    /// pair is really one contiguous range and would defeat the boundary-is-a-gap guarantee callers
    /// depend on. A rejected set yields [`None`] rather than a silently repaired container.
    #[inline]
    pub fn new(range_iter: impl IntoIterator<Item = ViRange>) -> Option<Self> {
        let mut range_list = range_iter.into_iter().collect::<alloc::vec::Vec<_>>();

        range_list.sort_unstable();

        let are_disjoint = range_list.windows(2).all(|range_window| {
            if let [
                ViRange {
                    start_address: left_start,
                    end_address: left_end,
                },
                ViRange {
                    start_address: right_start,
                    end_address: _,
                },
            ] = range_window
            {
                // NOTE: A gap is mandatory, so the left end must land strictly below the right start.
                left_start < left_end && left_end < right_start
            } else {
                // NOTE: Uneven windows, this is always true.
                true
            }
        });

        let is_non_empty = range_list
            .last()
            .is_none_or(|target_last| target_last.start_address < target_last.end_address);

        if are_disjoint && is_non_empty {
            Some(Self(range_list.into_boxed_slice()))
        } else {
            None
        }
    }

    /// Borrow the contained ranges in ascending, disjoint order.
    #[inline]
    pub const fn range_list(&self) -> &[ViRange] {
        let Self(target_list) = self;

        target_list
    }
}

#[cfg(test)]
mod test {
    use core::{any::TypeId, mem};

    use super::{Pointer32, Pointer64};

    #[test]
    fn foreign_pointer_layout_matches_its_encoded_width() {
        assert_eq!(mem::size_of::<Pointer32<*mut u8>>(), mem::size_of::<u32>());
        assert_eq!(
            mem::align_of::<Pointer32<*mut u8>>(),
            mem::align_of::<u32>()
        );
        assert_eq!(mem::size_of::<Pointer64<*mut u8>>(), mem::size_of::<u64>());
        assert_eq!(
            mem::align_of::<Pointer64<*mut u8>>(),
            mem::align_of::<u64>()
        );
    }

    #[test]
    fn foreign_pointer_identity_includes_width_and_raw_pointer_type() {
        assert_ne!(
            TypeId::of::<Pointer32<*const u8>>(),
            TypeId::of::<Pointer32<*mut u8>>()
        );
        assert_ne!(
            TypeId::of::<Pointer64<*const u8>>(),
            TypeId::of::<Pointer64<*mut u8>>()
        );
        assert_ne!(
            TypeId::of::<Pointer32<*const u8>>(),
            TypeId::of::<Pointer64<*const u8>>()
        );
    }
}
