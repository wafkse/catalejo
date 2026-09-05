//! A peephole management context.

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

/// A new-type over an [`u64`] that determines the frame indice of a [`Peephole`] in relative to its respective level.
///
/// A frame is a virtual address divided by a granule size, analogous to a page frame number. It is
/// a logical cell index rather than an address, so it can be neither dereferenced nor mistaken for
/// one, and is meaningful only relative to the [`Rebase`] grid (granule and offset basis) that
/// produced it.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct Frame(pub u64);

impl Frame {
    /// Construct a [`Frame`] from the target logical frame index.
    #[inline]
    pub const fn new(target_index: u64) -> Self {
        Self(target_index)
    }

    /// Determine the encapsulated logical frame index.
    #[inline]
    pub const fn value(self) -> u64 {
        let Self(target_index) = self;

        target_index
    }
}

/// The specific granule at which peepholes are open for a target.
///
/// # Representation
///
/// This is represented as an [`prim@u64`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u64)]
pub enum Granule {
    /// A `4 KiB` peephole range.
    ///
    /// This is the minimum size a peephole can be. This can be very expensive, as each peepholed page is
    Page = 4 * 1024 - 1,

    /// A `2 MiB` peephole range.
    ///
    /// This is fairly standard, and is a balanced middle-ground to:
    ///
    /// * Achieve more granular peephole placement.
    /// * Mitigate the inherent costs to set up a peephole, effectively reducing average latency for what would be
    ///   sequential or same-peephole random memory accesses or pointer-chasing.
    #[default]
    Hugepage = 2 * 1024 * 1024 - 1,

    /// A `1 GiB` peephole range.
    ///
    /// It is recommended to use [`Granule::Hugepage`] instead.
    HumongousPage = 1024 * 1024 * 1024 - 1,
}

impl Granule {
    /// Mask off the virtual address for the target granularity.
    ///
    /// This transforms an arbitrary virtual address to the base address where a peephole would reside.
    #[inline]
    pub const fn base(self, target_address: ViAddr) -> ViAddr {
        let ViAddr(target_address) = target_address;

        ViAddr(target_address & !(self as ffi::binding::virtual_address_t))
    }

    /// Mask off the virtual address for the target granularity.
    ///
    /// This transforms an arbitrary virtual address to the in-peephole offset that would be used for access.
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

    /// Determine whether the [`Granule`] is indivisible.
    ///
    /// Peephole-mapped memory operates at the pagetable level, which limits which [`Granule`] can be used peephole sizes.
    #[inline]
    pub const fn indivisible(self) -> bool {
        matches!(self, Self::Page)
    }
}

/// A manager of foreign virtual address to [`Peephole`] mappings.
///
/// An implementor resolves an absolute address in a foreign address space into an [`Access`], the
/// pairing of a [`Peephole`] with the in-window [`Offset`] at which the access lands. A [`Peephole`]
/// is a relatively expensive structure to construct due to the heavy cross-address-space
/// synchronization involved, so an implementor is expected to cache and reuse windows across
/// accesses rather than open a fresh one per request.
///
/// # Logical frame indexing
///
/// An implementor keys its cache on a logical frame index rather than on a raw address. Dividing an
/// address by the granule size, a right-shift by [`Granule::shift`], yields a [`Frame`] analogous to
/// a page frame number that names the granule-sized window an address falls into. Two addresses in
/// the same window share a [`Frame`] and so reuse a single cached [`Peephole`], with the distance
/// between them recovered as the in-window [`Offset`]. Keying on a frame rather than a masked
/// address keeps the cache free of dereferenceable values and hands the backing map a dense,
/// well-distributed key space.
///
/// # Structure constraints
///
/// A [`Peephole`] is both granule-aligned and granule-sized, so an access whose structure runs past
/// its window boundary cannot be served from a single [`Frame`]. Address space layout randomization
/// places a structure at an arbitrary alignment relative to the granule tiling, so whether a given
/// structure straddles a boundary is not known ahead of time and an implementor must account for
/// straddlers deterministically. This imposes implementor-specific constraints on the size of an
/// accessed structure relative to the granule, documented by each implementor.
pub trait Manage {
    /// Resolve an absolute foreign address into an [`Access`] for `U` from an already-open [`Peephole`].
    ///
    /// This is a pure lookup. It quantizes the address into its [`Frame`] and returns [`Some`] when a
    /// cached [`Peephole`] already covers that frame and `U` fits the window from the derived offset,
    /// and [`None`] otherwise. A [`None`] does not mean the address is unmappable, only that no open
    /// window presently serves it.
    ///
    /// # Remarks
    ///
    /// Even on a [`Some`], the downstream [`Foreign`] retrieval is not to be treated as infallible.
    ///
    /// [`Foreign`]: crate::peephole::Foreign
    fn absolute<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated;

    /// Resolve an absolute foreign address into an [`Access`] for `U`, opening a [`Peephole`] on miss.
    ///
    /// This behaves as [`Manage::absolute`] save that, when no cached window covers the address, it
    /// opens a fresh [`Peephole`] over the [`Frame`] through the engaged [`crate::target::Target`] and caches it
    /// before awarding the [`Access`]. A [`None`] denotes that no frame of this manager can contain
    /// `U` at the given address, which arises only when the structure exceeds the implementor size
    /// constraint.
    ///
    /// # Failure
    ///
    /// This may fail when a [`Peephole`] had to be opened and its creation or memory-mapping failed.
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

    /// Determine the [`Granule`] at which this manager tiles the address space into windows.
    ///
    /// The granule is the span and alignment a window is quantized to, so a bulk consumer such as a
    /// scanner sizes and aligns its tiles to the window grid rather than rediscovering it.
    ///
    /// The granule is assumed to be static for the lifetime of a manager instance, because a manager
    /// is constructed at one granularity and never re-tiles, so a caller may read it once and cache
    /// it rather than re-querying per resolution.
    fn granule(&self) -> Granule;
}

/// A structure that indicates the intent of access of foreign-address-space memory for an `U` at the contained offset for a peephole.
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
    /// This is the fallible downstream retrieval alluded to by [`Manage::absolute`], and re-validates
    /// the bounds and alignment of `U` against the peephole window.
    ///
    /// [`Foreign`]: crate::peephole::Foreign
    #[inline]
    pub fn foreign(self) -> Option<Foreign<U>> {
        let Self(target_peephole, target_offset, ..) = self;

        target_peephole.at::<U>(target_offset)
    }
}
