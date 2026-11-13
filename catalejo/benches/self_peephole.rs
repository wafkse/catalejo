//! Self-peephole microbenchmarks.
//!
//! The suite observes the benchmarking process through its own peephole mappings.
//! To show where read latency comes from, separate groups measure cached manager
//! resolution, window creation through `ioctl` and `mmap`, and first-touch page
//! population. Resident-read groups then measure access after those setup costs
//! have been paid.
//!
//! Larger values use `Foreign::copy`, so a separate group reports copy throughput
//! across several payload sizes after populating the mapping with a priming copy.
//!
//! Self-engagement needs a loaded Mirilla module and access to its device but no
//! target child process. If that environment is unavailable, the suite prints a
//! skip notice and returns.

use std::hint::black_box;
use std::mem::MaybeUninit;

use catalejo::{
    address::ViAddr,
    manage::{Manage, rebased::Rebased},
    peephole::Foreign,
    target::Target,
};

use catalejo_sys::ffi;
use criterion::{
    BatchSize, BenchmarkGroup, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main,
    measurement::WallTime,
};

/// The size of the default peephole granule (a `Hugepage`), the span of a single window.
const GRANULE_BYTES: usize = 2 * 1024 * 1024;

/// Bytes observed per primitive read, used for Criterion throughput reporting.
const READ_BYTES: u64 = size_of::<u64>() as u64;

/// The number of distinct granules the backing region covers.
///
/// Cold-path measurements rotate through these backed granules, opening a
/// window per iteration so cached windows do not replace the setup being timed.
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
        // fault-in of the peephole mapping over resident target pages.
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
        ViAddr::new(
            (self.base_address + (granule_index % GRANULE_COUNT) * GRANULE_BYTES)
                as ffi::binding::virtual_address_t,
        )
    }
}

/// Engage the current process as a target, returning [`None`] when the environment is unavailable.
fn engage_self() -> Option<Target> {
    Target::engage(std::process::id() as libc::pid_t).ok()
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

/// Measure copy throughput for a `[u64; N]` payload in a resident window.
///
/// A priming copy populates the mapping before timing begins. Each iteration
/// copies into caller-owned storage, excluding window creation and first-touch
/// faults from the measurement. Criterion reports throughput by payload size.
fn bench_copy<const N: usize>(
    copy_group: &mut BenchmarkGroup<'_, WallTime>,
    manager: &Rebased,
    warm_address: ViAddr,
) {
    let target_foreign = manager
        .absolute::<[u64; N]>(warm_address)
        .expect("window is open")
        .foreign()
        .expect("handle validates");

    let mut target_buffer = MaybeUninit::<[u64; N]>::uninit();

    // NOTE: Prime the mapping so every page the payload spans is resident before it is timed.
    assert!(
        black_box(target_foreign.copy(&mut target_buffer)).is_ok(),
        "self-peephole copy should mirror resident memory",
    );

    let payload_bytes = size_of::<[u64; N]>() as u64;

    copy_group.throughput(Throughput::Bytes(payload_bytes));

    copy_group.bench_with_input(
        BenchmarkId::from_parameter(payload_bytes),
        &payload_bytes,
        |bencher, _| {
            bencher.iter(|| black_box(target_foreign.copy(&mut target_buffer).is_ok()));
        },
    );
}

fn self_peephole(criterion: &mut Criterion) {
    let Some(target_engaged) = engage_self() else {
        eprintln!(
            "skipping self-peephole benchmarks: the mirilla device or fault backend is unavailable"
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

    // The whole-structure mirror costs, every benchmark streams an aggregate wider than a machine
    // word out of the warm window, so the group carries a per-payload throughput and criterion
    // reports each figure as a mirror speed. The sizes span from a single page to many, so the
    // series exposes how the byte-granular copy amortizes across page boundaries.
    {
        let mut copy_group = criterion.benchmark_group("self-peephole-copy");

        // A `4 KiB` payload, a single page mirrored in whole.
        bench_copy::<512>(&mut copy_group, &manager, warm_address);

        // A `64 KiB` payload, spanning many pages within the warm granule.
        bench_copy::<8192>(&mut copy_group, &manager, warm_address);

        // A `512 KiB` payload, the widest mirror the half-granule structure bound admits.
        bench_copy::<65536>(&mut copy_group, &manager, warm_address);

        copy_group.finish();
    }
}

criterion_group!(self_peephole_benches, self_peephole);
criterion_main!(self_peephole_benches);
