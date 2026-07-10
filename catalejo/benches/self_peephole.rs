//! Self-peephole microbenchmarks.
//!
//! These measure the end-to-end latency of reading a machine word out of a peephole window that
//! points back into the benchmarking process itself. The suite isolates the three costs a read can
//! incur, matching the layered memoization of the manager.
//!
//! * The memoized `Manage` resolution, an address to frame quantization plus a sharded map lookup.
//! * The lazy `ioctl` plus `mmap` that opens a fresh window the first time a granule is touched.
//! * The lazy fault-in of a window page the first time that page is read through the mapping.
//!
//! They require the `mirilla` kernel module to be loaded and its device present, and they engage
//! the current process as their own target so no second process is needed. When the environment is
//! unavailable the suite prints a skip notice and returns rather than failing, so it stays runnable
//! everywhere.

use std::hint::black_box;

use catalejo::{
    address::ViAddr,
    manage::{Manage, Rebased},
    peephole::Foreign,
    target::Target,
};

use catalejo_fault::ffi::Subsystem;

use criterion::{BatchSize, Criterion, Throughput, criterion_group, criterion_main};

/// The size of the default peephole granule (a `Hugepage`), the span of a single window.
const GRANULE_BYTES: usize = 2 * 1024 * 1024;

/// The number of bytes a single peephole read observes, one machine word.
///
/// The read groups declare this as their criterion throughput so the reported figures carry a
/// read-speed dimension (bytes per second, which criterion renders as GiB/s) rather than a bare
/// per-iteration latency.
const READ_BYTES: u64 = size_of::<u64>() as u64;

/// The number of distinct granules the backing region covers.
///
/// A cold-path benchmark opens one window per iteration, so the region must offer enough distinct,
/// fully-backed granules that a run rotates through fresh frames rather than re-touching one.
const GRANULE_COUNT: usize = 32;

/// The addressable span of the backing region.
const REGION_BYTES: usize = GRANULE_BYTES * GRANULE_COUNT;

/// A granule-aligned, fully-resident region of this process' own memory used as the peephole target.
///
/// The region is over-allocated by a granule and then addressed from its first granule-aligned byte,
/// so every window opened over it lands wholly inside resident memory and never straddles into an
/// unmapped neighbour.
struct Region {
    // NOTE: Keep the backing storage alive for the whole run, the peephole windows point into it.
    _backing: Box<[u64]>,

    /// The first granule-aligned address within the backing storage.
    base_address: usize,
}

impl Region {
    /// Allocate and fully populate a granule-aligned backing region.
    fn new() -> Self {
        let word_count = (REGION_BYTES + GRANULE_BYTES) / size_of::<u64>();

        let mut backing = vec![0_u64; word_count].into_boxed_slice();

        // NOTE: Touch every word so all backing pages are resident, the cold path then measures the
        // fault-in of the peephole mapping rather than of the target region.
        for (index, slot) in backing.iter_mut().enumerate() {
            *slot = index as u64;
        }

        let raw_address = backing.as_ptr() as usize;
        let base_address = (raw_address + (GRANULE_BYTES - 1)) & !(GRANULE_BYTES - 1);

        Self {
            _backing: backing,
            base_address,
        }
    }

    /// Determine the target address of the first word of the granule at the specified index.
    fn granule_address(&self, granule_index: usize) -> ViAddr {
        ViAddr::new(self.base_address + (granule_index % GRANULE_COUNT) * GRANULE_BYTES)
    }
}

/// Engage the current process as a target, returning [`None`] when the environment is unavailable.
fn engage_self() -> Option<Target> {
    // SAFETY: The benchmark binary installs no competing `SIGSEGV`/`SIGBUS` handlers, and no other
    // thread registers one while this runs, so the subsystem may claim them.
    let target_subsystem = unsafe { Subsystem::initialize() }?;

    Target::engage(target_subsystem, std::process::id() as libc::pid_t).ok()
}

/// Materialize a warmed [`Foreign`] handle, opening the window and faulting its page in once.
fn warm_foreign(manager: &Rebased, target_address: ViAddr) -> Foreign<u64> {
    let target_access = manager
        .source::<u64>(target_address)
        .expect("peephole should open")
        .expect("granule should admit a word");

    let target_foreign = target_access.foreign().expect("handle should validate");

    // NOTE: Touch the word once so the mapping page is resident before it is timed.
    assert!(
        black_box(target_foreign.read()).is_some(),
        "self-peephole read should observe resident memory",
    );

    target_foreign
}

fn self_peephole(criterion: &mut Criterion) {
    let Some(target_engaged) = engage_self() else {
        eprintln!(
            "skipping self-peephole benchmarks: the mirilla device or fault subsystem is unavailable"
        );

        return;
    };

    let region = Region::new();

    let manager = Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

    let warm_address = region.granule_address(0);
    let warm_foreign = warm_foreign(&manager, warm_address);

    // The resolution and window-open costs, none of which read a word, so the group carries no
    // throughput dimension. Their figures are latencies, not read speeds.
    {
        let mut resolve_group = criterion.benchmark_group("self-peephole-resolve");

        // The memoized manager lookup in isolation, an address quantized to a frame and resolved
        // against an already-open window, without any read.
        resolve_group.bench_function("resolve-only", |bencher| {
            bencher.iter(|| black_box(manager.absolute::<u64>(black_box(warm_address))));
        });

        // The cold open, a fresh manager per iteration forces a `source` miss that pays the `ioctl`
        // plus `mmap` to create a window, but stops short of reading so no page is faulted in.
        resolve_group.bench_function("open-cold", |bencher| {
            let mut granule_cursor = 0_usize;

            bencher.iter_batched(
                || {
                    let fresh_manager =
                        Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

                    let target_address = region.granule_address(granule_cursor);

                    granule_cursor += 1;

                    (fresh_manager, target_address)
                },
                |(fresh_manager, target_address)| {
                    black_box(
                        fresh_manager
                            .source::<u64>(target_address)
                            .expect("source succeeds"),
                    )
                },
                BatchSize::SmallInput,
            );
        });

        resolve_group.finish();
    }

    // The read costs, every benchmark observes exactly one machine word, so the group declares a
    // per-iteration throughput of that word and criterion reports each figure as a read speed.
    {
        let mut read_group = criterion.benchmark_group("self-peephole-read");

        read_group.throughput(Throughput::Bytes(READ_BYTES));

        // The fault-protected read in isolation, over a pre-resolved handle whose page is resident.
        read_group.bench_function("read-hot", |bencher| {
            bencher.iter(|| black_box(warm_foreign.read()));
        });

        // The whole hot path, a memoized lookup followed by a resident read.
        read_group.bench_function("resolve-read-hot", |bencher| {
            bencher.iter(|| {
                let target_access = manager
                    .absolute::<u64>(black_box(warm_address))
                    .expect("window is open");

                let target_foreign = target_access.foreign().expect("handle validates");

                black_box(target_foreign.read())
            });
        });

        // The warm find-or-open path, identical to the hot path but paying the fallible `source`
        // surface that would open a window on a miss. Here every frame is already resident.
        read_group.bench_function("source-read-warm", |bencher| {
            bencher.iter(|| {
                let target_access = manager
                    .source::<u64>(black_box(warm_address))
                    .expect("source succeeds")
                    .expect("granule admits a word");

                let target_foreign = target_access.foreign().expect("handle validates");

                black_box(target_foreign.read())
            });
        });

        // The cold open plus first read, a fresh-manager miss followed by a read that faults the
        // brand-new mapping page in. The difference against `open_cold` is the fault-in cost.
        read_group.bench_function("open-read-cold", |bencher| {
            let mut granule_cursor = 0_usize;

            bencher.iter_batched(
                || {
                    let fresh_manager =
                        Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

                    let target_address = region.granule_address(granule_cursor);

                    granule_cursor += 1;

                    (fresh_manager, target_address)
                },
                |(fresh_manager, target_address)| {
                    let target_access = fresh_manager
                        .source::<u64>(target_address)
                        .expect("source succeeds")
                        .expect("granule admits a word");

                    let target_foreign = target_access.foreign().expect("handle validates");

                    black_box(target_foreign.read())
                },
                BatchSize::SmallInput,
            );
        });

        read_group.finish();
    }
}

criterion_group!(self_peephole_benches, self_peephole);
criterion_main!(self_peephole_benches);
