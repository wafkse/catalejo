//! The home module for the [`Memoize`] manager.

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

/// An interval-indexed manager that opens one tight, page-aligned [`Peephole`] per observed span.
///
/// # Why this exists
///
/// [`crate::manage::rebased::Rebased`] quantizes every access to a granule, so it opens a granule-sized window even for a
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
/// substitutes for [`crate::manage::rebased::Rebased`] without any other change.
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
        // granularity it statically guarantees is a single page rather than a fixed larger window.
        Granule::Page
    }
}
