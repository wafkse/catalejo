//! A peephole management context.

use std::io;

use catalejo_sys::ffi;

use dashmap::DashMap;

use crate::{
    address::{Offset, ViAddr, ViRange},
    prelude::Peephole,
    target::Target,
};

/// The specific granularity at which peepholes are open for a target.
///
/// # Representation
///
/// This is represented as an [`prim@u64`] or [`prim@u32`], matching the platform's pointer width for implementation simplicity, and
/// each variant corresponds to the bitwise mask that would be applied to an address to map it to a peephole of the specified bounds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[cfg_attr(target_pointer_width = "32", repr(u32))]
#[cfg_attr(target_pointer_width = "64", repr(u64))]
pub enum Granunality {
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
    /// It is recommended to use [`Granularity::Hugepage`] instead.
    HumongousPage = 1 * 1024 * 1024 * 1024 - 1,
}

impl Granunality {
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
    pub const fn size(self) -> ffi::binding::virtual_address_t {
        // NOTE: The discriminant is stored as the bitwise mask of the intra-peephole offset.
        // Its a power of two, so it can be added 1 to determine the size.
        self as ffi::binding::virtual_address_t + 1
    }
}

/// A managed peephole set for a specific [`Target`].
#[derive(Debug)]
pub struct Manager(Target, DashMap<ViAddr, Peephole>, Granunality);

impl Manager {
    /// Construct a managed peephole set of the specified target context.
    #[inline]
    pub fn manage(target_context: Target) -> Self {
        Self::manage_within(target_context, Granunality::default())
    }

    /// Construct a managed peephole set of the specified target context, within the specified granularity.
    #[inline]
    pub fn manage_within(target_context: Target, peephole_granularity: Granunality) -> Self {
        Self(target_context, DashMap::new(), peephole_granularity)
    }
}

impl Manager {
    /// Find the peephole relevant to the target address, invoking a closure with the peephole and local offset if found.
    #[inline]
    pub fn find<T>(
        &self,
        target_address: ViAddr,
        target_closure: impl FnOnce(&Peephole, Offset) -> T,
    ) -> Option<T> {
        let &Self(.., ref target_mapping, peephole_size) = self;

        let (peephole_base, peephole_offset) = (
            peephole_size.base(target_address),
            Offset::byte(peephole_size.offset(target_address).unwrap() as _),
        );

        if let Some(target_peephole) = target_mapping.get(&peephole_base) {
            Some(target_closure(target_peephole.value(), peephole_offset))
        } else {
            None
        }
    }

    /// Find or emplace (as in, create) the peephole relevant to the target address, invoking a closure with the peephole and local offset.
    #[inline]
    pub fn find_or_emplace<T>(
        &self,
        target_address: ViAddr,
        target_closure: impl FnOnce(&Peephole, Offset) -> T,
    ) -> io::Result<T> {
        let &Self(ref target_context, ref target_mapping, peephole_size) = self;

        let (peephole_base, peephole_offset) = (
            peephole_size.base(target_address),
            Offset::byte(peephole_size.offset(target_address).unwrap() as _),
        );

        if let Some(target_peephole) = target_mapping.get(&peephole_base) {
            Ok(target_closure(target_peephole.value(), peephole_offset))
        } else {
            let end_address = ViAddr(peephole_base.unwrap().wrapping_add(peephole_size.size()));

            let address_range = ViRange {
                start_address: peephole_base,
                end_address,
            };

            let target_peephole = Peephole::view(target_context, address_range)?;

            let target_value = target_closure(&target_peephole, peephole_offset);

            let _ = target_mapping.insert(peephole_base, target_peephole);

            Ok(target_value)
        }
    }
}
