//! Faultable region management for the `catalejo-fault` crate.

use core::{
    marker,
    num::NonZero,
    ops::{Deref, DerefMut},
    ptr,
};

use catalejo_memory::prelude::{Primitive, Unassociated};

use crate::{
    behavior::Faultable,
    ffi::{self, Subsystem},
};

/// An opaque probe.
///
/// This implements [`Faultable`], so it can be used for fault-recovery memory accesses.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default, Hash)]
#[repr(transparent)]
pub struct Opaque(u8);

// SAFETY: The type has the same in-memory representation as an `u8`, which implements [`Unassociated`].
unsafe impl Unassociated for Opaque {}

// SAFETY: The type has the same in-memory representation as an `u8`, which implements [`Faultable`].
unsafe impl Faultable for Opaque {
    const PRIMITIVE: Primitive = u8::PRIMITIVE;
}

/// A maybe-fault pointer represents a non-null pointer to memory region where reads or writes may result in a synchronous hardware exception.
///
/// This may be due to an unpopulated page table, bad access to the underlying page table, or any other hardware exception that is signaled via
/// a `SIGBUS` or `SIGSEGV` signal to the faulting thread.
///
/// This is `repr(transparent)` over the managed [`NonZero`]`<usize>` address. It stores a bare, provenance-free address rather than a live pointer. At the point of access the address is reconstituted into a pointer carrying *exposed* provenance, under which memory outside the Rust abstract machine, such as this MMU-adjudicated foreign region disjoint from the stack, heap, and statics, is always accessible.
///
/// ## Remarks
///
/// Even if this may be fault-resiliant, the sole standing constraint is that the address is never null, hence a [`NonZero`]`<usize>` is required.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct MaybeFault<F>(NonZero<usize>, marker::PhantomData<F>)
where
    F: Faultable;

impl<F> MaybeFault<F>
where
    F: Faultable,
{
    /// Construct a maybe-fault region with the target parameters.
    ///
    /// This is safe, but the act of reading from a maybe-fault region is still unsafe.
    #[inline]
    pub const fn new(target_address: NonZero<usize>) -> Self {
        Self(target_address, marker::PhantomData::<F>)
    }

    /// Determine the address of the maybe-fault region.
    #[inline]
    pub const fn address(self) -> NonZero<usize> {
        let Self(target_address, ..) = self;

        target_address
    }
}

impl<F> MaybeFault<F>
where
    F: Faultable,
{
    /// Read from the maybe-fault pointer.
    ///
    /// This is identical to a volatile memory read.
    ///
    /// # Safety
    ///
    /// See [`ffi::read`] for safety concerns.
    #[inline]
    pub unsafe fn read(&self, target_subsystem: Subsystem) -> Option<F> {
        let &Self(target_source, ..) = self;

        // SAFETY: Safety constraints are delegated to the caller.
        unsafe {
            ffi::read(
                target_subsystem,
                ptr::with_exposed_provenance_mut(NonZero::<usize>::get(target_source)),
            )
        }
    }

    /// Write to the maybe-fault pointer. Returns *true* if the write did *not fault*, *false* otherwise.
    ///
    /// This is identical to a volatile memory read.
    ///
    /// # Safety
    ///
    /// See [`ffi::write`] for safety concerns.
    #[inline]
    pub unsafe fn write(&self, target_subsystem: Subsystem, target_value: F) -> bool {
        let &Self(target_address, ..) = self;

        // SAFETY: Safety constraints are delegated to the caller.
        unsafe {
            ffi::write(
                target_subsystem,
                ptr::with_exposed_provenance_mut(NonZero::<usize>::get(target_address)),
                target_value,
            )
        }
    }
}

impl<F> Deref for MaybeFault<F>
where
    F: Faultable,
{
    type Target = NonZero<usize>;

    fn deref(&self) -> &Self::Target {
        let Self(target_address, ..) = self;

        target_address
    }
}

impl<F> DerefMut for MaybeFault<F>
where
    F: Faultable,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        let Self(target_address, ..) = self;

        target_address
    }
}

#[cfg(test)]
mod test {
    use core::num::NonZero;
    use core::ptr;

    use core::fmt::Debug;

    use crate::behavior::Faultable;
    use crate::ffi::Subsystem;

    use super::MaybeFault;

    /// An address inside the unmapped bottom page, so any access faults.
    ///
    /// It is `8`-aligned to satisfy the machine-word coherence requirement of the routines.
    const FAULTING_ADDRESS: NonZero<usize> =
        NonZero::new(0x50).expect("the faulting address must be non-zero");

    /// Acquire the subsystem token for a test.
    ///
    /// # Safety
    ///
    /// The test harness does not install a conflicting `SIGSEGV`/`SIGBUS` handler concurrently.
    unsafe fn subsystem() -> Subsystem {
        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        unsafe { Subsystem::initialize() }.expect("the catalejo subsystem must initialize")
    }

    /// The size of a single page, in bytes.
    fn page_size() -> usize {
        // SAFETY: `_SC_PAGESIZE` is a valid `sysconf` query with no preconditions.
        let target_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };

        assert!(target_size > 0, "the page size must be a positive value");

        target_size as usize
    }

    /// Map a single fresh, zero-filled anonymous page with the given protection.
    fn map(protection: libc::c_int) -> *mut libc::c_void {
        // SAFETY: A kernel-chosen anonymous private mapping; no fixed address is requested.
        let target_page = unsafe {
            libc::mmap(
                ptr::null_mut(),
                page_size(),
                protection,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            )
        };

        assert_ne!(target_page, libc::MAP_FAILED, "mmap must succeed");

        target_page
    }

    /// Re-protect a previously-mapped page.
    fn protect(target_page: *mut libc::c_void, protection: libc::c_int) {
        // SAFETY: `target_page` is a live, page-aligned mapping of `page_size()` bytes.
        let target_outcome = unsafe { libc::mprotect(target_page, page_size(), protection) };

        assert_eq!(target_outcome, 0, "mprotect must succeed");
    }

    /// Unmap a page previously obtained from [`map`].
    fn unmap(target_page: *mut libc::c_void) {
        // SAFETY: `target_page` and `page_size()` are exactly the mapping returned by `map`.
        let target_outcome = unsafe { libc::munmap(target_page, page_size()) };

        assert_eq!(target_outcome, 0, "munmap must succeed");
    }

    /// The non-zero, provenance-exposed address backing a [`MaybeFault`].
    ///
    /// The provenance is exposed so the opaque fault routines are assumed to
    /// access the underlying allocation, matching the out-of-abstract-machine
    /// read the address is reconstituted for.
    fn address<T>(target_pointer: *const T) -> NonZero<usize> {
        NonZero::new(target_pointer.expose_provenance()).expect("the address must be non-zero")
    }

    /// Environment marker selecting the child role of the stack-overflow test.
    const OVERFLOW_CHILD_VARIABLE: &str = "CATALEJO_OVERFLOW_CHILD";

    /// Recurse with a non-trivial, escaping frame until the stack guard faults.
    #[inline(never)]
    #[allow(
        unconditional_recursion,
        reason = "we're looking towards a stack overflow"
    )]
    fn overflow_the_stack(target_depth: u64) -> u64 {
        let target_frame = core::hint::black_box([target_depth; 256]);

        // Consume the recursive result so the call cannot be tail-optimized away.
        target_depth.wrapping_add(overflow_the_stack(core::hint::black_box(
            target_depth.wrapping_add(target_frame[0]),
        )))
    }

    #[test]
    fn read_of_a_valid_address_yields_the_value() {
        let target_value = 0xDEAD_BEEF_u64;

        let maybe = MaybeFault::<u64>::new(address(&raw const target_value));

        // SAFETY: The address is a live, aligned `u64` for the duration of the read.
        let target_outcome = unsafe { maybe.read(subsystem()) };

        assert_eq!(target_outcome, Some(0xDEAD_BEEF_u64));
    }

    #[test]
    fn read_of_a_faulting_address_yields_none() {
        let maybe = MaybeFault::<u64>::new(FAULTING_ADDRESS);

        // SAFETY: The address is aligned; the read faults and is caught by the subsystem.
        let target_outcome = unsafe { maybe.read(subsystem()) };

        assert_eq!(target_outcome, None);
    }

    #[test]
    fn write_to_a_valid_address_succeeds_and_takes_effect() {
        let mut target_slot = 0_u64;

        let maybe = MaybeFault::<u64>::new(address((&raw mut target_slot).cast_const()));

        // SAFETY: The address is a live, aligned, exclusively-borrowed `u64`.
        let target_outcome = unsafe { maybe.write(subsystem(), 0xC0FF_EE00) };

        assert!(target_outcome);
        assert_eq!(target_slot, 0xC0FF_EE00);
    }

    #[test]
    fn write_to_a_faulting_address_reports_failure() {
        let maybe = MaybeFault::<u64>::new(FAULTING_ADDRESS);

        // SAFETY: The address is aligned; the write faults and is caught by the subsystem.
        let target_outcome = unsafe { maybe.write(subsystem(), 0xFF) };

        assert!(!target_outcome);
    }

    /// Exercise a real mapping lifecycle: a live anonymous page reads back the
    /// written sentinel, and the *same* address faults to `None` once its
    /// backing page is taken away.
    #[test]
    fn read_tracks_a_pages_lifecycle() {
        const SENTINEL: u64 = 0x1234_5678_9ABC_DEF0;

        let target_page = map(libc::PROT_READ | libc::PROT_WRITE);

        // A page is page-aligned, hence trivially `8`-aligned for a `u64`.
        let maybe = MaybeFault::<u64>::new(address(target_page.cast::<u64>().cast_const()));

        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        let target_subsystem = unsafe { subsystem() };

        // SAFETY: The page is mapped read-write and aligned for the duration of the call.
        assert!(
            unsafe { maybe.write(target_subsystem, SENTINEL) },
            "writing through a live mapping must succeed",
        );

        // SAFETY: The page is still mapped readable and aligned.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            Some(SENTINEL),
            "reading a live mapping must yield the written sentinel",
        );

        // Revoke access in place rather than `munmap`-ing: a bare unmapped
        // address can be concurrently re-mapped by another (parallel) test,
        // which would make the fault below non-deterministic. Reserving the
        // address as `PROT_NONE` pins it while still forcing the access to fault.
        protect(target_page, libc::PROT_NONE);

        // The address is unchanged, but the page is now inaccessible: the access
        // faults and the subsystem converts the hardware exception into `None`.
        //
        // SAFETY: The address is aligned; the read faults and is caught by the subsystem.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            None,
            "reading the now-inaccessible page must fault to `None`",
        );

        unmap(target_page);
    }

    /// A read-only page must serve reads but reject writes: the store faults to
    /// `false` and the underlying memory is left untouched.
    #[test]
    fn write_to_a_readonly_page_faults_while_reads_succeed() {
        let target_page = map(libc::PROT_READ);

        // A freshly-mapped anonymous page reads back as zero.
        let maybe = MaybeFault::<u64>::new(address(target_page.cast::<u64>().cast_const()));

        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        let target_subsystem = unsafe { subsystem() };

        // SAFETY: The page is mapped readable and aligned.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            Some(0),
            "a fresh read-only page must read back as zero",
        );

        // SAFETY: The address is aligned; the write faults on the read-only page.
        assert!(
            !unsafe { maybe.write(target_subsystem, 0xDEAD_BEEF) },
            "writing to a read-only page must fault to `false`",
        );

        // The faulting store must not have committed any bytes.
        //
        // SAFETY: The page is still mapped readable and aligned.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            Some(0),
            "a faulting write must leave the memory untouched",
        );

        unmap(target_page);
    }

    /// `mprotect`-ing a live read-write page down to read-only must flip writes
    /// from succeeding to faulting, while reads keep observing the last value.
    #[test]
    fn revoking_write_permission_flips_writes_to_faulting() {
        const SENTINEL: u64 = 0x0BAD_F00D_DEAD_C0DE;

        let target_page = map(libc::PROT_READ | libc::PROT_WRITE);

        let maybe = MaybeFault::<u64>::new(address(target_page.cast::<u64>().cast_const()));

        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        let target_subsystem = unsafe { subsystem() };

        // SAFETY: The page is mapped read-write and aligned.
        assert!(
            unsafe { maybe.write(target_subsystem, SENTINEL) },
            "the initial write to a read-write page must succeed",
        );

        protect(target_page, libc::PROT_READ);

        // SAFETY: The address is aligned; the write now faults on the read-only page.
        assert!(
            !unsafe { maybe.write(target_subsystem, !SENTINEL) },
            "the write must fault once write permission is revoked",
        );

        // SAFETY: The page is still mapped readable and aligned.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            Some(SENTINEL),
            "the read must still observe the value from before the revocation",
        );

        unmap(target_page);
    }

    /// A `PROT_NONE` page is inaccessible: both reads and writes must fault.
    #[test]
    fn an_inaccessible_page_faults_on_both_reads_and_writes() {
        let target_page = map(libc::PROT_NONE);

        let maybe = MaybeFault::<u64>::new(address(target_page.cast::<u64>().cast_const()));

        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        let target_subsystem = unsafe { subsystem() };

        // SAFETY: The address is aligned; the read faults on the inaccessible page.
        assert_eq!(
            unsafe { maybe.read(target_subsystem) },
            None,
            "reading a `PROT_NONE` page must fault to `None`",
        );

        // SAFETY: The address is aligned; the write faults on the inaccessible page.
        assert!(
            !unsafe { maybe.write(target_subsystem, 0xFF) },
            "writing a `PROT_NONE` page must fault to `false`",
        );

        unmap(target_page);
    }

    /// Drive every primitive width through both a live mapping (round-trip) and
    /// the unmapped bottom page (fault), so each per-width routine is covered.
    fn exercise_width<F>(target_subsystem: Subsystem, target_value: F)
    where
        F: Faultable + Copy + PartialEq + Debug,
    {
        let target_page = map(libc::PROT_READ | libc::PROT_WRITE);

        // A page is page-aligned, hence aligned for any primitive width.
        let live = MaybeFault::<F>::new(address(target_page.cast::<F>().cast_const()));

        // SAFETY: The page is mapped read-write and aligned for `F`.
        assert!(
            unsafe { live.write(target_subsystem, target_value) },
            "a width-{} write to a live mapping must succeed",
            size_of::<F>() * 8,
        );

        // SAFETY: The page is still mapped readable and aligned for `F`.
        assert_eq!(
            unsafe { live.read(target_subsystem) },
            Some(target_value),
            "a width-{} read must yield the written value",
            size_of::<F>() * 8,
        );

        unmap(target_page);

        let faulting = MaybeFault::<F>::new(FAULTING_ADDRESS);

        // SAFETY: `0x50` is aligned for any primitive width; the read faults.
        assert_eq!(
            unsafe { faulting.read(target_subsystem) },
            None,
            "a width-{} read of an unmapped page must fault to `None`",
            size_of::<F>() * 8,
        );

        // SAFETY: `0x50` is aligned for any primitive width; the write faults.
        assert!(
            !unsafe { faulting.write(target_subsystem, target_value) },
            "a width-{} write to an unmapped page must fault to `false`",
            size_of::<F>() * 8,
        );
    }

    #[test]
    fn every_primitive_width_round_trips_and_faults() {
        // SAFETY: The test harness does not race the subsystem with a conflicting handler.
        let target_subsystem = unsafe { subsystem() };

        exercise_width::<u8>(target_subsystem, 0xA5);
        exercise_width::<u16>(target_subsystem, 0xA55A);
        exercise_width::<u32>(target_subsystem, 0xDEAD_BEEF);
        exercise_width::<u64>(target_subsystem, 0x1234_5678_9ABC_DEF0);
    }

    /// A genuine stack overflow must still reach the host's overflow handler:
    /// catalejo installs itself for `SIGSEGV`, but on finding the faulting `%RIP`
    /// outside its routine section it must *chain* to the previously-installed
    /// handler (here, the Rust runtime's stack-overflow reporter) rather than
    /// swallow the fault. Verified out-of-process because it ends in `abort`.
    #[test]
    fn a_stack_overflow_still_reaches_the_host_handler() {
        use std::os::unix::process::ExitStatusExt;

        // Child role: install the subsystem, then deliberately overflow.
        if std::env::var_os(OVERFLOW_CHILD_VARIABLE).is_some() {
            // Suppress the core dump from the intentional abort. `RLIMIT_CORE`
            // alone is ignored when `core_pattern` is a pipe (e.g. systemd), so
            // also clear the dumpable flag, which suppresses cores unconditionally.
            let target_limit = libc::rlimit {
                rlim_cur: 0,
                rlim_max: 0,
            };

            // SAFETY: A well-formed `rlimit` paired with a valid resource id, and
            // a parameterless `prctl` operation; neither has further preconditions.
            unsafe {
                libc::setrlimit(libc::RLIMIT_CORE, &target_limit);
                libc::prctl(libc::PR_SET_DUMPABLE, 0);
            }

            // SAFETY: The test harness does not race the subsystem with a conflicting handler.
            let _ = unsafe { subsystem() };

            // Overflow on a dedicated, small-stacked thread so the runtime's
            // per-thread guard page and alternate stack are in force.
            std::thread::Builder::new()
                .stack_size(256 * 1024)
                .spawn(|| core::hint::black_box(overflow_the_stack(0)))
                .expect("the overflow thread must spawn")
                .join()
                .expect("unreachable: the thread aborts the whole process");

            return;
        }

        // Parent role: re-run *this* test in a child wearing the child role.
        let target_executable =
            std::env::current_exe().expect("the test executable path must be available");

        let target_output = std::process::Command::new(target_executable)
            .args([
                "--exact",
                "maybe::test::a_stack_overflow_still_reaches_the_host_handler",
            ])
            .env(OVERFLOW_CHILD_VARIABLE, "1")
            .output()
            .expect("the child process must run");

        let target_stderr = String::from_utf8_lossy(&target_output.stderr);

        // The child must die abnormally: not exit cleanly, and crucially not be
        // silently rescued into a `None`-style return by the catalejo handler.
        assert!(
            !target_output.status.success(),
            "the child must terminate abnormally; stderr was:\n{target_stderr}",
        );
        assert!(
            target_output.status.signal().is_some(),
            "the child must be terminated by a signal; stderr was:\n{target_stderr}",
        );

        // The host handler's message is proof the fault chained all the way
        // through catalejo to the runtime's stack-overflow reporter.
        assert!(
            target_stderr.contains("overflowed its stack"),
            "the host stack-overflow handler must have run; stderr was:\n{target_stderr}",
        );
    }
}
