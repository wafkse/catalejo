//! Device-backed integration tests for catalejo peephole reads.
//!
//! These engage the current process as their own target, so they require the mirilla module to be
//! loaded and `/dev/mirilla` to be present. They are `#[ignore]`d so a default `cargo test` on a
//! host without the device skips them cleanly. Run them where the device exists with:
//!
//! ```text
//! cargo test -p catalejo --test self_peephole -- --include-ignored
//! ```
//!
//! In CI they run inside the mirilla VM against a loaded module.

use catalejo::{
    address::ViAddr,
    manage::{Granule, Manage, Memoize},
    target::Target,
};

use catalejo_fault::ffi::Subsystem;
use catalejo_sys::ffi;

/// Engage the current process as a target, panicking with a clear message on failure.
fn engage_self() -> Target {
    // SAFETY: This test binary installs no competing `SIGSEGV`/`SIGBUS` handlers, and no other
    // thread registers one while this runs, so the subsystem may claim them.
    let target_subsystem =
        unsafe { Subsystem::initialize() }.expect("the fault subsystem should initialize");

    Target::engage(target_subsystem, std::process::id() as libc::pid_t)
        .expect("engaging the current process should succeed")
}

/// Compute the target address of a value in this process' own address space.
fn address_of<T>(target_value: &T) -> ViAddr {
    ViAddr::new(core::ptr::from_ref(target_value) as ffi::binding::virtual_address_t)
}

#[test]
#[ignore = "requires the mirilla device"]
fn reads_a_known_word() {
    let manager = Memoize::new(engage_self());

    let cell = Box::new(0xDEAD_BEEF_CAFE_BABE_u64);
    let target_address = address_of(&*cell);

    let target_access = manager
        .source::<u64>(target_address)
        .expect("the peephole should open")
        .expect("the granule should admit a word");

    let observed = target_access
        .foreign()
        .expect("the handle should validate")
        .read();

    assert_eq!(
        observed,
        Some(*cell),
        "the read should observe the written word"
    );
}

#[test]
#[ignore = "requires the mirilla device"]
fn reads_distinct_words() {
    let manager = Memoize::new(engage_self());

    let words: Vec<u64> = (0..128_u64)
        .map(|index| index.wrapping_mul(0x9E37_79B9_7F4A_7C15))
        .collect();

    for (expected_index, expected_word) in words.iter().enumerate() {
        let target_address = address_of(expected_word);

        let observed = manager
            .source::<u64>(target_address)
            .expect("the peephole should open")
            .expect("the granule should admit a word")
            .foreign()
            .expect("the handle should validate")
            .read();

        assert_eq!(
            observed,
            Some(*expected_word),
            "word {expected_index} should read back",
        );
    }
}

#[test]
#[ignore = "requires the mirilla device"]
fn memoizes_a_peephole_by_page() {
    let manager = Memoize::new(engage_self());

    let page = Granule::Page.size();

    // Span two pages and align the base up to a page, so `here` and `near` share a page while `far`
    // lands in the next one. Over-allocating by a word keeps every probe inside resident memory.
    let region: Vec<u64> = vec![0; (2 * page as usize) / size_of::<u64>() + 2];
    let base_address = core::ptr::from_ref(&region[0]) as ffi::binding::virtual_address_t;
    let aligned_base = (base_address + (page - 1)) & !(page - 1);

    let here = ViAddr::new(aligned_base);
    let near = ViAddr::new(aligned_base + size_of::<u64>() as ffi::binding::virtual_address_t);
    let far = ViAddr::new(aligned_base + page);

    let peephole_id = |target_address| {
        manager
            .source::<u64>(target_address)
            .expect("the peephole should open")
            .expect("the window should admit a word")
            .peephole()
            .id()
    };

    assert_eq!(
        peephole_id(here),
        peephole_id(near),
        "same-page addresses should reuse one peephole",
    );

    assert_ne!(
        peephole_id(here),
        peephole_id(far),
        "page-distant addresses should resolve to distinct peepholes",
    );
}
