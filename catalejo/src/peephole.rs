//! In-memory peephole module.

use alloc::sync::Arc;
use core::{alloc::Layout, marker, num::NonZero, ptr::NonNull};
use std::{
    io::{self, ErrorKind},
    os::fd::{AsFd, OwnedFd},
};

use catalejo_fault::{
    behavior::Faultable,
    ffi::Subsystem,
    maybe::{MaybeFault, Opaque},
};
use catalejo_sys::{
    ffi::{self},
    id::PeepholeId,
};
use nix::sys::mman::{MapFlags, ProtFlags};

use crate::{
    address::{Offset, ViAddr, ViRange},
    target::Target,
};

/// A handle to a peephole into a foreign memory address space.
#[derive(Debug)]
pub struct Peephole {
    /// The peephole instance file descriptor.
    peephole_file: OwnedFd,

    /// The peephole identifier.
    peephole_id: PeepholeId,

    /// The foreign virtual address range of this peephole.
    address_range: ViRange,

    /// The lazily memory-mapped area of the peephole.
    target_window: Arc<Window>,
}

impl Peephole {
    /// Create a peephole into the target at the specified virtual address range.
    #[inline]
    pub fn view(target_context: Target, address_range: ViRange) -> io::Result<Self> {
        let ViRange {
            start_address: ViAddr(start_address),
            end_address: ViAddr(end_address),
        } = address_range;

        // SAFETY: The device file descriptor is sourced from a `Target`, so this is safe.
        let (peephole_id, peephole_file) = unsafe {
            ffi::command::peephole(
                target_context.device(),
                target_context.id(),
                start_address,
                end_address,
            )?
        };

        let target_window = {
            let region_size = address_range
                .size()
                .ok_or(io::Error::from(ErrorKind::InvalidInput))?;

            // SAFETY: A private, full-length, zero-offset, read-only mapping of the peephole
            // file, which is exactly what the kernel module requires; it validates the
            // parameters and rejects (`-EINVAL`/`-EACCES`) anything else.
            let target_value = unsafe {
                nix::sys::mman::mmap(
                    None,
                    region_size,
                    ProtFlags::PROT_READ,
                    MapFlags::MAP_PRIVATE,
                    peephole_file.as_fd(),
                    0,
                )?
            };

            let base_address = MaybeFault::new(target_value.expose_provenance());

            Arc::new(Window {
                base_address,
                region_size,
            })
        };

        Ok(Self {
            peephole_file,
            peephole_id,
            address_range,
            target_window,
        })
    }

    /// Duplicate the handle to the peephole.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole file descriptor failed to be duplicated.
    #[inline]
    pub fn duplicate(&self) -> io::Result<Self> {
        let &Self {
            ref peephole_file,
            ref target_window,
            peephole_id,
            address_range,
            ..
        } = self;

        let peephole_file = OwnedFd::try_clone(peephole_file)?;

        let target_window = Arc::clone(target_window);

        Ok(Self {
            peephole_file,
            peephole_id,
            address_range,
            target_window,
        })
    }
}

impl Peephole {
    /// Fabricate a [`Foreign`] handle for `F` at the specified offset from the peephole base address.
    #[inline]
    pub fn at<F>(&self, target_displacement: Offset) -> Option<Foreign<'_, F>>
    where
        F: Faultable,
    {
        let Peephole { target_window, .. } = self;

        let target_layout = Layout::new::<F>();

        let displacement_value = target_displacement.value();

        let in_bounds = NonZero::<usize>::get(Window::size(target_window))
            .checked_sub(displacement_value)?
            >= target_layout.size();

        // NOTE: The window base is page-aligned (the kernel `-EINVAL`s a non-page-aligned
        // peephole before it is ever created), so a displacement-only alignment check suffices.
        let is_aligned = displacement_value.is_multiple_of(target_layout.align());

        if in_bounds && is_aligned {
            Some(Foreign(self, target_displacement, marker::PhantomData))
        } else {
            None
        }
    }
}

/// A memory-mapped peephole virtual memory window.
#[derive(Debug)]
pub struct Window {
    /// The base address of the memory-mapped peephole region.
    ///
    /// This is a maybe-fault region, and is protected against
    /// memory-access-related synchronous hardware exceptions (e.g.,
    /// #GP, unhandled #PF forwarded to userspace via `SIGSEGV`) that
    /// are exposed in a clear manner to the offender thread of execution.
    base_address: MaybeFault<Opaque>,

    /// The size of the memory-mapped region.
    region_size: NonZero<usize>,
}

impl Window {
    /// Determine the base address of the window.
    #[inline]
    pub const fn address(&self) -> NonZero<usize> {
        let &Self { base_address, .. } = self;

        MaybeFault::address(base_address)
    }

    /// Determine the size of the window.
    #[inline]
    pub const fn size(&self) -> NonZero<usize> {
        let &Self { region_size, .. } = self;

        region_size
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        let &mut Self {
            base_address,
            region_size,
            ..
        } = self;

        let base_address =
            // SAFETY: The provenance of this pointer was exposed at the time of `mmap`.
            NonNull::with_exposed_provenance(MaybeFault::address(base_address));

        // SAFETY: The program memory-mapped a region at the provided base address of the same size.
        let _ = unsafe { nix::sys::mman::munmap(base_address, NonZero::<usize>::get(region_size)) };
    }
}

/// A [`Faultable`]-family type located in a foreign address space.
#[derive(Debug, Clone, Copy)]
// NOTE(invariant): Offset is in-bounds and properly aligned for `F`.
pub struct Foreign<'a, F>(&'a Peephole, Offset, marker::PhantomData<F>)
where
    F: Faultable;

impl<'a, F> Foreign<'a, F>
where
    F: Faultable,
{
    /// Attempt to read a [`Faultable`] `F` from the foreign address space.
    ///
    /// This is a fault-protected, machine-word-coherent read of the foreign window. A [`Some`]
    /// holds the value observed at the instant of the read; a [`None`] denotes that the read
    /// faulted, i.e. the peephole was dead (its pages reclaimed by the kernel) at that instant.
    #[inline]
    pub fn read(&self, target_subsystem: Subsystem) -> Option<F> {
        let Self(target_peephole, target_displacement, ..) = self;

        let Peephole { target_window, .. } = target_peephole;

        let target_address =
            Window::address(target_window).checked_add(Offset::value(target_displacement))?;

        // SAFETY:
        //
        // * `F` is `Faultable`, so every bit-pattern read back is a valid value.
        //
        // * `Self`'s invariant guarantees the displacement is in-bounds and aligned for `F`, so
        //   `target_address` lies within the live mapping, kept mapped for `'a` by the
        //   `Arc<Window>` borrowed through the peephole; the foreign window is ordinary RAM,
        //   never side-effecting MMIO.
        //
        // * A dead peephole faults and is reported as `None` rather than being undefined behavior.
        unsafe { MaybeFault::<F>::new(target_address).read(target_subsystem) }
    }
}
