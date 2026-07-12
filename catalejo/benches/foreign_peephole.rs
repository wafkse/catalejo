//! Foreign-peephole microbenchmarks.
//!
//! These measure the end-to-end cost of peepholing into a *separate* target process, the case a
//! [`Rebased`] manager is built for. A child process maps a granule-tiled, fully-resident region and
//! reports its base address, then the benchmarking parent engages that child and reads and mirrors
//! its memory across the three peephole warmth states.
//!
//! * A hot peephole, its window already open and its pages already faulted into the parent mapping.
//! * A warm peephole, reached through the fallible find-or-open surface over an already-open window.
//! * A cold peephole, opened fresh so the `ioctl` plus `mmap` and the first page fault-in are paid.
//!
//! They require the mirilla kernel module loaded and its device present, and engaging a foreign
//! process needs `CAP_SYS_PTRACE`, so the suite runs as root inside the mirilla VM. When the
//! environment is unavailable the suite prints a skip notice and returns rather than failing, so it
//! stays runnable everywhere.
//!
//! The same binary doubles as the target. Re-executed with a marker in its environment it takes the
//! child role, maps the region, and blocks so the region stays mapped for the whole measurement.

use std::hint::black_box;
use std::io::{BufRead, BufReader, Read, Write};
use std::mem::MaybeUninit;
use std::process::{Child, Command, Stdio};

use catalejo::{
    address::ViAddr,
    manage::{Manage, Rebased},
    peephole::Foreign,
    target::Target,
};

use catalejo_fault::ffi::Subsystem;

use catalejo_sys::ffi;
use criterion::{
    BatchSize, BenchmarkGroup, BenchmarkId, Criterion, Throughput, measurement::WallTime,
};

/// The environment marker that selects the target-child role of this binary.
const TARGET_CHILD_VARIABLE: &str = "CATALEJO_FOREIGN_TARGET_CHILD";

/// The size of the default peephole granule (a `Hugepage`), the span of a single window.
const GRANULE_BYTES: usize = 2 * 1024 * 1024;

/// The number of bytes a single peephole read observes, one machine word.
///
/// The read group declares this as its criterion throughput so the reported figures carry a
/// read-speed dimension rather than a bare per-iteration latency.
const READ_BYTES: u64 = size_of::<u64>() as u64;

/// The number of distinct granules the target region covers.
///
/// A cold-path benchmark opens one window per iteration, so the region must offer enough distinct,
/// fully-backed granules that a run rotates through fresh frames rather than re-touching one.
const GRANULE_COUNT: usize = 32;

/// The addressable span of the target region.
const REGION_BYTES: usize = GRANULE_BYTES * GRANULE_COUNT;

/// A granule-aligned, fully-resident region of a process' own memory used as the foreign target.
///
/// The region is over-allocated by a granule and then addressed from its first granule-aligned byte,
/// so every window opened over it lands wholly inside resident memory and never straddles into an
/// unmapped neighbour. Only the child role builds one, the parent works from the reported base alone.
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

        // NOTE: Touch every word so all backing pages are resident, the parent then measures the
        // fault-in of its own peephole mapping rather than of the target region.
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
}

/// Run the target-child role, mapping the region, reporting its base, and blocking until released.
fn run_target_child() {
    let region = Region::new();

    // Report the granule-aligned base so the parent can address the region in the child's space.
    let mut target_output = std::io::stdout();
    writeln!(target_output, "{}", region.base_address)
        .expect("the target child must report its region base");
    target_output
        .flush()
        .expect("the target child must flush its region base");

    // Block on standard input so the region stays mapped. The parent closing or killing the child
    // ends the read, at which point the child exits and the region is torn down.
    let mut target_sink = [0_u8; 1];
    let _ = std::io::stdin().read(&mut target_sink);

    // Keep the region alive right up to the exit so no page is reclaimed mid-measurement.
    black_box(&region);
}

/// A spawned foreign target process holding a resident, granule-tiled region.
struct ForeignTarget {
    /// The child process handle, killed on drop.
    target_process: Child,

    /// The granule-aligned base of the child region, an address in the child's own space.
    base_address: usize,
}

impl ForeignTarget {
    /// Spawn this binary in its target-child role and read back the region base it reports.
    fn spawn() -> Option<Self> {
        let target_executable = std::env::current_exe().ok()?;

        let mut target_process = Command::new(target_executable)
            .env(TARGET_CHILD_VARIABLE, "1")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .ok()?;

        // Read the single base-address line the child prints before it blocks. Its standard input
        // stays open through the retained `Child`, so the child keeps the region mapped until drop.
        let target_output = target_process.stdout.take()?;
        let mut target_reader = BufReader::new(target_output);
        let mut target_line = String::new();
        target_reader.read_line(&mut target_line).ok()?;

        let base_address = target_line.trim().parse::<usize>().ok()?;

        Some(Self {
            target_process,
            base_address,
        })
    }

    /// Determine the process identifier of the target child.
    fn pid(&self) -> libc::pid_t {
        self.target_process.id() as libc::pid_t
    }

    /// Determine the target address of the first word of the granule at the specified index.
    fn granule_address(&self, granule_index: usize) -> ViAddr {
        ViAddr::new(
            (self.base_address + (granule_index % GRANULE_COUNT) * GRANULE_BYTES)
                as ffi::binding::virtual_address_t,
        )
    }
}

impl Drop for ForeignTarget {
    fn drop(&mut self) {
        // Tear the child down and reap it, releasing its region.
        let _ = self.target_process.kill();
        let _ = self.target_process.wait();
    }
}

/// Engage a foreign target, returning [`None`] when the environment forbids it.
///
/// A [`None`] means the fault subsystem could not initialize, or the mirilla device is absent, or
/// the caller lacks the `CAP_SYS_PTRACE` that engaging a foreign address space demands.
fn engage_foreign(target_child: &ForeignTarget) -> Option<Target> {
    // SAFETY: The benchmark binary installs no competing `SIGSEGV`/`SIGBUS` handlers, and no other
    // thread registers one while this runs, so the subsystem may claim them.
    let target_subsystem = unsafe { Subsystem::initialize() }?;

    Target::engage(target_subsystem, target_child.pid()).ok()
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
        "foreign-peephole read should observe resident memory",
    );

    target_foreign
}

/// Time a whole-structure hot mirror of a `[u64; N]` payload out of an already-open granule window.
///
/// The window is open over the warm granule, so the handle is materialized once and the mapping
/// pages the payload spans are faulted in by a priming copy. The timed loop then measures the
/// steady-state byte-granular stream into a caller-owned buffer, so its figure is a mirror speed
/// rather than the one-off open and fault-in cost.
fn bench_copy_hot<const N: usize>(
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
        "foreign-peephole copy should mirror resident memory",
    );

    let payload_bytes = size_of::<[u64; N]>() as u64;

    copy_group.throughput(Throughput::Bytes(payload_bytes));

    copy_group.bench_with_input(
        BenchmarkId::new("copy-hot", payload_bytes),
        &payload_bytes,
        |bencher, _| {
            bencher.iter(|| black_box(target_foreign.copy(&mut target_buffer).is_ok()));
        },
    );
}

fn foreign_peephole(criterion: &mut Criterion) {
    let Some(target_child) = ForeignTarget::spawn() else {
        eprintln!("skipping foreign-peephole benchmarks: the target child could not be spawned");

        return;
    };

    let Some(target_engaged) = engage_foreign(&target_child) else {
        eprintln!(
            "skipping foreign-peephole benchmarks: the mirilla device or fault subsystem is \
             unavailable, or engaging a foreign process is not permitted (CAP_SYS_PTRACE)"
        );

        return;
    };

    let manager = Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

    let warm_address = target_child.granule_address(0);
    let warm_foreign = warm_foreign(&manager, warm_address);

    // The resolution and window-open costs, neither of which reads a word, so the group carries no
    // throughput dimension. Their figures are latencies, not read speeds.
    {
        let mut resolve_group = criterion.benchmark_group("foreign-peephole-resolve");

        // The memoized manager lookup in isolation, an address quantized to a frame and resolved
        // against an already-open window, without any read.
        resolve_group.bench_function("resolve-only", |bencher| {
            bencher.iter(|| black_box(manager.absolute::<u64>(black_box(warm_address))));
        });

        // The cold open, a fresh manager per iteration forces a `source` miss that pays the `ioctl`
        // plus `mmap` to open a window into the foreign space, but stops short of reading so no page
        // is faulted in.
        resolve_group.bench_function("open-cold", |bencher| {
            let mut granule_cursor = 0_usize;

            bencher.iter_batched(
                || {
                    let fresh_manager =
                        Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

                    let target_address = target_child.granule_address(granule_cursor);

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
        let mut read_group = criterion.benchmark_group("foreign-peephole-read");

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
        // brand-new mapping page in. The difference against `open-cold` is the fault-in cost.
        read_group.bench_function("open-read-cold", |bencher| {
            let mut granule_cursor = 0_usize;

            bencher.iter_batched(
                || {
                    let fresh_manager =
                        Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

                    let target_address = target_child.granule_address(granule_cursor);

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
    // word out of the foreign window, so the group carries a per-payload throughput and criterion
    // reports each figure as a mirror speed. The hot series spans several payload widths to expose
    // how the byte-granular copy amortizes across page boundaries, and one fixed payload is mirrored
    // warm and cold so the open and fault-in cost stands out against the resident steady state.
    {
        let mut copy_group = criterion.benchmark_group("foreign-peephole-copy");

        // Hot mirrors over the warm window across growing payloads.
        bench_copy_hot::<512>(&mut copy_group, &manager, warm_address); // 4 KiB
        bench_copy_hot::<8192>(&mut copy_group, &manager, warm_address); // 64 KiB
        bench_copy_hot::<65536>(&mut copy_group, &manager, warm_address); // 512 KiB

        // A fixed 64 KiB payload mirrored warm and cold, sharing one reused destination so the two
        // figures differ only by the window-open and fault-in the cold path pays.
        const SCRATCH_WORDS: usize = 8192;

        let scratch_bytes = size_of::<[u64; SCRATCH_WORDS]>() as u64;
        let mut scratch_buffer = MaybeUninit::<[u64; SCRATCH_WORDS]>::uninit();

        copy_group.throughput(Throughput::Bytes(scratch_bytes));

        // The warm find-or-open path, the fallible `source` surface over an already-open window
        // followed by a resident mirror.
        copy_group.bench_function("copy-warm", |bencher| {
            bencher.iter(|| {
                let target_foreign = manager
                    .source::<[u64; SCRATCH_WORDS]>(black_box(warm_address))
                    .expect("source succeeds")
                    .expect("granule admits the payload")
                    .foreign()
                    .expect("handle validates");

                black_box(target_foreign.copy(&mut scratch_buffer).is_ok())
            });
        });

        // The cold open plus first mirror, a fresh-manager miss whose window-open and fault-in fold
        // into the byte-granular stream.
        copy_group.bench_function("open-copy-cold", |bencher| {
            let mut granule_cursor = 0_usize;

            bencher.iter_batched_ref(
                || {
                    let fresh_manager =
                        Rebased::new(target_engaged.duplicate().expect("target should duplicate"));

                    let target_address = target_child.granule_address(granule_cursor);

                    granule_cursor += 1;

                    (fresh_manager, target_address)
                },
                |(fresh_manager, target_address)| {
                    let target_foreign = fresh_manager
                        .source::<[u64; SCRATCH_WORDS]>(*target_address)
                        .expect("source succeeds")
                        .expect("granule admits the payload")
                        .foreign()
                        .expect("handle validates");

                    black_box(target_foreign.copy(&mut scratch_buffer).is_ok())
                },
                BatchSize::SmallInput,
            );
        });

        copy_group.finish();
    }
}

fn main() {
    // Re-executed with the marker set, this binary is the target the parent peepholes into rather
    // than the benchmark driver, so it maps its region and blocks before criterion is ever touched.
    if std::env::var_os(TARGET_CHILD_VARIABLE).is_some() {
        run_target_child();

        return;
    }

    let mut criterion = Criterion::default().configure_from_args();

    foreign_peephole(&mut criterion);

    criterion.final_summary();
}
