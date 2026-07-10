//! In-memory peephole module.
#![allow(
    clippy::std_instead_of_core,
    reason = "imports are false-flagged by clippy where the fix would be nightly-only"
)]

use alloc::sync::Arc;

use core::{
    alloc::Layout,
    borrow::Borrow,
    marker, mem,
    num::NonZero,
    ops::Deref,
    ptr::{self, NonNull},
};

use std::{
    io,
    io::ErrorKind,
    os::fd::{AsFd, OwnedFd},
};

use catalejo_memory::{behavior::Immortal, prelude::Unassociated};

use catalejo_fault::{
    behavior::Faultable,
    ffi::{self as fault, Subsystem},
    maybe::{MaybeFault, Opaque},
};

use catalejo_sys::{ffi, id::PeepholeId};

use nix::sys::mman::{MapFlags, ProtFlags};

use crate::{
    address::{ViAddr, ViRange},
    offset::{Field, Offset},
    target::Target,
};

/// The creation-time initialization word for a [`Peephole`].
///
/// This is a bitset of the kernel's `MIRILLA_MAP_PEEPHOLE_INITIALIZE_*` preferences, applied when
/// the peephole is created, so that a caller can request one-shot behavior without a follow-up
/// command. Combine flags with [`InitializeWord::with`].
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct InitializeWord(ffi::binding::mirilla_map_peephole_initialize_word_t);

impl InitializeWord {
    /// An empty word, requesting no creation-time behavior.
    pub const EMPTY: Self = Self(0);

    /// Populate the mapping during `mmap` rather than on first touch, so that a later read walks
    /// resident PTEs instead of paying a fault and a remote pin per granule. This trades a slower
    /// `mmap` for that faster steady state, so it suits an observer that reads the whole window.
    pub const POPULATE: Self = Self(
        ffi::binding::MIRILLA_MAP_PEEPHOLE_INITIALIZE_POPULATE
            as ffi::binding::mirilla_map_peephole_initialize_word_t,
    );

    /// Return the word with the bits of `other` also set.
    #[inline]
    #[must_use]
    pub const fn with(self, other: Self) -> Self {
        let Self(target_value) = self;
        let Self(target_other) = other;

        Self(target_value | target_other)
    }

    /// The raw word for the foreign-function boundary.
    #[inline]
    #[must_use]
    pub const fn bits(self) -> ffi::binding::mirilla_map_peephole_initialize_word_t {
        let Self(target_value) = self;

        target_value
    }
}

/// The context backing a peephole into a foreign memory address space.
#[derive(Debug)]
pub struct PeepholeContext {
    /// The peephole instance file descriptor.
    peephole_file: OwnedFd,

    /// The peephole identifier.
    peephole_id: PeepholeId,

    /// The foreign virtual address range of this peephole.
    address_range: ViRange,

    /// The lazily memory-mapped area of the peephole.
    peephole_window: Window,

    /// The initialization token to the `catalejo-fault` subsystem for fault-tolerant memory access.
    peephole_subsystem: Subsystem,
}

impl PeepholeContext {
    /// Open a [`Peephole`] into the target over the specified virtual address range.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view(target_context: &Target, address_range: ViRange) -> io::Result<Self> {
        Self::view_with(target_context, address_range, InitializeWord::EMPTY)
    }

    /// Open a [`Peephole`] into the target, applying the given creation-time [`InitializeWord`].
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view_with(
        target_context: &Target,
        address_range: ViRange,
        initialize_word: InitializeWord,
    ) -> io::Result<Self> {
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
                initialize_word.bits(),
            )?
        };

        let peephole_window = {
            let region_size = address_range
                .size()
                .ok_or(io::Error::from(ErrorKind::InvalidInput))?;

            // SAFETY: A private, full-length, zero-offset, read-only mapping of the peephole
            // file, which is exactly what the kernel module requires, it validates the
            // parameters and rejects (with `-EINVAL` or `-EACCES`) anything else.
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

            Window {
                base_address,
                region_size,
            }
        };

        let peephole_subsystem = target_context.subsystem();

        Ok(Self {
            peephole_file,
            peephole_id,
            address_range,
            peephole_window,
            peephole_subsystem,
        })
    }
}

impl PeepholeContext {
    /// Duplicate the peephole context.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole file descriptor failed to be duplicated.
    #[inline]
    pub fn duplicate(&self) -> io::Result<Self> {
        let &Self {
            ref peephole_file,
            peephole_id,
            address_range,
            peephole_subsystem,
            ..
        } = self;

        let peephole_file = OwnedFd::try_clone(peephole_file)?;

        let peephole_window = {
            let region_size = address_range
                .size()
                .ok_or(io::Error::from(ErrorKind::InvalidInput))?;

            // SAFETY: A private, full-length, zero-offset, read-only mapping of the peephole
            // file, which is exactly what the kernel module requires, it validates the
            // parameters and rejects (with `-EINVAL` or `-EACCES`) anything else.
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

            Window {
                base_address,
                region_size,
            }
        };

        Ok(Self {
            peephole_file,
            peephole_id,
            address_range,
            peephole_window,
            peephole_subsystem,
        })
    }
}

/// A handle to a peephole into a foreign memory address space.
#[derive(Debug, Clone)]
pub struct Peephole(Arc<PeepholeContext>);

impl Peephole {
    /// Open a [`Peephole`] into the target over the specified virtual address range.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view(target_context: &Target, address_range: ViRange) -> io::Result<Self> {
        Self::view_with(target_context, address_range, InitializeWord::EMPTY)
    }

    /// Open a [`Peephole`] into the target, applying the given creation-time [`InitializeWord`].
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view_with(
        target_context: &Target,
        address_range: ViRange,
        initialize_word: InitializeWord,
    ) -> io::Result<Self> {
        PeepholeContext::view_with(target_context, address_range, initialize_word)
            .map(Arc::new)
            .map(Self)
    }

    /// Fabricate a [`Foreign`] handle for `F` at the specified offset from the peephole base address.
    #[inline]
    pub fn at<F>(&self, target_displacement: Offset) -> Option<Foreign<F>>
    where
        F: Unassociated,
    {
        let Self(target_handle) = self;

        let PeepholeContext {
            peephole_window, ..
        } = target_handle.as_ref();

        let target_layout = Layout::new::<F>();

        let displacement_value = target_displacement.value();

        let in_bounds = NonZero::<usize>::get(peephole_window.size())
            .checked_sub(displacement_value)?
            >= target_layout.size();

        // NOTE: The window base is page-aligned (the kernel `-EINVAL`s a non-page-aligned
        // peephole before it is ever created), so a displacement-only alignment check suffices.
        let is_aligned = displacement_value.is_multiple_of(target_layout.align());

        if in_bounds && is_aligned {
            Some(Foreign(
                Self(Arc::clone(target_handle)),
                target_displacement,
                marker::PhantomData,
            ))
        } else {
            None
        }
    }

    /// Determine the identifier of the underlying peephole.
    #[inline]
    pub fn id(&self) -> PeepholeId {
        let Self(target_handle) = self;

        let &PeepholeContext { peephole_id, .. } = target_handle.as_ref();

        peephole_id
    }

    /// Determine the virtual address range that this peephole is for in the respective foreign address space.
    #[inline]
    pub fn range(&self) -> ViRange {
        let Self(target_handle) = self;

        let &PeepholeContext { address_range, .. } = target_handle.as_ref();

        address_range
    }

    /// Fabricate a reference to then underlying window area.
    #[inline]
    pub fn window(&self) -> &Window {
        let Self(target_handle) = self;

        let PeepholeContext {
            peephole_window, ..
        } = target_handle.as_ref();

        peephole_window
    }
}

impl Deref for Peephole {
    type Target = PeepholeContext;

    #[inline]
    fn deref(&self) -> &Self::Target {
        let Self(target_handle) = self;

        AsRef::<Self::Target>::as_ref(target_handle)
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
#[derive(Debug, Clone)]
// NOTE(invariant): Offset is in-bounds and properly aligned for `F`.
pub struct Foreign<F>(Peephole, Offset, marker::PhantomData<F>)
where
    // NOTE: Allow regular structures to be `Foreign`, but not readable as a primitive.
    F: Unassociated;

impl<F> Foreign<F>
where
    // NOTE: Allow regular structures to be `Foreign`, but not readable as a primitive.
    F: Unassociated,
{
    /// Field-project into a field of `F`, to the respective [`P::Value`].
    #[inline]
    pub fn project<P>(&self, target_project: impl Borrow<P>) -> Foreign<P::Value>
    where
        P: Field<Structure = F>,
    {
        let &Self(ref peephole_state, target_value, ..) = self;

        Foreign::<P::Value>(
            peephole_state.clone(),
            // NOTE(invariant): This remains in-bounds as `F` is guaranteed to be contained completely
            // into the peephole window, and the `Field` trait requires that the field offset is in-bounds
            // of the containing structure as a safety requirement.
            Offset::stack(target_value, Field::offset(target_project)),
            marker::PhantomData::<P::Value>,
        )
    }

    /// Cast a foreign value to another, as long as:
    ///
    /// * The casted-to type has the same alignment requirement or smaller.
    /// * The casted-to type is equal or smaller in size compared to the casted-from type.
    ///
    /// This is required to not alter the soundness-providing invariants of the [`Foreign`] handle.
    #[inline]
    pub fn cast<V>(self) -> Option<Foreign<V>>
    where
        V: Unassociated,
    {
        let Self(peephole_state, target_value, ..) = self;

        let is_equal_or_smaller_size = mem::size_of::<V>() <= mem::size_of::<F>();
        let is_equal_or_smaller_alignment = mem::align_of::<V>() <= mem::align_of::<F>();

        if is_equal_or_smaller_alignment && is_equal_or_smaller_size {
            Some(Foreign::<V>(
                peephole_state,
                target_value,
                marker::PhantomData::<V>,
            ))
        } else {
            None
        }
    }

    /// Attempt to lift the [`Foreign`] type into the locally-managed value.
    #[inline]
    pub fn lift<L>(self) -> Result<L, L::Error>
    where
        L: Lift<Value = F>,
    {
        L::construct(self)
    }

    /// Attempt to mirror the whole [`Unassociated`] `F` out of the foreign address space.
    ///
    /// Where [`read`](Self::read) is restricted to a single machine-word-coherent [`Faultable`]
    /// primitive, this streams the entire byte span of `F` through the fault-protected copy, so it
    /// serves the composite structures that exceed a machine word and therefore carry no snapshot
    /// coherence. The bytes are assembled ascending and byte-granular, so a foreign mutation in
    /// flight tears the observed value, yet `F` being [`Unassociated`] guarantees every resulting
    /// bit-pattern remains a valid inhabitant.
    ///
    /// The caller owns the destination through `target_buffer`, so a large aggregate lands directly
    /// in its final home rather than being returned by value and copied a second time. A full mirror
    /// initializes the buffer in whole and hands back an exclusive reference to the now-live `F`. A
    /// fault yields an [`Err`] carrying the count of bytes left uncopied at the faulting byte, and no
    /// reference is produced, because the buffer holds only the copied prefix and its tail stays
    /// uninitialized.
    #[inline]
    pub fn copy<'buffer>(
        &self,
        target_buffer: &'buffer mut mem::MaybeUninit<F>,
    ) -> Result<&'buffer mut F, usize> {
        let Self(target_peephole, target_displacement, ..) = self;

        let PeepholeContext {
            peephole_subsystem,
            ref peephole_window,
            ..
        } = **target_peephole;

        let target_count = mem::size_of::<F>();

        // NOTE(invariant): `Self` keeps the displacement in-bounds and aligned for `F`, so the
        // window base plus the displacement names the live foreign span and never overflows. Treat
        // an overflow defensively as a total fault, with the whole span left uncopied.
        let Some(target_source) =
            Window::address(peephole_window).checked_add(target_displacement.value())
        else {
            return Err(target_count);
        };

        let target_address = target_buffer.as_mut_ptr().cast::<u8>();

        let target_source =
            ptr::with_exposed_provenance::<u8>(NonZero::<usize>::get(target_source));

        // SAFETY:
        //
        // * The source names the foreign peephole window, memory outside the abstract machine
        //   reached under exposed provenance and kept mapped for the borrow by the `Arc` behind the
        //   peephole. It is ordinary RAM rather than side-effecting MMIO, and a dead peephole faults
        //   and is reported as `Err` rather than being undefined behavior.
        //
        // * The destination is the caller's `MaybeUninit<F>`, ordinary abstract-machine memory
        //   valid and writable for `target_count` bytes, exclusively borrowed for `'buffer`, and
        //   aligned for `F` by construction, so it never overlaps the disjoint foreign source.
        let target_outcome = unsafe {
            fault::copy(
                peephole_subsystem,
                target_address,
                target_source,
                target_count,
            )
        };

        match target_outcome {
            // SAFETY: The copy wrote every one of the `target_count` bytes, so the buffer is
            // initialized in whole, and `F` is `Unassociated`, so the assembled bit-pattern is a
            // valid inhabitant of `F` regardless of any tearing.
            Ok(()) => Ok(unsafe { target_buffer.assume_init_mut() }),
            Err(target_remaining) => Err(target_remaining),
        }
    }

    /// Mirror `F` out of the foreign window one page run at a time, recording which pages survived.
    ///
    /// Where [`copy`](Self::copy) stops at the first dead page and reports the uncopied remainder,
    /// this resumes past each dead page and keeps filling the buffer at the faulting byte's true
    /// offset, so the destination stays a one-to-one spatial image of the foreign window rather than
    /// a compacted one. Compaction would slide live bytes together and forge an adjacency the target
    /// never had, which a byte scanner would then report as a false match. The spatial image avoids
    /// that, because a match offset maps back to its foreign address by plain addition.
    ///
    /// `target_pages` receives one bit per host page of `F`, set when that page copied whole and left
    /// clear when it was dead at copy time. Unmapping is page-granular, so a fault always lands on a
    /// page boundary, and a page that copies is a page fully present. The cleared bits double as the
    /// initialized-memory mask, because a hole is never written and must not be read back as `F`. The
    /// returned count is the number of live pages, so a caller can gauge remaining work against it.
    #[inline]
    pub fn copy_sparse(
        &self,
        target_buffer: &mut mem::MaybeUninit<F>,
        target_pages: &mut [usize],
    ) -> usize {
        // NOTE: Host base page size. The foreign window base is page-aligned and a tile spans whole
        // pages, so page indices divide the span exactly and a fault falls on a page boundary.
        const TARGET_PAGE: usize = 4096;

        // NOTE: Width of a bitset word, so the page index splits into word and bit without a magic
        // constant that would silently disagree with the word type.
        const TARGET_WORD_BITS: usize = usize::BITS as usize;

        let Self(target_peephole, target_displacement, ..) = self;

        let PeepholeContext {
            peephole_subsystem,
            ref peephole_window,
            ..
        } = **target_peephole;

        let target_span = mem::size_of::<F>();

        let Some(target_source_base) =
            Window::address(peephole_window).checked_add(target_displacement.value())
        else {
            // NOTE: A degenerate base names no live span, so no page is live.
            return 0;
        };

        let target_destination = target_buffer.as_mut_ptr().cast::<u8>();

        let target_source =
            ptr::with_exposed_provenance::<u8>(NonZero::<usize>::get(target_source_base));

        let mut target_offset = 0usize;
        let mut target_live = 0usize;

        while target_offset < target_span {
            let target_remaining = target_span - target_offset;

            // SAFETY:
            //
            // * The foreign side names the peephole window under exposed provenance kept mapped for
            //   the borrow by the `Arc` behind the peephole, so a dead page faults and is caught by
            //   the subsystem rather than being undefined behavior.
            //
            // * The local side is the caller's `MaybeUninit<F>`, valid for the whole span and
            //   disjoint from the foreign source, so the resumed writes never overlap the reads.
            let target_outcome = unsafe {
                fault::copy(
                    peephole_subsystem,
                    target_destination.add(target_offset),
                    target_source.add(target_offset),
                    target_remaining,
                )
            };

            let target_copied = match target_outcome {
                Ok(()) => target_remaining,
                Err(target_left) => target_remaining - target_left,
            };

            // NOTE: Mark every whole page the copy just filled as live. A trailing partial page is
            // only ever the final page of the span, and it still counts as covered.
            let target_page_start = target_offset / TARGET_PAGE;
            let target_page_end = (target_offset + target_copied).div_ceil(TARGET_PAGE);

            for target_page in target_page_start..target_page_end {
                target_pages[target_page / TARGET_WORD_BITS] |=
                    1usize << (target_page % TARGET_WORD_BITS);

                target_live += 1;
            }

            // NOTE: On success the span is exhausted. On a fault the copy halted at the first byte of
            // a dead page, so step past exactly that one page and resume, leaving its bit clear.
            if target_outcome.is_err() {
                target_offset = target_offset + target_copied + TARGET_PAGE;
            } else {
                break;
            }
        }

        target_live
    }
}

impl<F> Foreign<F>
where
    F: Faultable,
{
    /// Attempt to read a [`Faultable`] `F` from the foreign address space.
    ///
    /// This is a fault-protected, machine-word-coherent read of the foreign window. A [`Some`]
    /// holds the value observed at the instant of the read, a [`None`] denotes that the read
    /// faulted, i.e. the peephole was dead (its pages reclaimed by the kernel) at that instant.
    #[inline]
    pub fn read(&self) -> Option<F> {
        let Self(target_peephole, target_displacement, ..) = self;

        let PeepholeContext {
            peephole_subsystem,
            ref peephole_window,
            ..
        } = **target_peephole;

        let target_address =
            Window::address(peephole_window).checked_add(target_displacement.value())?;

        // SAFETY:
        //
        // * `F` is `Faultable`, so every bit-pattern read back is a valid value.
        //
        // * `Self`'s invariant guarantees the displacement is in-bounds and aligned for `F`, so
        //   `target_address` lies within the live mapping, kept mapped for `'a` by the
        //   `Arc<Window>` borrowed through the peephole, and the foreign window is ordinary RAM,
        //   never side-effecting MMIO.
        //
        // * A dead peephole faults and is reported as `None` rather than being undefined behavior.
        unsafe { MaybeFault::<F>::new(target_address).read(peephole_subsystem) }
    }
}

/// A trait that is implemented for foreigner-struct-wrapping types.
///
/// This describes a type that can be constructed from a respective [`Foreign`] handle.
pub trait Lift: Immortal {
    /// The foreign structure to be read for construction purposes.
    ///
    /// This does not require to be [`Faultable`], as it may be a structure itself.
    type Value: Unassociated;

    /// The error that can arise during construction.
    type Error;

    /// Construct the type from a [`Foreign`] handle to the target value type.
    fn construct(target_handle: Foreign<Self::Value>) -> Result<Self, Self::Error>
    where
        Self: Sized;
}
