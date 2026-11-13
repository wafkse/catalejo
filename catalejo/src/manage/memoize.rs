//! Page-aligned windows sized to requested address spans.

use std::{io, sync::RwLock};

use alloc::collections::BTreeMap;

use core::alloc::Layout;

use catalejo_memory::behavior::Unassociated;
use catalejo_sys::ffi;

use crate::{
    address::{ViAddr, ViRange},
    manage::{Access, Granule, Manage},
    offset::Offset,
    peephole::Peephole,
    target::Target,
};

/// Cache page-aligned windows sized to the requested spans.
///
/// A request reuses a cached window if that window covers the value. Otherwise,
/// the manager opens a window around the value's page-aligned extent. Values
/// are not subject to the half-granule size limit of [`crate::manage::Rebased`].
///
/// # Self-observation
///
/// This placement is useful for self-observation. A large fixed window can
/// include unmapped target addresses where the kernel might place the observer
/// mapping. If the two ranges overlap, Mirilla rejects the mapping with
/// `EINVAL`. A tight window around mapped target pages avoids that placement.
///
/// For self-observation, the requested page-aligned span must therefore stay
/// within mapped memory. Extending it into an unmapped gap can still allow an
/// overlapping placement and fail.
///
/// # Cache lifetime
///
/// Cached windows remain owned by the manager until it is dropped. Use
/// [`crate::manage::lru::Lru`] when the cache needs a capacity limit.
#[derive(Debug)]
pub struct Memoize {
    /// The engaged target process that is being peepholed into.
    target_engaged: Target,

    /// The open windows, keyed by base address, forming an interval tree over the observed space.
    window_tree: RwLock<BTreeMap<ffi::binding::virtual_address_t, Peephole>>,
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
    fn window_range(target_address: ViAddr, target_span: u64) -> Option<ViRange> {
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
        window_tree: &BTreeMap<ffi::binding::virtual_address_t, Peephole>,
        target_address: ViAddr,
        target_span: u64,
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

    /// Create an access intent for a value already known to fit within the window.
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

            Self::covering(
                &window_tree,
                target_address,
                target_layout.size() as ffi::binding::virtual_size_t,
            )?
        };

        // NOTE: Mirror the grid managers and award an access only when `U` is aligned at its offset,
        // leaving the fault-protected read itself as the sole fallible step.
        let ViRange {
            start_address: ViAddr(window_start),
            ..
        } = target_peephole.range();

        let ViAddr(address) = target_address;

        (address - window_start)
            .is_multiple_of(target_layout.align() as ffi::binding::virtual_align_t)
            .then(|| Self::access(target_peephole, target_address))
    }

    #[inline]
    fn source<U>(&self, target_address: ViAddr) -> io::Result<Option<Access<U>>>
    where
        U: Unassociated,
    {
        let target_layout = Layout::new::<U>();

        let Self { window_tree, .. } = self;

        // Reuse an open window that already covers the whole structure.
        if let Some(target_peephole) = {
            let window_tree = window_tree.read().expect("the lock is poisoned");

            Self::covering(
                &window_tree,
                target_address,
                target_layout.size() as ffi::binding::virtual_size_t,
            )
        } {
            return Ok(Some(Self::access(target_peephole, target_address)));
        }

        // NOTE: Open the window outside the lock. The mapping is the expensive step, and holding the
        // tree across it would serialize every unrelated resolution.
        let Some(target_range) = Self::window_range(
            target_address,
            target_layout.size() as ffi::binding::virtual_size_t,
        ) else {
            return Ok(None);
        };

        let target_peephole = Peephole::view(self.engaged(), target_range)?;

        let target_peephole = {
            let mut window_tree = window_tree.write().expect("the lock is poisoned");

            // NOTE: A racing open may have covered this span already. Yield to it and let the freshly
            // opened window drop, both map the identical foreign range.
            match Self::covering(
                &window_tree,
                target_address,
                target_layout.size() as ffi::binding::virtual_size_t,
            ) {
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

    #[inline]
    fn granule(&self) -> Granule {
        // NOTE: Memoize sizes every window to its structure and page-aligns it, so the finest
        // granularity it guarantees is a page. Window sizes depend on the requested span.
        Granule::Page
    }
}
