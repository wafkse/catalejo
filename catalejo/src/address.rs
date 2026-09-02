//! Virtual addresses and ranges.

use core::{fmt, num::NonZero};

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
        let &Self(target_value) = self;

        write!(f, "#{target_value:#x}")
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

        let is_non_empty = range_list.last().is_none_or(
            |&ViRange {
                 start_address,
                 end_address,
             }| start_address < end_address,
        );

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
