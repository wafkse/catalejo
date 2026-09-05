//! Bounded least-recently-used logical frame retention.
//!
//! [`Lru`] mirrors the two-level rebase geometry of [`crate::manage::rebased::Rebased`]. Each level
//! discovers live peepholes through a concurrent weak [`Frame`] index while one global [`LruCache`]
//! owns the manager's bounded strong retention.

use alloc::sync::{Arc, Weak};

use core::{alloc::Layout, mem, num::NonZeroUsize};

use std::{
    io,
    sync::{Mutex, MutexGuard},
};

use catalejo_memory::behavior::Unassociated;
use catalejo_sys::ffi;
use dashmap::{DashMap, mapref::entry::Entry};
use lru::LruCache;

use crate::{
    address::{ViAddr, ViRange},
    manage::{Access, Frame, Granule, Manage},
    offset::Offset,
    peephole::{Peephole, PeepholeContext},
    target::Target,
};

/// The logical rebase level that owns one frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum Level {
    /// The flush rebase grid.
    L0,

    /// The half-granule-shifted rebase grid.
    L1,
}

impl Level {
    /// Determine this level's displacement from the flush grid.
    #[inline]
    const fn shift(self, target_granule: Granule) -> ffi::binding::virtual_address_t {
        match self {
            Self::L0 => 0,
            Self::L1 => Granule::half(target_granule),
        }
    }
}

/// The global LRU identity of one logical frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
// NOTE(invariant): the level distinguishes equal frame indices from the flush and shifted grids, so one key identifies exactly one rebase window.
struct WindowKey(Level, Frame);

/// One weak logical rebase level used by [`Lru`].
#[derive(Debug)]
// NOTE(invariant): `rebase_map` stores only weak peephole ownership keyed by frames derived from `rebase_granule` and `rebase_level`, so discovery never extends peephole lifetime.
struct LruRebase {
    /// The weak peephole index keyed by logical frame.
    rebase_map: DashMap<Frame, Weak<PeepholeContext>>,

    /// The granule used to derive frames and window ranges.
    rebase_granule: Granule,

    /// The flush or shifted grid represented by this level.
    rebase_level: Level,
}

impl LruRebase {
    /// Construct one empty weak rebase level.
    #[inline]
    fn level_with(target_granule: Granule, target_level: Level) -> Self {
        let rebase_map = DashMap::new();
        let rebase_granule = target_granule;
        let rebase_level = target_level;

        Self {
            rebase_map,
            rebase_granule,
            rebase_level,
        }
    }

    /// Determine the granule used by this level.
    #[inline]
    const fn granule(&self) -> Granule {
        let Self { rebase_granule, .. } = self;

        *rebase_granule
    }

    /// Determine this level's offset basis.
    #[inline]
    const fn shift(&self) -> ffi::binding::virtual_address_t {
        let Self {
            rebase_granule,
            rebase_level,
            ..
        } = self;

        Level::shift(*rebase_level, *rebase_granule)
    }

    /// Determine the logical frame that owns one virtual address.
    #[inline]
    const fn frame(&self, target_address: ViAddr) -> Frame {
        let ViAddr(target_address) = target_address;
        let target_framed = target_address.wrapping_sub(Self::shift(self));

        Frame(target_framed >> Granule::shift(Self::granule(self)))
    }

    /// Determine the window base represented by one logical frame.
    #[inline]
    const fn base(&self, target_frame: Frame) -> ViAddr {
        let Frame(target_frame) = target_frame;
        let target_base = target_frame << Granule::shift(Self::granule(self));

        ViAddr(target_base.wrapping_add(Self::shift(self)))
    }

    /// Determine the in-window offset of one virtual address.
    #[inline]
    const fn offset(&self, target_address: ViAddr) -> Offset {
        let ViAddr(target_base) = Self::base(self, Self::frame(self, target_address));
        let ViAddr(target_address) = target_address;

        Offset::byte(target_address.wrapping_sub(target_base))
    }

    /// Determine the foreign range represented by one logical frame.
    #[inline]
    const fn range(&self, target_frame: Frame) -> ViRange {
        let ViAddr(target_base) = Self::base(self, target_frame);
        let target_end = target_base.wrapping_add(Granule::size(Self::granule(self)));

        ViRange {
            start_address: ViAddr(target_base),
            end_address: ViAddr(target_end),
        }
    }

    /// Determine whether `U` can be served from this level at one offset.
    #[inline]
    fn admits<U>(&self, target_offset: Offset) -> bool
    where
        U: Unassociated,
    {
        let target_layout = Layout::new::<U>();
        let target_displacement = target_offset.value();
        let target_in_bounds = Granule::size(Self::granule(self))
            .checked_sub(target_displacement)
            .is_some_and(|target_remainder| {
                target_remainder >= target_layout.size() as ffi::binding::virtual_size_t
            });
        let target_aligned = target_displacement
            .is_multiple_of(target_layout.align() as ffi::binding::virtual_align_t);

        target_in_bounds && target_aligned
    }

    /// Determine the global retention key for one frame in this level.
    #[inline]
    const fn key(&self, target_frame: Frame) -> WindowKey {
        let Self { rebase_level, .. } = self;

        WindowKey(*rebase_level, target_frame)
    }

    /// Resolve one weak frame entry and remove it once its peephole is dead.
    #[inline]
    fn find(&self, target_frame: Frame) -> Option<Peephole> {
        let Self { rebase_map, .. } = self;
        let target_weak = rebase_map
            .get(&target_frame)
            .map(|target_entry| target_entry.value().clone())?;

        match target_weak.upgrade().map(Peephole) {
            Some(target_peephole) => Some(target_peephole),
            None => {
                let _ = rebase_map.remove_if(&target_frame, |_, target_current| {
                    Weak::ptr_eq(target_current, &target_weak)
                });

                None
            }
        }
    }

    /// Point one frame at the selected live peephole allocation.
    #[inline]
    fn remember(&self, target_frame: Frame, target_peephole: &Peephole) {
        let Self { rebase_map, .. } = self;
        let Peephole(target_handle) = target_peephole;

        let target_weak = Arc::downgrade(target_handle);

        let _ = rebase_map.insert(target_frame, target_weak);
    }

    /// Remove one weak frame entry only when it still refers to the evicted allocation.
    #[inline]
    fn forget(&self, target_frame: Frame, target_weak: &Weak<PeepholeContext>) {
        let Self { rebase_map, .. } = self;

        let _ = rebase_map.remove_if(&target_frame, |_, target_current| {
            Weak::ptr_eq(target_current, target_weak)
        });
    }

    /// Resolve an already-live frame without changing LRU recency.
    #[inline]
    fn resolve<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated,
    {
        let target_frame = Self::frame(self, target_address);
        let target_offset = Self::offset(self, target_address);
        let target_admitted = Self::admits::<U>(self, target_offset);

        match target_admitted {
            true => Self::find(self, target_frame)
                .map(|target_peephole| Access::intent(target_peephole, target_offset)),
            false => None,
        }
    }

    /// Resolve one frame through the global LRU and open a peephole on a true miss.
    fn acquire<U>(
        &self,
        target_manager: &Lru,
        target_address: ViAddr,
    ) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        let target_offset = Self::offset(self, target_address);

        if Self::admits::<U>(self, target_offset) {
            let target_frame = Self::frame(self, target_address);
            let target_key = Self::key(self, target_frame);
            let target_cached = Self::find(self, target_frame)
                .or_else(|| Lru::retained(target_manager, target_key));

            let target_peephole = match target_cached {
                Some(target_peephole) => target_peephole,
                None => {
                    let Self { rebase_map, .. } = self;

                    match rebase_map.entry(target_frame) {
                        Entry::Occupied(mut target_entry) => {
                            match target_entry.get().upgrade().map(Peephole) {
                                Some(target_peephole) => target_peephole,
                                None => {
                                    Lru::reserve(target_manager);

                                    let target_range = Self::range(self, target_frame);
                                    let target_peephole =
                                        Peephole::view(Lru::engaged(target_manager), target_range)?;
                                    let Peephole(target_handle) = &target_peephole;
                                    let target_weak = Arc::downgrade(target_handle);

                                    let _ = target_entry.insert(target_weak);

                                    target_peephole
                                }
                            }
                        }
                        Entry::Vacant(target_entry) => {
                            Lru::reserve(target_manager);

                            let target_range = Self::range(self, target_frame);
                            let target_peephole =
                                Peephole::view(Lru::engaged(target_manager), target_range)?;
                            let Peephole(target_handle) = &target_peephole;
                            let target_weak = Arc::downgrade(target_handle);

                            target_entry.insert(target_weak);

                            target_peephole
                        }
                    }
                }
            };

            let target_peephole = Lru::retain(target_manager, target_key, target_peephole);

            Self::remember(self, target_frame, &target_peephole);

            Ok(Some(Access::intent(target_peephole, target_offset)))
        } else {
            Ok(None)
        }
    }
}

/// A bounded two-level offset-rebased peephole manager with one global LRU order.
///
/// Placement matches [`crate::manage::rebased::Rebased`]. Weak frame maps support concurrent
/// discovery while the global LRU owns at most [`Lru::limit`] strong peephole references.
#[derive(Debug)]
// NOTE(invariant): `retained` owns at most `peephole_limit` strong peephole references, `l0` and `l1` store only weak references, and every retained key identifies one logical frame in exactly one rebase level.
pub struct Lru {
    /// The engaged target process.
    target_engaged: Target,

    /// The granule used by both logical rebase levels.
    peephole_granule: Granule,

    /// The exact manager-owned strong retention bound.
    peephole_limit: NonZeroUsize,

    /// The flush logical frame index.
    l0: LruRebase,

    /// The optional half-granule-shifted logical frame index.
    l1: Option<LruRebase>,

    /// The complete manager-owned strong retention and global recency order.
    cache_storage: Mutex<LruCache<WindowKey, Peephole>>,
}

impl Lru {
    /// Construct an LRU manager with the default granule.
    #[inline]
    #[must_use]
    pub fn new(target_engage: Target, target_limit: NonZeroUsize) -> Self {
        Self::new_with(target_engage, Granule::default(), target_limit)
    }

    /// Construct an LRU manager with the specified granule.
    #[inline]
    #[must_use]
    pub fn new_with(
        target_engage: Target,
        target_granule: Granule,
        target_limit: NonZeroUsize,
    ) -> Self {
        let target_engaged = target_engage;
        let peephole_granule = target_granule;
        let peephole_limit = target_limit;
        let l0 = LruRebase::level_with(target_granule, Level::L0);

        let l1 = if Granule::indivisible(target_granule) {
            None
        } else {
            Some(LruRebase::level_with(target_granule, Level::L1))
        };

        let cache_storage = Mutex::new(LruCache::new(target_limit));

        Self {
            target_engaged,
            peephole_granule,
            peephole_limit,
            l0,
            l1,
            cache_storage,
        }
    }

    /// Determine the engaged target.
    #[inline]
    #[must_use]
    pub const fn engaged(&self) -> &Target {
        let Self { target_engaged, .. } = self;

        target_engaged
    }

    /// Determine the granule used by this manager.
    #[inline]
    #[must_use]
    pub const fn granule(&self) -> Granule {
        let Self {
            peephole_granule, ..
        } = self;

        *peephole_granule
    }

    /// Determine the exact manager-owned strong retention bound.
    #[inline]
    #[must_use]
    pub const fn limit(&self) -> NonZeroUsize {
        let Self { peephole_limit, .. } = self;

        *peephole_limit
    }

    /// Determine the largest structure size this manager can peephole without straddling.
    #[inline]
    #[must_use]
    pub const fn capacity(&self) -> ffi::binding::virtual_address_t {
        Granule::half(Self::granule(self))
    }

    /// Determine whether this manager maintains a shifted logical frame index.
    #[inline]
    #[must_use]
    pub const fn is_cascaded(&self) -> bool {
        let Self { l1, .. } = self;

        l1.is_some()
    }

    /// Lock the global strong-retention LRU.
    #[inline]
    fn lock_retained(&self) -> MutexGuard<'_, LruCache<WindowKey, Peephole>> {
        let Self {
            cache_storage: retained,
            ..
        } = self;

        match retained.lock() {
            Ok(target_guard) => target_guard,
            Err(target_poisoned) => target_poisoned.into_inner(),
        }
    }

    /// Find one retained peephole and promote it to most recently used.
    #[inline]
    fn retained(&self, target_key: WindowKey) -> Option<Peephole> {
        let mut target_retained = Self::lock_retained(self);

        target_retained.get(&target_key).cloned()
    }

    /// Select one eviction when the global retention set is full.
    fn make_room(
        target_retained: &mut LruCache<WindowKey, Peephole>,
    ) -> Option<(WindowKey, Peephole)> {
        let target_full = target_retained.len() == target_retained.cap().get();

        match target_full {
            false => None,
            true => {
                let target_reclaimable = target_retained
                    .iter()
                    .filter(|(_, target_peephole)| {
                        let Peephole(target_handle) = target_peephole;

                        Arc::strong_count(target_handle) == 1
                    })
                    .map(|(target_key, _)| *target_key)
                    .next_back();

                match target_reclaimable {
                    Some(target_key) => target_retained.pop_entry(&target_key),
                    None => target_retained.pop_lru(),
                }
            }
        }
    }

    /// Free one manager retention slot before opening another operating-system peephole.
    fn reserve(&self) {
        let target_evicted = {
            let mut target_retained = Self::lock_retained(self);

            Self::make_room(&mut target_retained)
        };

        Self::release(self, target_evicted);
    }

    /// Retain one peephole as most recently used and release a capacity eviction afterward.
    fn retain(&self, target_key: WindowKey, target_peephole: Peephole) -> Peephole {
        let (target_selected, target_evicted) = {
            let mut target_retained = Self::lock_retained(self);

            match target_retained.get(&target_key) {
                Some(target_existing) => (Peephole::clone(target_existing), None),
                None => {
                    let target_selected = Peephole::clone(&target_peephole);
                    let target_evicted = Self::make_room(&mut target_retained);
                    let target_previous = target_retained.put(target_key, target_peephole);

                    debug_assert!(target_previous.is_none());

                    (target_selected, target_evicted)
                }
            }
        };

        Self::release(self, target_evicted);

        target_selected
    }

    /// Drop one evicted strong owner outside the retention mutex and clean dead weak metadata.
    fn release(&self, target_evicted: Option<(WindowKey, Peephole)>) {
        match target_evicted {
            None => {}
            Some((target_key, target_peephole)) => {
                let Peephole(target_handle) = &target_peephole;
                let target_weak = Arc::downgrade(target_handle);

                drop(target_peephole);

                if target_weak.strong_count() == 0 {
                    let WindowKey(target_level, target_frame) = target_key;
                    let Self { l0, l1, .. } = self;

                    match target_level {
                        Level::L0 => LruRebase::forget(l0, target_frame, &target_weak),
                        Level::L1 => match l1 {
                            Some(target_l1) => {
                                LruRebase::forget(target_l1, target_frame, &target_weak);
                            }
                            None => unreachable!("an L1 key requires a cascaded manager"),
                        },
                    }
                }
            }
        }
    }
}

impl Manage for Lru {
    #[inline]
    fn absolute<U>(&self, target_address: ViAddr) -> Option<Access<U>>
    where
        U: Unassociated,
    {
        let Self { l0, l1, .. } = self;

        match LruRebase::resolve::<U>(l0, target_address) {
            Some(target_access) => Some(target_access),
            None => l1
                .as_ref()
                .and_then(|target_l1| LruRebase::resolve::<U>(target_l1, target_address)),
        }
    }

    #[inline]
    fn source<U>(&self, target_address: ViAddr) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        #[cfg(feature = "stealth-mode")]
        debug_assert!(mem::size_of::<U>() as u64 <= Lru::capacity(self));

        #[cfg(not(feature = "stealth-mode"))]
        debug_assert!(
            mem::size_of::<U>() as u64 <= Lru::capacity(self),
            "structure exceeds the half-granule peephole cap",
        );

        let Self { l0, l1, .. } = self;
        let target_l0 = LruRebase::acquire::<U>(l0, self, target_address)?;

        match target_l0 {
            Some(target_access) => Ok(Some(target_access)),
            None => match l1 {
                Some(target_l1) => LruRebase::acquire::<U>(target_l1, self, target_address),
                None => Ok(None),
            },
        }
    }

    #[inline]
    fn granule(&self) -> Granule {
        Self::granule(self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manage::rebased::Rebase;

    #[test]
    fn l0_geometry_matches_rebased() {
        let target_granule = Granule::Hugepage;
        let target_reference = Rebase::<0>::level_with(target_granule);
        let target_lru = LruRebase::level_with(target_granule, Level::L0);
        let target_address = ViAddr::new(0x32_1234);
        let target_frame = Rebase::frame(&target_reference, target_address);
        let target_offset = Rebase::offset(&target_reference, target_address);

        assert_eq!(LruRebase::frame(&target_lru, target_address), target_frame);
        assert_eq!(
            LruRebase::base(&target_lru, target_frame),
            Rebase::base(&target_reference, target_frame)
        );
        assert_eq!(
            LruRebase::offset(&target_lru, target_address),
            target_offset
        );
        assert_eq!(
            LruRebase::range(&target_lru, target_frame),
            Rebase::range(&target_reference, target_frame)
        );
        assert_eq!(
            LruRebase::admits::<u64>(&target_lru, target_offset),
            Rebase::admits::<u64>(&target_reference, target_offset)
        );
    }

    #[test]
    fn l1_geometry_matches_rebased() {
        let target_granule = Granule::Hugepage;
        let target_reference = Rebase::<1>::level_with(target_granule);
        let target_lru = LruRebase::level_with(target_granule, Level::L1);
        let target_address = ViAddr::new(0x1f_fff8);
        let target_frame = Rebase::frame(&target_reference, target_address);
        let target_offset = Rebase::offset(&target_reference, target_address);

        assert_eq!(LruRebase::frame(&target_lru, target_address), target_frame);
        assert_eq!(
            LruRebase::base(&target_lru, target_frame),
            Rebase::base(&target_reference, target_frame)
        );
        assert_eq!(
            LruRebase::offset(&target_lru, target_address),
            target_offset
        );
        assert_eq!(
            LruRebase::range(&target_lru, target_frame),
            Rebase::range(&target_reference, target_frame)
        );
        assert_eq!(
            LruRebase::admits::<[u8; 16]>(&target_lru, target_offset),
            Rebase::admits::<[u8; 16]>(&target_reference, target_offset)
        );
    }
}
