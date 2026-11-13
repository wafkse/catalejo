//! Window placement, reuse, and resolution of typed access requests.

use std::io;

use core::marker;

use catalejo_memory::behavior::Unassociated;
use catalejo_sys::ffi;

use crate::{
    address::{ViAddr, ViRange},
    offset::Offset,
    peephole::Foreign,
    prelude::Peephole,
};

pub mod lru;

pub mod memoize;

pub mod rebased;

pub use self::{
    memoize::Memoize,
    rebased::{Rebase, Rebased},
};

/// A logical window index within a rebase grid.
///
/// A [`Rebase`] computes this index by subtracting its grid shift from an
/// address and dividing by its granule size. Addresses in the same window then
/// share an index that the manager can use for cache lookup.
///
/// # Notes
///
/// The index is meaningful only with the granule and shift of its originating
/// grid. It is not an absolute virtual address.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct Frame(pub u64);

impl Frame {
    /// Construct a [`Frame`] from the target logical frame index.
    #[inline]
    pub const fn new(target_index: u64) -> Self {
        Self(target_index)
    }

    /// Return the grid-relative window index.
    #[inline]
    pub const fn value(self) -> u64 {
        let Self(target_index) = self;

        target_index
    }
}

/// Window size and alignment for grid-based managers.
///
/// Larger granules allow nearby accesses to share a window. They also reserve
/// more observer address space per window. The choice does not require the
/// target to use hardware huge pages.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u64)]
pub enum Granule {
    /// A `4 KiB` window, the minimum supported mapping size.
    ///
    /// Page-sized windows limit the mapped span but cannot support a
    /// half-granule-shifted grid because the shift would not be page-aligned.
    Page = 4 * 1024 - 1,

    /// A `2 MiB` window, used by default.
    ///
    /// Nearby reads can reuse the window, amortizing its creation cost across
    /// sequential scans or pointer traversal within the range.
    #[default]
    Hugepage = 2 * 1024 * 1024 - 1,

    /// A `1 GiB` window for accesses spread across a large target range.
    ///
    /// Each window reserves a correspondingly large observer mapping.
    HumongousPage = 1024 * 1024 * 1024 - 1,
}

impl Granule {
    /// Round an address down to the base of its unshifted granule window.
    #[inline]
    pub const fn base(self, target_address: ViAddr) -> ViAddr {
        let ViAddr(target_address) = target_address;

        ViAddr(target_address & !(self as ffi::binding::virtual_address_t))
    }

    /// Return the byte offset within an address's unshifted granule window.
    #[inline]
    pub const fn offset(self, target_address: ViAddr) -> ViAddr {
        let ViAddr(target_address) = target_address;

        ViAddr(target_address & (self as ffi::binding::virtual_address_t))
    }

    /// Determine the size of the target granularity.
    #[inline]
    pub const fn size(self) -> u64 {
        // NOTE: The discriminant is stored as the bitwise mask of the intra-peephole offset.
        // Its a power of two, so it can be added 1 to determine the size.
        self as ffi::binding::virtual_address_t + 1
    }

    /// Determine half of the target granularity size.
    ///
    /// This is the offset basis by which the shifted rebase grid (`L1`) is displaced, and doubles as
    /// the largest structure a [`Rebased`] manager can peephole without straddling a granule boundary.
    #[inline]
    pub const fn half(self) -> ffi::binding::virtual_address_t {
        // NOTE: The size is a power of two, so a single right-shift halves it exactly.
        self.size() >> 1
    }

    /// Determine the granule shift of the target granularity.
    ///
    /// This is the number of low bits an address dedicates to its intra-peephole offset, i.e. the
    /// amount by which an address is shifted to derive its granule [`Frame`] index.
    #[inline]
    pub const fn shift(self) -> u32 {
        // NOTE: The size is a power of two (the discriminant is its all-ones offset mask), so its
        // trailing-zero count is the shift that divides an address into a granule frame index.
        self.size().trailing_zeros()
    }

    /// Return whether a half-granule shift would violate page alignment.
    #[inline]
    pub const fn indivisible(self) -> bool {
        matches!(self, Self::Page)
    }
}

/// Resolve foreign addresses into reusable peephole access handles.
///
/// To serve a request, the manager chooses a window and returns an [`Access`]
/// pairing that [`Peephole`] with the value's offset. By caching those windows,
/// it lets nearby or repeated accesses share the cost of opening and mapping
/// target memory.
///
/// # Constraints
///
/// The requested value must fit within a window and meet its alignment
/// requirements. Grid-based managers may use shifted windows to accommodate
/// values that cross a grid boundary. Each implementation documents its size
/// limits and placement restrictions.
pub trait Manage {
    /// Look up a cached window that can serve `U` at the foreign address.
    ///
    /// Because lookup does not open windows, [`None`] only means the current
    /// cache cannot serve the request. Use [`Manage::source`] to try opening a
    /// suitable window before concluding that access is unavailable.
    ///
    /// When lookup succeeds, convert the returned intent through
    /// [`Access::foreign`] to validate bounds and alignment before reading.
    fn absolute<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated;

    /// Resolve a foreign address, opening and caching a window if needed.
    ///
    /// Returns [`None`] when the requested value cannot be served under this
    /// manager's placement, size, or alignment constraints.
    ///
    /// # Failure
    ///
    /// Opening or mapping a new peephole can return an operating system error.
    fn source<U>(&self, target_address: ViAddr) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated;

    /// Refresh an [`Access`] intent that may not correspond to the [`Peephole`] internally referenced.
    #[inline]
    fn refresh<U>(&self, target_value: Access<U>) -> Option<Foreign<U>>
    where
        U: Unassociated,
    {
        let Access(ref target_peephole, target_offset, ..) = target_value;

        let ViRange {
            start_address: ViAddr(base_address),
            ..
        } = target_peephole.range();
        let target_address = ViAddr::new(base_address.checked_add(target_offset.value())?);

        match Access::foreign(target_value) {
            Some(target_value) => Some(target_value),
            None => match Self::source::<U>(self, target_address) {
                Ok(target_value) => target_value.and_then(Access::<U>::foreign),
                Err(..) => None,
            },
        }
    }

    /// Return the manager's placement granularity.
    ///
    /// Grid-based managers use this as their window size and alignment.
    /// [`Memoize`] reports page granularity but may open larger windows to fit
    /// requested spans. The value is fixed for the manager's lifetime.
    fn granule(&self) -> Granule;
}

/// A request to access `U` at an offset within a peephole.
///
/// # Notes
///
/// The offset may come from sparse projection and need not fit the current
/// window. Use [`Manage::refresh`] to resolve such requests before access.
#[derive(Debug, Clone)]
pub struct Access<U>(pub Peephole, pub Offset, marker::PhantomData<U>)
where
    U: Unassociated;

impl<U> Access<U>
where
    U: Unassociated,
{
    /// Construct an [`Access`] intent for the peephole-offset pair.
    #[inline]
    pub const fn intent(target_peephole: Peephole, target_offset: Offset) -> Self {
        Self(target_peephole, target_offset, marker::PhantomData::<U>)
    }

    /// Borrow the [`Peephole`] this access is served from.
    #[inline]
    pub const fn peephole(&self) -> &Peephole {
        let Self(target_peephole, ..) = self;

        target_peephole
    }

    /// Determine the resolved in-window [`Offset`] of the access.
    #[inline]
    pub const fn offset(&self) -> Offset {
        let &Self(_, target_offset, ..) = self;

        target_offset
    }

    /// Consume the intent into a [`Foreign`] handle for `U` at the resolved offset.
    ///
    /// Returns [`None`] if `U` does not fit or is misaligned within the window.
    ///
    /// [`Foreign`]: crate::peephole::Foreign
    #[inline]
    pub fn foreign(self) -> Option<Foreign<U>> {
        let Self(target_peephole, target_offset, ..) = self;

        target_peephole.at::<U>(target_offset)
    }
}
