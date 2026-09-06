//! In-memory peephole module.
#![allow(
    clippy::std_instead_of_core,
    reason = "imports are false-flagged by clippy where the fix would be nightly-only"
)]

use alloc::{rc::Rc, sync::Arc, vec::Vec};

use core::{
    alloc::Layout,
    borrow::Borrow,
    marker, mem,
    num::NonZero,
    ops::Deref,
    ptr::{self, NonNull},
};

use std::{
    io::{self, ErrorKind},
    os::fd::{AsFd, OwnedFd},
    thread,
    time::Instant,
};

use bitflags::bitflags;
use catalejo_memory::{behavior::Immortal, prelude::Unassociated};
use fack::prelude::Error;

use catalejo_fault::{
    behavior::{Faultable, equal},
    maybe::{MaybeFault, Opaque},
};

use catalejo_sys::{access as system_access, exception::Image, ffi, id::PeepholeId, monitor};

use nix::sys::mman::{MapFlags, ProtFlags};

use crate::{
    address::{ViAddr, ViRange},
    manage::Access,
    offset::{Field, Offset, Sparse},
    target::Target,
};

// NOTE: Re-export the system monitor backend through the high-level peephole API.
pub use catalejo_sys::monitor::MonitorBackend;

bitflags! {
    /// The creation-time initialization word for a [`Peephole`].
    ///
    /// Each flag mirrors one generated `MIRILLA_MAP_PEEPHOLE_INITIALIZE_*` preference.
    #[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
    pub struct InitializeWord: ffi::binding::mirilla_map_peephole_initialize_word_t {
        /// Populate the mapping during `mmap` rather than on first touch.
        const POPULATE = ffi::binding::MIRILLA_MAP_PEEPHOLE_INITIALIZE_POPULATE
            as ffi::binding::mirilla_map_peephole_initialize_word_t;
    }
}

/// The context backing a peephole into a foreign memory address space.
#[derive(Debug)]
// NOTE(invariant): `exception_session` shares the open file description that registered the
// process-global exception image for the current address space and remains alive for every
// protected operation through this context.
pub struct PeepholeContext {
    /// A duplicate of the Mirilla session that owns the exception-image registration.
    exception_session: OwnedFd,

    /// The peephole instance file descriptor.
    peephole_file: OwnedFd,

    /// The peephole identifier.
    peephole_id: PeepholeId,

    /// The foreign virtual address range of this peephole.
    address_range: ViRange,

    /// The lazily memory-mapped area of the peephole.
    peephole_window: Window,
}

impl PeepholeContext {
    /// Open a [`Peephole`] into the target over the specified virtual address range.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view(target_context: &Target, address_range: ViRange) -> io::Result<Self> {
        Self::view_with(target_context, address_range, InitializeWord::empty())
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
        let exception_session = target_context.device().try_clone_to_owned()?;
        let peephole_window = {
            let region_size = address_range
                .size()
                .ok_or(io::Error::from(ErrorKind::InvalidInput))?;

            let region_protection = ProtFlags::PROT_READ
                | if cfg!(feature = "write") {
                    ProtFlags::PROT_WRITE
                } else {
                    ProtFlags::PROT_NONE
                };

            // SAFETY: A private, full-length, zero-offset, read-only mapping of the peephole
            // file, which is exactly what the kernel module requires, it validates the
            // parameters and rejects (with `-EINVAL` or `-EACCES`) anything else.
            let target_value = unsafe {
                nix::sys::mman::mmap(
                    None,
                    region_size,
                    region_protection,
                    MapFlags::MAP_PRIVATE,
                    peephole_file.as_fd(),
                    0,
                )?
            };

            let base_address = MaybeFault::new(target_value.expose_provenance());

            Window {
                base_address,
                region_size,
                region_protection,
            }
        };

        Ok(Self {
            exception_session,
            peephole_file,
            peephole_id,
            address_range,
            peephole_window,
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
            ref exception_session,
            ref peephole_file,
            peephole_id,
            address_range,
            peephole_window: Window {
                region_protection, ..
            },
            ..
        } = self;

        let peephole_file = OwnedFd::try_clone(peephole_file)?;
        let exception_session = OwnedFd::try_clone(exception_session)?;

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
                    region_protection,
                    MapFlags::MAP_PRIVATE,
                    peephole_file.as_fd(),
                    0,
                )?
            };

            let base_address = MaybeFault::new(target_value.expose_provenance());

            Window {
                base_address,
                region_size,
                region_protection,
            }
        };

        Ok(Self {
            exception_session,
            peephole_file,
            peephole_id,
            address_range,
            peephole_window,
        })
    }
}

/// A handle to a peephole into a foreign memory address space.
#[derive(Debug, Clone)]
// NOTE(invariant): every `Peephole` owns one strong `Arc` to its context, and dropping the final strong owner destroys the mapped window and closes the peephole file descriptor.
pub struct Peephole(pub Arc<PeepholeContext>);

impl Peephole {
    /// Open a [`Peephole`] into the target over the specified virtual address range.
    ///
    /// # Failure
    ///
    /// This may fail if the underlying peephole could not be created or memory-mapped.
    #[inline]
    pub fn view(target_context: &Target, address_range: ViRange) -> io::Result<Self> {
        Self::view_with(target_context, address_range, InitializeWord::empty())
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

        let displacement_value = target_displacement.native();

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
    /// This is a maybe-fault region protected by the Mirilla exception image registered when the
    /// owning target session was acquired.
    base_address: MaybeFault<Opaque>,

    /// The size of the memory-mapped region.
    region_size: NonZero<usize>,

    /// The virtual memory protection flags applied to the window.
    // NOTE(invariant): This is fixed and depends on the enabled feature set.
    region_protection: ProtFlags,
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

/// The implementation used by an armed foreign monitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorMode {
    /// Intel user monitor and user wait instructions.
    IntelUmonitor,

    /// AMD extended monitor and extended wait instructions.
    AmdMonitorx,

    /// Fault-protected reads with cooperative thread yielding.
    Polling,
}

impl From<Option<MonitorBackend>> for MonitorMode {
    #[inline]
    fn from(target_backend: Option<MonitorBackend>) -> Self {
        match target_backend {
            Some(MonitorBackend::IntelUmonitor) => Self::IntelUmonitor,
            Some(MonitorBackend::AmdMonitorx) => Self::AmdMonitorx,
            None => Self::Polling,
        }
    }
}

/// A failure while arming a foreign monitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorArmError {
    /// The local peephole downstream address faulted.
    Fault,
}

/// The result of waiting on a foreign monitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorWaitOutcome {
    /// The observed value changed at the bit level.
    Changed,

    /// The finite deadline was reached before a change was observed.
    TimedOut,

    /// A fault made the local peephole downstream address unreadable.
    Fault,
}

/// An armed monitor borrowing a [`Foreign`] value.
///
/// The borrow keeps the peephole mapping alive through the wait. The token is bound to the
/// arming thread because hardware monitor state is local to a logical processor context.
///
/// ```compile_fail
/// use catalejo::peephole::ArmedMonitor;
///
/// fn require_send<T: Send>() {}
///
/// fn rejected(target_monitor: ArmedMonitor<'_, u64>) {
///     require_send::<ArmedMonitor<'_, u64>>();
/// }
/// ```
#[derive(Debug)]
// NOTE(invariant): The token borrows the armed handle and cannot move away from the arming thread.
pub struct ArmedMonitor<'foreign, F>
where
    F: Faultable,
{
    /// The foreign handle that keeps the peephole mapping alive.
    target_handle: &'foreign Foreign<F>,

    /// The value observed before the hardware monitor was armed.
    target_expected: F,

    /// The hardware backend armed for this token.
    target_backend: Option<MonitorBackend>,

    /// Whether the value changed while the monitor was being armed.
    target_changed: bool,

    /// The marker that prevents transfer to another thread.
    marker: marker::PhantomData<Rc<()>>,
}

impl<'foreign, F> ArmedMonitor<'foreign, F>
where
    F: Faultable,
{
    /// Determine the implementation used by this monitor.
    #[inline]
    pub fn mode(&self) -> MonitorMode {
        let Self { target_backend, .. } = self;

        MonitorMode::from(*target_backend)
    }

    /// Wait until the value changes or the finite deadline is reached.
    ///
    /// This consumes the armed token so its same-thread hardware state cannot be reused. Hardware
    /// waits run in bounded slices. Every wake is followed by another protected arm and snapshot
    /// sequence. Unsupported hardware switches to cooperative polling with the same deadline.
    #[inline]
    pub fn wait(self, target_deadline: Instant) -> MonitorWaitOutcome {
        let Self {
            target_handle,
            target_expected,
            mut target_backend,
            target_changed,
            marker: _,
        } = self;

        if target_changed {
            return MonitorWaitOutcome::Changed;
        }

        loop {
            let Some(target_observed) = Foreign::read(target_handle) else {
                return MonitorWaitOutcome::Fault;
            };

            if !equal(target_expected, target_observed) {
                return MonitorWaitOutcome::Changed;
            }

            if Instant::now() >= target_deadline {
                return MonitorWaitOutcome::TimedOut;
            }

            let Some(target_backend_value) = target_backend else {
                thread::yield_now();

                continue;
            };

            let Foreign(target_peephole, ..) = target_handle;
            let image: &Image = {
                Image::retrieve()
                    .expect("a peephole context exists only after exception-image registration")
            };

            // SAFETY:
            //
            // * This token was armed on the current thread and cannot move to another thread.
            //
            // * Its borrow keeps the local downstream mapping alive through the wait.
            let wait_outcome = unsafe {
                monitor::wait(
                    #[cfg(not(feature = "stealth-mode"))]
                    Image::retrieve().expect(
                        "a peephole context exists only after exception-image registration",
                    ),
                    #[cfg(feature = "stealth-mode")]
                    Image::infallible(),
                    target_backend_value,
                )
            };

            match wait_outcome {
                Ok(()) => {}
                Err(monitor::MonitorError::Unsupported) => {
                    target_backend = None;

                    continue;
                }
                Err(monitor::MonitorError::Fault) => return MonitorWaitOutcome::Fault,
            }

            let Some(target_address) = Foreign::address(target_handle) else {
                return MonitorWaitOutcome::Fault;
            };
            let target_address =
                ptr::with_exposed_provenance::<u8>(NonZero::<usize>::get(target_address));

            // SAFETY:
            //
            // * The address is the in-bounds local downstream address.
            //
            // * This consuming wait remains on the thread that created the armed token.
            //
            // * The borrowed foreign handle keeps the mapping alive.
            let monitor_backend = unsafe { monitor::arm(image, target_address) };

            match monitor_backend {
                Ok(backend_value) => target_backend = Some(backend_value),
                Err(monitor::MonitorError::Unsupported) => target_backend = None,
                Err(monitor::MonitorError::Fault) => return MonitorWaitOutcome::Fault,
            }
        }
    }
}

/// Completion state of an arbitrary byte copy.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ByteCopyStatus {
    /// The complete requested span was copied.
    Complete,

    /// The copy faulted after writing a prefix.
    Faulted,
}

/// Result of copying an arbitrary byte span from a peephole window.
#[derive(Debug)]
// NOTE(invariant): `bytes` is exactly the initialized destination prefix written by the protected copy, and `state` records whether that prefix covers the complete requested span.
pub struct ByteCopy<'a> {
    /// The populated copy buffer.
    copy_buffer: &'a mut [u8],

    /// The final state of the byte copy.
    copy_state: ByteCopyStatus,
}

impl ByteCopy<'_> {
    /// Borrow the initialized bytes written by the protected copy.
    #[inline]
    #[must_use]
    pub const fn bytes(&self) -> &[u8] {
        let Self {
            copy_buffer: bytes, ..
        } = self;

        bytes
    }

    /// Determine the number of bytes written before completion or fault.
    #[inline]
    #[must_use]
    pub const fn copied(&self) -> usize {
        ByteCopy::bytes(self).len()
    }

    /// Determine the completion state of the protected copy.
    #[inline]
    #[must_use]
    pub const fn status(&self) -> ByteCopyStatus {
        let &Self { copy_state, .. } = self;

        copy_state
    }

    /// Determine whether the complete requested span was copied.
    #[inline]
    #[must_use]
    pub const fn complete(&self) -> bool {
        let &Self { copy_state, .. } = self;

        matches!(copy_state, ByteCopyStatus::Complete)
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
    /// Determine the local downstream address in the peephole mapping.
    #[inline]
    pub fn address(&self) -> Option<NonZero<usize>> {
        let Self(target_peephole, target_displacement, ..) = self;

        Window::address(target_peephole.window()).checked_add(target_displacement.native())
    }

    /// Field-project into a field of `F`, to the respective `P::Value`.
    #[inline]
    pub fn project<P>(&self, target_project: impl Borrow<P>) -> Foreign<P::Value>
    where
        P: Field<Structure = F>,
    {
        let &Self(ref peephole_state, target_value, ..) = self;

        let target_offset = Offset::stack(target_value, Field::offset(target_project));

        #[cfg(feature = "stealth-mode")]
        // SAFETY: `F` is contained completely within the peephole and `Field` requires an in-bounds
        // field offset, so the stacked offset always exists.
        let target_offset = unsafe { target_offset.unwrap_unchecked() };

        #[cfg(not(feature = "stealth-mode"))]
        let target_offset = target_offset.expect("stacked offset should remain in-bounds");

        Foreign::<P::Value>(
            peephole_state.clone(),
            // NOTE(invariant): This remains in-bounds as `F` is guaranteed to be contained completely
            // into the peephole window, and the `Field` trait requires that the field offset is in-bounds
            // of the containing structure as a safety requirement.
            target_offset,
            marker::PhantomData::<P::Value>,
        )
    }

    /// Create a sparse access intent for a profile-selected field of `F`.
    ///
    /// The returned intent may lie outside the current peephole. Resolve it with
    /// [`Manage::refresh`](crate::manage::Manage::refresh) before reading so the
    /// manager can reuse or open a window that fits the projected value.
    #[inline]
    pub fn sparse<P>(&self, target_project: impl Borrow<P>) -> Option<Access<P::Value>>
    where
        P: Sparse<Structure = F>,
    {
        let &Self(ref peephole_state, target_value, ..) = self;
        let target_displacement = Offset::stack(target_value, Sparse::offset(target_project))?;

        Some(Access::intent(peephole_state.clone(), target_displacement))
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
        L::Context: Default,
    {
        L::construct(self)
    }

    /// Repeatedly lift the foreign structure until two sequential values compare as equal.
    #[inline]
    pub fn coherent<L>(self) -> Result<L, L::Error>
    where
        L: Coherent<Value = F>,
        L::Context: Default,
    {
        <L as Coherent>::construct(self)
    }

    /// Lift at most `N` times while seeking two equal sequential observations.
    #[inline]
    pub fn stabilize<L, const N: usize>(self) -> Result<L, StabilizeError<L::Error>>
    where
        L: Stabilize<N, Value = F>,
        L::Context: Default,
    {
        <L as Stabilize<N>>::construct(self)
    }

    /// Attempt to lift the [`Foreign`] type into the locally-managed value.
    #[inline]
    pub fn lift_with<L>(self, target_context: &L::Context) -> Result<L, L::Error>
    where
        L: Lift<Value = F>,
    {
        L::construct_with(self, target_context)
    }

    /// Repeatedly lift the foreign structure until two sequential values compare as equal.
    #[inline]
    pub fn coherent_with<L>(self, target_context: &L::Context) -> Result<L, L::Error>
    where
        L: Coherent<Value = F>,
    {
        <L as Coherent>::construct_with(self, target_context)
    }

    /// Lift at most `N` times with context while seeking equal sequential observations.
    #[inline]
    pub fn stabilize_with<L, const N: usize>(
        self,
        target_context: &L::Context,
    ) -> Result<L, StabilizeError<L::Error>>
    where
        L: Stabilize<N, Value = F>,
    {
        <L as Stabilize<N>>::construct_with(self, target_context)
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
            ref peephole_window,
            ..
        } = **target_peephole;
        let image = {
            let this = &target_peephole;
            Image::retrieve()
                .expect("a peephole context exists only after exception-image registration")
        };

        let target_count = mem::size_of::<F>();

        // NOTE(invariant): `Self` keeps the displacement in-bounds and aligned for `F`, so the
        // window base plus the displacement names the live foreign span and never overflows. Treat
        // an overflow defensively as a total fault, with the whole span left uncopied.
        let Some(target_source) =
            Window::address(peephole_window).checked_add(target_displacement.native())
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
        let target_outcome =
            unsafe { system_access::copy(image, target_address, target_source, target_count) };

        match target_outcome {
            // SAFETY: The copy wrote every one of the `target_count` bytes, so the buffer is
            // initialized in whole, and `F` is `Unassociated`, so the assembled bit-pattern is a
            // valid inhabitant of `F` regardless of any tearing.
            Ok(()) => Ok(unsafe { target_buffer.assume_init_mut() }),
            Err(target_count) => Err(target_count),
        }
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
        let Self(target_peephole, ..) = self;
        let target_address = Foreign::address(self)?;

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
        let image = {
            let this = &target_peephole;
            Image::retrieve()
                .expect("a peephole context exists only after exception-image registration")
        };

        unsafe { MaybeFault::<F>::new(target_address).read(image) }
    }

    /// Attempt to write a [`Faultable`] `F` to the foreign address space.
    ///
    /// This is a fault-protected, machine-word-coherent write of the foreign window. It returns
    /// `true` when the store completes and `false` when the access faults, for example because the
    /// target mapping disappeared or no longer permits writes.
    #[inline]
    #[cfg(feature = "write")]
    pub fn write(&self, target_value: F) -> bool {
        let Self(target_peephole, ..) = self;

        match Foreign::address(self) {
            Some(target_address) => {
                // SAFETY:
                //
                // * `F` is `Faultable`, so every bit-pattern written is a valid value.
                //
                // * `Self`'s invariant guarantees the displacement is in-bounds and aligned for `F`, so
                //   `target_address` lies within the live mapping, kept mapped for `'a` by the
                //   `Arc<Window>` borrowed through the peephole, and the foreign window is ordinary RAM,
                //   never side-effecting MMIO.
                //
                // * A dead peephole faults and is reported as `None` rather than being undefined behavior.
                let image = {
                    let this = &target_peephole;
                    Image::retrieve()
                        .expect("a peephole context exists only after exception-image registration")
                };

                unsafe { MaybeFault::<F>::new(target_address).write(image, target_value) }
            }
            None => false,
        }
    }

    /// Arm a monitor for changes to this foreign value.
    ///
    /// The protected instruction receives the local downstream address in the peephole mapping.
    /// A snapshot read after arming closes the race between the initial read and monitor setup.
    /// Unsupported optional instructions select polling instead of failing the operation.
    ///
    /// # Failure
    ///
    /// This fails when the local downstream address faults during either protected snapshot or
    /// hardware arming operation.
    #[inline]
    pub fn monitor(&self) -> Result<ArmedMonitor<'_, F>, MonitorArmError> {
        let Self(target_peephole, ..) = self;
        let target_expected = Foreign::read(self).ok_or(MonitorArmError::Fault)?;
        let target_address = Foreign::address(self).ok_or(MonitorArmError::Fault)?;
        let target_address =
            ptr::with_exposed_provenance::<u8>(NonZero::<usize>::get(target_address));
        let image = {
            let this = &target_peephole;
            Image::retrieve()
                .expect("a peephole context exists only after exception-image registration")
        };

        let target_backend =
            // SAFETY:
            //
            // * The address is the in-bounds local downstream address rather than the foreign
            //   virtual address.
            //
            // * The borrow stored in the returned token keeps the peephole mapping alive.
            //
            // * The returned token can only wait on this thread because it is not `Send`.
            match unsafe { monitor::arm(image, target_address) } {
                Ok(target_backend) => Some(target_backend),
                Err(monitor::MonitorError::Unsupported) => None,
                Err(monitor::MonitorError::Fault) => return Err(MonitorArmError::Fault),
            };

        let target_observed = Foreign::read(self).ok_or(MonitorArmError::Fault)?;
        let target_changed = !equal(target_expected, target_observed);

        Ok(ArmedMonitor {
            target_handle: self,
            target_expected,
            target_backend,
            target_changed,
            marker: marker::PhantomData,
        })
    }
}

impl Foreign<u8> {
    /// Determine the nonzero byte capacity remaining from this foreign byte address.
    #[inline]
    #[must_use]
    pub fn leftover(&self) -> NonZero<usize> {
        let Self(peephole, displacement, ..) = self;
        let window_size = peephole.window().size().get();

        window_size
            .checked_sub(displacement.native())
            .and_then(NonZero::new)
            .expect("a Foreign<u8> always retains its starting byte inside the peephole")
    }

    /// Copy an arbitrary byte span from this foreign address into caller-owned storage.
    ///
    /// The requested span may extend beyond `F` but must fit within the current peephole window.
    /// A fault returns the initialized prefix written before the faulting byte.
    ///
    /// # Failure
    ///
    /// This returns [`None`] when the requested byte span exceeds the current peephole window.
    #[inline]
    pub fn bytes<'a>(&self, target_buffer: &'a mut [mem::MaybeUninit<u8>]) -> Option<ByteCopy<'a>> {
        let Self(target_peephole, target_displacement, ..) = self;

        let PeepholeContext {
            peephole_window, ..
        } = &**target_peephole;
        let image = {
            let this = &target_peephole;
            Image::retrieve()
                .expect("a peephole context exists only after exception-image registration")
        };
        let target_count = target_buffer.len();

        let target_available = Foreign::leftover(self);

        let in_bounds = target_count <= target_available.get();

        if in_bounds {
            let target_source =
                Window::address(peephole_window).checked_add(target_displacement.native())?;

            let target_address = target_buffer.as_mut_ptr().cast::<u8>();

            let target_source =
                ptr::with_exposed_provenance::<u8>(NonZero::<usize>::get(target_source));

            // SAFETY:
            //
            // * `Self` retains the peephole mapping and proves the starting displacement is
            //   in-bounds. The runtime fit check proves the complete requested span remains in
            //   that mapping.
            //
            // * The destination slice is caller-owned `MaybeUninit<u8>` storage valid and
            //   writable for `target_count` bytes.
            //
            // * The local destination allocation and foreign peephole mapping cannot overlap.
            let target_outcome =
                unsafe { system_access::copy(image, target_address, target_source, target_count) };

            let (target_copied, copy_state) = match target_outcome {
                Ok(()) => (target_count, ByteCopyStatus::Complete),
                Err(target_remaining) => (
                    target_count.saturating_sub(target_remaining),
                    ByteCopyStatus::Faulted,
                ),
            };

            // SAFETY: The protected copy reports the untouched suffix length. The preceding
            // `target_copied` bytes are therefore initialized. Saturation conservatively
            // exposes an empty prefix if a malformed backend reports an excessive remainder.
            let copy_buffer =
                unsafe { core::slice::from_raw_parts_mut(target_address, target_copied) };

            Some(ByteCopy {
                copy_buffer,
                copy_state,
            })
        } else {
            None
        }
    }

    /// Append a protected foreign byte copy directly into an owned byte vector.
    ///
    /// The vector grows only by the initialized prefix reported by [`Self::bytes`]. A fault keeps
    /// that prefix in the vector and is reported through the returned [`ByteCopyStatus`].
    ///
    /// # Failure
    ///
    /// This returns [`None`] when `count` exceeds the bytes remaining in the current peephole.
    #[inline]
    pub fn append<'a>(&self, buffer: &'a mut Vec<u8>, count: usize) -> Option<ByteCopy<'a>> {
        let start = buffer.len();

        buffer.reserve(count);

        let (copied, state) = {
            let spare = &mut buffer.spare_capacity_mut()[..count];
            let copy = Foreign::bytes(self, spare)?;
            let copied = copy.copied();
            let state = copy.status();

            (copied, state)
        };

        // SAFETY: `Foreign::bytes` exposes exactly the initialized destination prefix. The vector
        // reserved `count` bytes before that copy, and `copied` cannot exceed `count`.
        unsafe { buffer.set_len(start + copied) };

        let copy_buffer = &mut buffer[start..];
        let copy_state = state;

        Some(ByteCopy {
            copy_buffer,
            copy_state,
        })
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

    /// The context that may be used during construction.
    type Context;

    /// The error that can arise during construction.
    type Error;

    /// Construct the type from a [`Foreign`] handle to the target value type, with the required context.
    fn construct_with(
        target_handle: Foreign<Self::Value>,
        target_context: &Self::Context,
    ) -> Result<Self, Self::Error>;

    /// Construct the type from a [`Foreign`] handle to the target value type.
    #[inline]
    fn construct(target_handle: Foreign<Self::Value>) -> Result<Self, Self::Error>
    where
        Self::Context: Default,
        Self: Sized,
    {
        Self::construct_with(target_handle, &Self::Context::default())
    }
}

/// Failure while lifting one fault-safe primitive directly.
#[derive(Clone, Copy, Debug, Eq, Error, PartialEq)]
#[error("foreign primitive read faulted")]
pub struct PrimitiveLiftError;

impl<F> Lift for F
where
    F: Faultable,
{
    type Value = Self;

    type Context = ();

    type Error = PrimitiveLiftError;

    #[inline]
    fn construct_with(
        target_handle: Foreign<Self::Value>,
        _target_context: &Self::Context,
    ) -> Result<Self, Self::Error> {
        target_handle.read().ok_or(PrimitiveLiftError)
    }
}

/// A [`Lift`] that can establish whole-structure coherence through repeated reads.
///
/// Two sequential lifted values must compare as equal before the newer value is returned.
/// A foreign structure that never stabilizes can keep this operation from completing.
pub trait Coherent: Lift + Eq {
    /// Repeatedly lift the foreign structure until two sequential values compare as equal.
    fn construct(target_handle: Foreign<Self::Value>) -> Result<Self, Self::Error>
    where
        Self::Context: Default,
        Self: Sized,
    {
        <Self as Coherent>::construct_with(target_handle, &Self::Context::default())
    }

    /// Construct the type from a [`Foreign`] handle to the target value type, with the required context.
    #[inline]
    fn construct_with(
        target_handle: Foreign<Self::Value>,
        target_context: &Self::Context,
    ) -> Result<Self, Self::Error> {
        let mut target_previous =
            <Self as Lift>::construct_with(target_handle.clone(), target_context)?;

        loop {
            let target_current =
                <Self as Lift>::construct_with(target_handle.clone(), target_context)?;

            if target_previous == target_current {
                return Ok(target_current);
            }

            target_previous = target_current;
        }
    }
}

impl<L> Coherent for L where L: Lift + Eq {}

/// Failure while establishing bounded whole-structure stability.
#[derive(Debug, Clone, Copy, Error, PartialEq, Eq)]
pub enum StabilizeError<E> {
    /// The underlying [`Lift`] failed before stability could be established.
    #[error("lift failed while stabilizing: {0}")]
    #[error(source(0))]
    Lift(E),

    /// The configured lift count was exhausted before two sequential values compared equal.
    #[error("foreign structure did not stabilize within the configured lift count ({0})")]
    Unstable(usize),
}

/// A [`Coherent`]-like lift contract with a finite observation count.
///
/// `N` counts complete lifts including the initial baseline observation. Stability requires two
/// sequential lifted values to compare equal. Values of `N` below two therefore always yield
/// [`StabilizeError::Unstable`] unless an earlier lift fails.
pub trait Stabilize<const N: usize>: Lift + Eq {
    /// Lift until two sequential observations compare equal or `N` lifts are exhausted.
    #[inline]
    fn construct(target_handle: Foreign<Self::Value>) -> Result<Self, StabilizeError<Self::Error>>
    where
        Self::Context: Default,
        Self: Sized,
    {
        <Self as Stabilize<N>>::construct_with(target_handle, &Self::Context::default())
    }

    /// Lift with context until stability is established or `N` lifts are exhausted.
    #[inline]
    fn construct_with(
        target_handle: Foreign<Self::Value>,
        target_context: &Self::Context,
    ) -> Result<Self, StabilizeError<Self::Error>> {
        let mut target_count = 0_usize;
        let mut target_previous = None;

        loop {
            let target_within_bound = target_count < N;

            match target_within_bound {
                true => {}
                false => break Err(StabilizeError::Unstable(target_count)),
            }

            let target_current =
                <Self as Lift>::construct_with(target_handle.clone(), target_context)
                    .map_err(StabilizeError::Lift)?;

            target_count += 1;

            let target_stable = target_previous
                .as_ref()
                .is_some_and(|target_previous| target_previous == &target_current);

            match target_stable {
                true => break Ok(target_current),
                false => target_previous = Some(target_current),
            }
        }
    }
}

impl<L, const N: usize> Stabilize<N> for L where L: Lift + Eq {}
