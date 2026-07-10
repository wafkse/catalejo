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
    /// Construct a [`ViRange`] spanning the half-open interval from `start_address` to `end_address`.
    #[inline]
    pub const fn new(start_address: ViAddr, end_address: ViAddr) -> Self {
        Self {
            start_address,
            end_address,
        }
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
///
/// The invariant matters because the sweeper carries a straddle overlap across contiguous mapped
/// bytes but must reset it at every range boundary, and it can only trust a boundary to be a genuine
/// discontinuity when the container guarantees the ranges never touch. Constructing through
/// [`ViSparse::new`] validates the invariant so downstream code relies on it without re-checking.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ViSparse {
    /// The contained ranges, held sorted ascending, non-empty, and strictly disjoint.
    ranges: alloc::boxed::Box<[ViRange]>,
}

impl ViSparse {
    /// Construct a [`ViSparse`] from `target_ranges`, validating the ordering and disjointness invariant.
    ///
    /// The ranges are sorted by start address first, so the caller need not pre-sort. Validation then
    /// rejects the set when any range is empty or when two ranges overlap or abut, because an abutting
    /// pair is really one contiguous range and would defeat the boundary-is-a-gap guarantee callers
    /// depend on. A rejected set yields [`None`] rather than a silently repaired container.
    #[inline]
    pub fn new(target_ranges: impl IntoIterator<Item = ViRange>) -> Option<Self> {
        let mut target_ranges = target_ranges.into_iter().collect::<alloc::vec::Vec<_>>();

        target_ranges.sort_unstable();

        let target_disjoint = target_ranges.windows(2).all(|target_window| {
            let [target_left, target_right] = target_window else {
                return true;
            };

            // NOTE: A gap is mandatory, so the left end must land strictly below the right start.
            target_left.start_address < target_left.end_address
                && target_left.end_address < target_right.start_address
        });

        let target_nonempty = target_ranges
            .last()
            .is_none_or(|target_last| target_last.start_address < target_last.end_address);

        if target_disjoint && target_nonempty {
            Some(Self {
                ranges: target_ranges.into_boxed_slice(),
            })
        } else {
            None
        }
    }

    /// Borrow the contained ranges in ascending, disjoint order.
    #[inline]
    pub const fn ranges(&self) -> &[ViRange] {
        let Self { ranges } = self;

        ranges
    }
}
