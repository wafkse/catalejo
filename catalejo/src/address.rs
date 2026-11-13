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

/// A half-open virtual address interval.
///
/// # Notes
///
/// Construction does not validate ordering, size, or mapping availability.
#[derive(Debug, Eq, PartialEq, Default, Hash, Clone, Copy, Ord, PartialOrd)]
pub struct ViRange {
    /// The inclusive lower bound.
    pub start_address: ViAddr,

    /// The exclusive upper bound.
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

    /// Return the nonzero byte length if it fits in `usize`.
    ///
    /// Empty or reversed ranges return [`None`].
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

/// Nonempty address ranges sorted by start address and separated by gaps.
///
/// A scanner can traverse these ranges without revisiting an address. Adjacent
/// ranges are rejected because each boundary must represent excluded space.
///
/// # Notes
///
/// The set itself may be empty.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ViSparse(alloc::boxed::Box<[ViRange]>);

impl ViSparse {
    /// Sort address ranges and validate that gaps separate them.
    ///
    /// Because callers may supply ranges in any order, validation sorts by
    /// start address before checking that every range is nonempty and ends
    /// before the next begins. Overlap or adjacency returns [`None`] without
    /// merging ranges, preserving the guarantee that each boundary is a gap.
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
