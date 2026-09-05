//! Device-backed lifecycle tests for the bounded LRU manager.
//!
//! The parent test engages a child process whose resident region spans several hugepage granules.
//! The test is ignored on ordinary hosts because it requires the Mirilla device and foreign-process
//! engagement. The child-role test itself exits immediately during an ordinary test run.

use std::{
    hint::black_box,
    io::{BufRead, BufReader, Read, Write},
    num::NonZeroUsize,
    process::{Child, Command, Stdio},
    sync::{Arc, Barrier},
    thread,
};

use catalejo::{
    address::ViAddr,
    manage::{Manage, lru::Lru},
    peephole::Peephole,
    target::Target,
};
use catalejo_fault::ffi::Subsystem;
use catalejo_sys::ffi;

/// The environment marker selecting the child role of this integration-test binary.
const TARGET_CHILD_VARIABLE: &str = "CATALEJO_LRU_TARGET_CHILD";

/// The fixed window size used by the default manager granule.
const GRANULE_BYTES: usize = 2 * 1024 * 1024;

/// The number of resident granules exposed by the child process.
const GRANULE_COUNT: usize = 8;

/// A granule-aligned resident region retained by the child process.
// NOTE(invariant): `_backing` stays alive for the complete child role and `base_address` is hugepage aligned with at least `GRANULE_COUNT` complete resident granules following it.
struct Region {
    /// The allocation that owns every target byte used by the parent.
    _backing: Box<[u64]>,

    /// The first hugepage-aligned byte inside the backing allocation.
    base_address: usize,
}

impl Region {
    /// Allocate and fault in the complete target region.
    fn new() -> Self {
        let region_bytes = GRANULE_BYTES * GRANULE_COUNT;
        let word_count = (region_bytes + GRANULE_BYTES) / size_of::<u64>();
        let mut backing = vec![0_u64; word_count].into_boxed_slice();

        for (target_index, target_slot) in backing.iter_mut().enumerate() {
            *target_slot = target_index as u64;
        }

        let target_raw_address = backing.as_ptr() as usize;
        let base_address = (target_raw_address + (GRANULE_BYTES - 1)) & !(GRANULE_BYTES - 1);
        let _backing = backing;

        Self {
            _backing,
            base_address,
        }
    }
}

/// A child process retaining the resident foreign target region.
// NOTE(invariant): `target_process` remains alive while `base_address` is used, so every address derived from that base names the child address space represented by the stored process identifier.
struct ForeignTarget {
    /// The child process that owns the target region.
    target_process: Child,

    /// The reported hugepage-aligned base in the child address space.
    base_address: usize,
}

impl ForeignTarget {
    /// Spawn this test binary in its child role and recover its reported region base.
    fn spawn() -> Self {
        let target_executable = std::env::current_exe().expect("the test executable should exist");
        let mut target_process = Command::new(target_executable)
            .arg("target_child")
            .arg("--exact")
            .arg("--nocapture")
            .env(TARGET_CHILD_VARIABLE, "1")
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .expect("the foreign target child should spawn");
        let target_output = target_process
            .stderr
            .take()
            .expect("the child stderr should be piped");
        let mut target_reader = BufReader::new(target_output);
        let mut target_line = String::new();

        target_reader
            .read_line(&mut target_line)
            .expect("the child should report its target base");

        let base_address = target_line
            .trim()
            .parse::<usize>()
            .expect("the reported target base should be numeric");

        Self {
            target_process,
            base_address,
        }
    }

    /// Determine the operating-system process identifier of the child.
    fn pid(&self) -> libc::pid_t {
        let Self { target_process, .. } = self;

        target_process.id() as libc::pid_t
    }

    /// Engage the child address space through Catalejo.
    fn engage(&self) -> Target {
        Target::engage(Self::pid(self)).expect("the foreign target should engage")
    }

    /// Determine the first word address of one resident target granule.
    fn address(&self, target_granule: usize) -> ViAddr {
        let Self { base_address, .. } = self;
        let target_offset = (target_granule % GRANULE_COUNT) * GRANULE_BYTES;
        let target_address = base_address + target_offset;

        ViAddr::new(target_address as ffi::binding::virtual_address_t)
    }
}

impl Drop for ForeignTarget {
    fn drop(&mut self) {
        let Self { target_process, .. } = self;

        let _ = target_process.kill();
        let _ = target_process.wait();
    }
}

/// Run the resident target role when this test binary is spawned by the parent test.
#[test]
fn target_child() {
    match std::env::var_os(TARGET_CHILD_VARIABLE) {
        None => {}
        Some(_) => {
            let target_region = Region::new();
            let Region { base_address, .. } = target_region;
            let mut target_output = std::io::stderr();

            writeln!(target_output, "{base_address}")
                .expect("the child should report its region base");
            target_output
                .flush()
                .expect("the child should flush its region base");

            let mut target_sink = [0_u8; 1];
            let _ = std::io::stdin().read(&mut target_sink);

            black_box(&target_region);
        }
    }
}

/// Exercise global recency and `Arc` lifetime behavior against real peepholes.
#[test]
#[ignore = "requires the mirilla device and foreign-process engagement"]
fn lru_preserves_recency_and_peephole_lifetime() {
    // SAFETY: This test binary installs no competing synchronous fault handlers while it runs.
    let _ = unsafe { Subsystem::initialize() }.expect("the fault subsystem should initialize");

    let target_child = ForeignTarget::spawn();
    let target_a = ForeignTarget::address(&target_child, 0);
    let target_b = ForeignTarget::address(&target_child, 1);
    let target_c = ForeignTarget::address(&target_child, 2);
    let target_d = ForeignTarget::address(&target_child, 3);
    let target_limit_two = NonZeroUsize::new(2).expect("the fixture limit should be nonzero");
    let target_limit_one = NonZeroUsize::MIN;

    let manager = Lru::new(ForeignTarget::engage(&target_child), target_limit_two);
    let access_a = manager
        .source::<u64>(target_a)
        .expect("source A should succeed")
        .expect("source A should be admitted");
    let id_a = Peephole::id(access_a.peephole());
    drop(access_a);
    let access_b = manager
        .source::<u64>(target_b)
        .expect("source B should succeed")
        .expect("source B should be admitted");
    drop(access_b);
    let access_a = manager
        .source::<u64>(target_a)
        .expect("source A should promote")
        .expect("source A should remain admitted");
    assert_eq!(Peephole::id(access_a.peephole()), id_a);
    drop(access_a);
    drop(
        manager
            .source::<u64>(target_c)
            .expect("source C should succeed")
            .expect("source C should be admitted"),
    );
    assert!(manager.absolute::<u64>(target_a).is_some());
    assert!(manager.absolute::<u64>(target_b).is_none());

    let manager = Lru::new(ForeignTarget::engage(&target_child), target_limit_two);
    drop(
        manager
            .source::<u64>(target_a)
            .expect("source A should succeed")
            .expect("source A should be admitted"),
    );
    drop(
        manager
            .source::<u64>(target_b)
            .expect("source B should succeed")
            .expect("source B should be admitted"),
    );
    drop(
        manager
            .absolute::<u64>(target_a)
            .expect("absolute A should find the live window"),
    );
    drop(
        manager
            .source::<u64>(target_c)
            .expect("source C should succeed")
            .expect("source C should be admitted"),
    );
    assert!(manager.absolute::<u64>(target_a).is_none());
    assert!(manager.absolute::<u64>(target_b).is_some());

    let manager = Lru::new(ForeignTarget::engage(&target_child), target_limit_one);
    let access_a = manager
        .source::<u64>(target_a)
        .expect("source A should succeed")
        .expect("source A should be admitted");
    let id_a = Peephole::id(access_a.peephole());
    let foreign_a = access_a.foreign().expect("foreign A should validate");
    drop(
        manager
            .source::<u64>(target_b)
            .expect("source B should succeed")
            .expect("source B should be admitted"),
    );
    assert!(foreign_a.read().is_some());
    let resurrected_a = manager
        .source::<u64>(target_a)
        .expect("resurrected A should succeed")
        .expect("resurrected A should be admitted");
    assert_eq!(Peephole::id(resurrected_a.peephole()), id_a);
    drop(resurrected_a);
    drop(foreign_a);

    let manager = Lru::new(ForeignTarget::engage(&target_child), target_limit_two);
    let access_a = manager
        .source::<u64>(target_a)
        .expect("source A should succeed")
        .expect("source A should be admitted");
    let foreign_a = access_a.foreign().expect("foreign A should validate");
    drop(
        manager
            .source::<u64>(target_b)
            .expect("source B should succeed")
            .expect("source B should be admitted"),
    );
    drop(
        manager
            .source::<u64>(target_c)
            .expect("source C should succeed")
            .expect("source C should be admitted"),
    );
    drop(foreign_a);
    assert!(manager.absolute::<u64>(target_a).is_some());
    assert!(manager.absolute::<u64>(target_b).is_none());

    let manager = Arc::new(Lru::new(
        ForeignTarget::engage(&target_child),
        target_limit_two,
    ));
    let target_barrier = Arc::new(Barrier::new(8));
    let mut target_threads = Vec::new();

    for _ in 0..8 {
        let target_manager = Arc::clone(&manager);
        let target_barrier = Arc::clone(&target_barrier);

        target_threads.push(thread::spawn(move || {
            target_barrier.wait();

            let target_access = target_manager
                .source::<u64>(target_d)
                .expect("concurrent source should succeed")
                .expect("concurrent source should be admitted");

            Peephole::id(target_access.peephole())
        }));
    }

    let target_ids = target_threads
        .into_iter()
        .map(|target_thread| {
            target_thread
                .join()
                .expect("the source thread should finish")
        })
        .collect::<Vec<_>>();
    let target_first_id = target_ids[0];

    assert!(
        target_ids
            .iter()
            .all(|target_id| *target_id == target_first_id)
    );
}
