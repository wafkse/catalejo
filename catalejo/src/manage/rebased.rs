//! The home module for the [`Rebased`] manager.

use core::{alloc::Layout, mem};

use std::io;

use catalejo_memory::behavior::Unassociated;
use catalejo_sys::ffi;
use dashmap::DashMap;

use crate::{
    address::{ViAddr, ViRange},
    manage::{Access, Frame, Granule, Manage},
    offset::Offset,
    peephole::Peephole,
    target::Target,
};

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
        #[cfg(feature = "stealth-mode")]
        debug_assert!(mem::size_of::<U>() as u64 <= self.capacity());

        #[cfg(not(feature = "stealth-mode"))]
        debug_assert!(
            mem::size_of::<U>() as u64 <= self.capacity(),
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

    #[inline]
    fn granule(&self) -> Granule {
        Rebased::granule(self)
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
        (O as ffi::binding::virtual_address_t).wrapping_mul(self.granule().half())
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
            .is_some_and(|window_remainder| {
                window_remainder >= target_layout.size() as ffi::binding::virtual_size_t
            });

        // NOTE: The window base is granule-aligned (thus page-aligned), so an offset-only alignment
        // check against the local displacement suffices to establish alignment for `U`.
        let is_aligned = displacement_value
            .is_multiple_of(target_layout.align() as ffi::binding::virtual_align_t);

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
