//! A peephole management context.

use std::{io, sync::RwLock};

use core::{alloc::Layout, marker, mem};

use alloc::collections::BTreeMap;

use catalejo_memory::behavior::Unassociated;
use catalejo_sys::ffi;
use dashmap::DashMap;

use crate::{
    address::{ViAddr, ViRange},
    offset::Offset,
    prelude::Peephole,
    target::Target,
};

/// A new-type over an [`usize`] that determines the frame indice of a [`Peephole`] in relative to its respective level.
///
/// A frame is a virtual address divided by a granule size, analogous to a page frame number. It is
/// a logical cell index rather than an address, so it can be neither dereferenced nor mistaken for
/// one, and is meaningful only relative to the [`Rebase`] grid (granule and offset basis) that
/// produced it.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct Frame(pub usize);

impl Frame {
    /// Construct a [`Frame`] from the target logical frame index.
    #[inline]
    pub const fn new(target_index: usize) -> Self {
        Self(target_index)
    }

    /// Determine the encapsulated logical frame index.
    #[inline]
    pub const fn value(self) -> usize {
        let Self(target_index) = self;

        target_index
    }
}

/// The specific granule at which peepholes are open for a target.
///
/// # Representation
///
/// This is represented as an [`prim@u64`] or [`prim@u32`], matching the platform's pointer width for implementation simplicity, and
/// each variant corresponds to the bitwise mask that would be applied to an address to map it to a peephole of the specified bounds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(usize)]
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
    pub const fn size(self) -> usize {
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
    /// opens a fresh [`Peephole`] over the [`Frame`] through the engaged [`Target`] and caches it
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
    pub fn foreign(self) -> Option<crate::peephole::Foreign<U>> {
        let Self(target_peephole, target_offset, ..) = self;

        target_peephole.at::<U>(target_offset)
    }
}

/// A 2-level offset-rebased manager for [`Peephole`] allocation.
///
/// # Implementation-specific Constraints
///
/// This is limited to structures whose size are less than `G / 2` bytes, where
/// `G` is the size of the used [`Granule`].
#[derive(Debug)]
pub struct Rebased {
    /// The engaged target process that is being peepholed into.
    target_engaged: Target,

    /// The granule at which every peephole window is sized.
    peephole_granule: Granule,

    /// The offset-shifted grids of the peephole cascade. (`L0`)
    // NOTE(rationale): Keep the grids inline, as the cascade is at most two (`L0` and the
    // half-granule-shifted `L1`).
    l0: Rebase<0>,

    /// The offset-shifted grids of the peephole cascade. (`L1`)
    ///
    /// This may not exist if the used peephole granule is [`Granule::Page`]
    l1: Option<Rebase<1>>,
}

impl Rebased {
    /// Construct a [`Rebased`] manager with the default [`Granule`].
    #[inline]
    pub fn new(target_engage: Target) -> Self {
        Self::new_with(target_engage, Granule::default())
    }

    /// Construct a [`Rebased`] manager with the specified [`Granule`].
    #[inline]
    pub fn new_with(target_engage: Target, target_granule: Granule) -> Self {
        let (target_engaged, peephole_granule) = (target_engage, target_granule);

        let l0 = Rebase::level_with(target_granule);

        // NOTE: The shifted grid slides by a half-granule, so only build it when that half is itself
        // page-aligned, else its window bases would be sub-page and the kernel would reject them.
        let l1 = if target_granule.indivisible() {
            None
        } else {
            Some(Rebase::level_with(target_granule))
        };

        Self {
            target_engaged,
            peephole_granule,
            l0,
            l1,
        }
    }
}

impl Rebased {
    /// Determine the engaged target for this [`Rebased`].
    #[inline]
    pub const fn engaged(&self) -> &Target {
        let Self { target_engaged, .. } = self;

        target_engaged
    }

    /// Determine the granule used in this [`Rebased`].
    #[inline]
    pub const fn granule(&self) -> Granule {
        let Self {
            peephole_granule, ..
        } = self;

        *peephole_granule
    }

    /// Determine the level-0 [`Rebase`] of the [`Rebased`] manager.
    #[inline]
    pub const fn level0(&self) -> &Rebase<0> {
        let Self { l0, .. } = self;

        l0
    }

    /// Determine the potentially-missing level-1 [`Rebase`] of the [`Rebased`] manager.
    #[inline]
    pub const fn level1(&self) -> Option<&Rebase<1>> {
        let Self { l1, .. } = self;

        l1.as_ref()
    }

    /// Determine whether this [`Rebased`] maintains a shifted (`L1`) grid.
    ///
    /// Only granules whose half is page-aligned (i.e. every non-[`Granule::Page`] granule) can host
    /// a shifted grid, and without one a structure is constrained to never straddle a granule boundary.
    #[inline]
    pub const fn is_cascaded(&self) -> bool {
        let Self { l1, .. } = self;

        l1.is_some()
    }

    /// Determine the largest structure size this [`Rebased`] can peephole without straddling.
    #[inline]
    pub const fn capacity(&self) -> ffi::binding::virtual_address_t {
        self.granule().half()
    }
}

impl Manage for Rebased {
    #[inline]
    fn absolute<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated,
    {
        let Self { l0, l1, .. } = self;

        // NOTE: Prefer the flush grid and fall back to the half-shifted grid only for the structures
        // that straddle an `L0` boundary, of which an indivisible granule has none as it lacks `L1`.
        if let Some(target_access) = l0.resolve::<U>(target_address) {
            Some(target_access)
        } else {
            l1.as_ref().and_then(|l1| l1.resolve(target_address))
        }
    }

    #[inline]
    fn source<U>(&self, target_address: ViAddr) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        // NOTE: A structure larger than a half-granule cannot be guaranteed to fit either grid.
        debug_assert!(
            mem::size_of::<U>() <= self.capacity(),
            "structure exceeds the half-granule peephole cap",
        );

        let Self { l0, l1, .. } = self;

        // NOTE: The flush grid owns every structure that fits it, so the half-shifted grid receives
        // only the straddlers it was slid to re-home, and never a window the flush grid could keep.
        if let Some(target_access) = l0.acquire::<U>(self.engaged(), target_address)? {
            return Ok(Some(target_access));
        }

        match l1 {
            Some(l1) => l1.acquire::<U>(self.engaged(), target_address),
            None => Ok(None),
        }
    }
}

/// A single-rebase mapping in a [`Rebased`] with an offset basis of `O`.
///
/// This is a sub-manager, and use of it directly is discouraged.
#[derive(Debug)]
pub struct Rebase<const O: usize> {
    /// The map of peephole frame to its respective peephole.
    rebase_map: DashMap<Frame, Peephole>,

    /// The granule used for rebase of a peephole.
    ///
    /// This is used with the `O` const-generic parameter, to derive the respective rebase offset to be applied to the [`Peephole`].
    rebase_granule: Granule,
}

impl<const O: usize> Rebase<O> {
    /// Resolve an address into an [`Access`] for `U` from a [`Peephole`] already open in this grid.
    ///
    /// This is the single-grid, lookup-only counterpart used to implement [`Manage::absolute`]. It
    /// yields [`Some`] only when a cached window owns the address' [`Frame`] and `U` is admitted from
    /// the derived offset, and yields [`None`] on a straddle, a misalignment or an absent window.
    #[inline]
    pub fn resolve<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated,
    {
        let target_frame = self.frame(target_address);
        let target_offset = self.offset(target_address);

        // NOTE: The window base is granule-aligned, so `U` is servable from this grid only if it
        // neither straddles the trailing granule boundary nor is misaligned at the derived offset.
        self.admits::<U>(target_offset)
            .then(|| self.find(target_frame))
            .flatten()
            .map(|target_peephole| Access::intent(target_peephole, target_offset))
    }

    /// Resolve an address into an [`Access`] for `U`, opening a [`Peephole`] in this grid on miss.
    ///
    /// This is the single-grid, open-on-miss counterpart used to implement [`Manage::source`]. It
    /// yields [`None`] when `U` would straddle this grid's trailing granule boundary or is misaligned
    /// at the derived offset, signalling that a shifted grid must serve it instead.
    ///
    /// # Failure
    ///
    /// This may fail when a [`Peephole`] had to be opened and its creation or memory-mapping failed.
    #[inline]
    pub fn acquire<U>(
        &self,
        target_engaged: &Target,
        target_address: ViAddr,
    ) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        let target_offset = self.offset(target_address);

        // NOTE: A straddler or a misaligned access is rejected here so the caller can slide it onto
        // a shifted grid rather than open a window that could not serve the whole structure anyway.
        if !self.admits::<U>(target_offset) {
            return Ok(None);
        }

        let target_frame = self.frame(target_address);

        let target_peephole = match self.find(target_frame) {
            Some(target_peephole) => target_peephole,
            None => {
                let target_peephole = Peephole::view(target_engaged, self.range(target_frame))?;

                // NOTE: A concurrent open of the same frame is a benign race, and both windows map
                // the identical foreign span, so the emplaced loser is simply dropped by the caller.
                let _ = self.emplace(target_frame, target_peephole.clone());

                target_peephole
            }
        };

        Ok(Some(Access::intent(target_peephole, target_offset)))
    }
}

impl<const O: usize> Rebase<O> {
    /// Construct a rebase mapping level with the default [`Granule`].
    #[inline]
    pub fn level() -> Self {
        Self::level_with(Granule::default())
    }

    /// Construct a rebase mapping level with the specified [`Granule`].
    #[inline]
    pub fn level_with(target_granule: Granule) -> Self {
        let rebase_map = DashMap::<Frame, Peephole>::new();

        let rebase_granule = target_granule;

        Self {
            rebase_map,
            rebase_granule,
        }
    }
}

impl<const O: usize> Rebase<O> {
    /// Determine the granule used in this [`Rebase`].
    #[inline]
    pub const fn granule(&self) -> Granule {
        let Self { rebase_granule, .. } = self;

        *rebase_granule
    }

    /// Determine the offset basis by which this grid is displaced from the unshifted (`L0`) grid.
    ///
    /// This is `O` half-granules. The level-0 grid is flush with the granule tiling, whereas each
    /// successive level slides by a half-granule to re-home structures that straddle a boundary of
    /// the level below it.
    #[inline]
    pub const fn shift(&self) -> ffi::binding::virtual_address_t {
        O.wrapping_mul(self.granule().half())
    }

    /// Determine the [`Frame`] index that a virtual address falls into within this grid.
    #[inline]
    pub const fn frame(&self, target_address: ViAddr) -> Frame {
        let ViAddr(target_address) = target_address;

        // NOTE: Rebase the address into this grid's origin before quantizing, so the shifted (`L1`)
        // grid indexes from its own displaced basis rather than the absolute address space.
        let framed_address = target_address.wrapping_sub(self.shift());

        Frame(framed_address >> self.granule().shift())
    }

    /// Determine the base virtual address of the window that owns the specified [`Frame`].
    ///
    /// This is the inverse of [`Rebase::frame`], re-materializing the grid-relative frame index
    /// into the absolute, granule-aligned base address of its peephole window.
    #[inline]
    pub const fn base(&self, target_frame: Frame) -> ViAddr {
        let Frame(target_frame) = target_frame;

        let framed_base = target_frame << self.granule().shift();

        ViAddr(framed_base.wrapping_add(self.shift()))
    }

    /// Determine the in-window [`Offset`] of a virtual address within its owning [`Frame`].
    #[inline]
    pub const fn offset(&self, target_address: ViAddr) -> Offset {
        let ViAddr(base_address) = self.base(self.frame(target_address));
        let ViAddr(target_address) = target_address;

        Offset::byte(target_address.wrapping_sub(base_address))
    }

    /// Determine the virtual address range spanned by the window that owns the specified [`Frame`].
    #[inline]
    pub const fn range(&self, target_frame: Frame) -> ViRange {
        let ViAddr(base_address) = self.base(target_frame);

        ViRange {
            start_address: ViAddr(base_address),
            end_address: ViAddr(base_address.wrapping_add(self.granule().size())),
        }
    }

    /// Determine whether an `U` at the specified in-window offset is servable from a single window.
    ///
    /// A structure is admitted only if it neither runs past the trailing granule boundary nor is
    /// misaligned for its layout, and a rejection signals that a shifted grid must serve it instead.
    #[inline]
    pub fn admits<U>(&self, target_offset: Offset) -> bool
    where
        U: Unassociated,
    {
        let target_layout = Layout::new::<U>();

        let displacement_value = target_offset.value();

        let in_bounds = self
            .granule()
            .size()
            .checked_sub(displacement_value)
            .is_some_and(|window_remainder| window_remainder >= target_layout.size());

        // NOTE: The window base is granule-aligned (thus page-aligned), so an offset-only alignment
        // check against the local displacement suffices to establish alignment for `U`.
        let is_aligned = displacement_value.is_multiple_of(target_layout.align());

        in_bounds && is_aligned
    }
}

impl<const O: usize> Rebase<O> {
    /// Look up the [`Peephole`] that currently owns the specified [`Frame`], if any.
    #[inline]
    pub fn find(&self, target_frame: Frame) -> Option<Peephole> {
        let Self { rebase_map, .. } = self;

        rebase_map
            .get(&target_frame)
            .map(|target_entry| Peephole::clone(target_entry.value()))
    }

    /// Insert a [`Peephole`] for the specified [`Frame`], returning the previously-held one.
    ///
    /// The backing map shards its locks per bucket, so an insertion contends only with concurrent
    /// operations that land on the same shard rather than with the map as a whole.
    #[inline]
    pub fn emplace(&self, target_frame: Frame, target_peephole: Peephole) -> Option<Peephole> {
        let Self { rebase_map, .. } = self;

        rebase_map.insert(target_frame, target_peephole)
    }
}

/// An interval-indexed manager that opens one tight, page-aligned [`Peephole`] per observed span.
///
/// # Why this exists
///
/// [`Rebased`] quantizes every access to a granule, so it opens a granule-sized window even for a
/// single word. Against a foreign target the observer's address space is unrelated to the target's
/// and the kernel places the view clear of everything, so the oversized window is harmless. When the
/// observer engages itself the picture changes. The observed granule reaches past the mapped
/// structure into unpopulated address space, and the kernel is then free to place the view inside
/// that unpopulated tail, overlapping the very range it observes. The kernel rejects such a
/// self-overlapping view with `-EINVAL`, because a view over its own observed range can never resolve.
///
/// [`Memoize`] opens a window that covers only the page-aligned span the structure actually occupies.
/// A structure that is itself page resident yields a fully mapped observed range, leaving the kernel
/// no unpopulated hole to place the view into, so the self-peephole resolves. This makes [`Memoize`]
/// the manager to reach for when the observer is its own target. It is a drop-in [`Manage`], so it
/// substitutes for [`Rebased`] without any other change.
///
/// # Memoization
///
/// Open windows are held in an interval tree keyed by their base address. A resolution finds the
/// window whose range covers the whole structure and reuses it, opening a fresh page-aligned window
/// only on a miss. Reuse follows actual coverage rather than a fixed grid, so a structure of any size
/// is served without the straddle handling a granule grid demands.
///
/// # Constraints
///
/// The observed structure must lie within mapped, page-aligned memory. An access whose page-aligned
/// span reaches past the end of a mapping into unpopulated address space reintroduces the very hole
/// this manager avoids, so a self-peepholing caller keeps its observed structures page resident.
#[derive(Debug)]
pub struct Memoize {
    /// The engaged target process that is being peepholed into.
    target_engaged: Target,

    /// The open windows, keyed by base address, forming an interval tree over the observed space.
    window_tree: RwLock<BTreeMap<usize, Peephole>>,
}

impl Memoize {
    /// Construct a [`Memoize`] manager over an engaged target.
    #[inline]
    pub const fn new(target_engage: Target) -> Self {
        Self {
            target_engaged: target_engage,
            window_tree: RwLock::new(BTreeMap::new()),
        }
    }

    /// Determine the engaged target for this [`Memoize`].
    #[inline]
    pub const fn engaged(&self) -> &Target {
        let Self { target_engaged, .. } = self;

        target_engaged
    }

    /// Compute the page-aligned window range a structure of `target_span` bytes at an address occupies.
    ///
    /// Yields [`None`] when the structure would wrap the end of the address space.
    #[inline]
    fn window_range(target_address: ViAddr, target_span: usize) -> Option<ViRange> {
        let page_size = Granule::Page.size();
        let page_mask = page_size - 1;

        let ViAddr(address) = target_address;

        let window_start = address & !page_mask;

        // NOTE: Round the structure end up to a page so the window is page-aligned at both ends,
        // which the kernel requires of every peephole range.
        let structure_end = address.checked_add(target_span)?;
        let window_end = structure_end.checked_add(page_mask)? & !page_mask;

        // NOTE: A zero-size structure lands on an empty range the kernel rejects, so floor the window
        // at a single page.
        let window_end = if window_end <= window_start {
            window_start.checked_add(page_size)?
        } else {
            window_end
        };

        Some(ViRange {
            start_address: ViAddr(window_start),
            end_address: ViAddr(window_end),
        })
    }

    /// Find an open window that already covers the whole structure at an address, if any.
    #[inline]
    fn covering(
        window_tree: &BTreeMap<usize, Peephole>,
        target_address: ViAddr,
        target_span: usize,
    ) -> Option<Peephole> {
        let ViAddr(address) = target_address;

        // NOTE: The greatest base at or below the address is the only window that can own it, as the
        // tree is keyed by base and every window runs contiguously from its base.
        let (_, target_peephole) = window_tree.range(..=address).next_back()?;

        let ViRange {
            end_address: ViAddr(window_end),
            ..
        } = target_peephole.range();

        address
            .checked_add(target_span)
            .is_some_and(|structure_end| structure_end <= window_end)
            .then(|| Peephole::clone(target_peephole))
    }

    /// Award an [`Access`] into a window for a structure at an address the window is known to cover.
    #[inline]
    fn access<U>(target_peephole: Peephole, target_address: ViAddr) -> Access<U>
    where
        U: Unassociated,
    {
        let ViRange {
            start_address: ViAddr(window_start),
            ..
        } = target_peephole.range();

        let ViAddr(address) = target_address;

        Access::intent(target_peephole, Offset::byte(address - window_start))
    }
}

impl Manage for Memoize {
    #[inline]
    fn absolute<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated,
    {
        let target_layout = Layout::new::<U>();

        let target_peephole = {
            let window_tree = self.window_tree.read().unwrap();

            Self::covering(&window_tree, target_address, target_layout.size())?
        };

        // NOTE: Mirror the grid managers and award an access only when `U` is aligned at its offset,
        // leaving the fault-protected read itself as the sole fallible step.
        let ViRange {
            start_address: ViAddr(window_start),
            ..
        } = target_peephole.range();

        let ViAddr(address) = target_address;

        (address - window_start)
            .is_multiple_of(target_layout.align())
            .then(|| Self::access(target_peephole, target_address))
    }

    #[inline]
    fn source<U>(&self, target_address: ViAddr) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        let target_layout = Layout::new::<U>();

        // Reuse an open window that already covers the whole structure.
        if let Some(target_peephole) = {
            let window_tree = self.window_tree.read().unwrap();

            Self::covering(&window_tree, target_address, target_layout.size())
        } {
            return Ok(Some(Self::access(target_peephole, target_address)));
        }

        // NOTE: Open the window outside the lock. The mapping is the expensive step, and holding the
        // tree across it would serialize every unrelated resolution.
        let Some(target_range) = Self::window_range(target_address, target_layout.size()) else {
            return Ok(None);
        };

        let target_peephole = Peephole::view(self.engaged(), target_range)?;

        let target_peephole = {
            let mut window_tree = self.window_tree.write().unwrap();

            // NOTE: A racing open may have covered this span already. Yield to it and let the freshly
            // opened window drop, both map the identical foreign range.
            match Self::covering(&window_tree, target_address, target_layout.size()) {
                Some(existing_peephole) => existing_peephole,
                None => {
                    let ViRange {
                        start_address: ViAddr(window_start),
                        ..
                    } = target_range;

                    window_tree.insert(window_start, Peephole::clone(&target_peephole));

                    target_peephole
                }
            }
        };

        Ok(Some(Self::access(target_peephole, target_address)))
    }
}
