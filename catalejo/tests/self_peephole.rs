//! Device-backed integration tests for catalejo peephole reads and writes.
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
    manage::{Granule, Manage, memoize::Memoize},
    target::Target,
};

use catalejo_sys::ffi;

/// Engage the current process as a target, panicking with a clear message on failure.
fn engage_self() -> Target {
    Target::engage(std::process::id() as libc::pid_t)
        .expect("engaging the current process should succeed")
}

/// Compute the target address of a value in this process' own address space.
fn address_of<T>(target_value: &T) -> ViAddr {
    ViAddr::new(core::ptr::from_ref(target_value) as ffi::binding::virtual_address_t)
}

/// Determine the host page size.
fn page_size() -> usize {
    // SAFETY: `_SC_PAGESIZE` is a valid `sysconf` query with no preconditions.
    let target_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };

    assert!(target_size > 0, "the page size should be positive");

    target_size as usize
}

/// Allocate one anonymous page with the requested protection.
fn map_page(target_protection: libc::c_int) -> *mut u8 {
    // SAFETY: This requests a kernel-chosen private anonymous mapping.
    let target_page = unsafe {
        libc::mmap(
            core::ptr::null_mut(),
            page_size(),
            target_protection,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
            -1,
            0,
        )
    };

    assert_ne!(target_page, libc::MAP_FAILED, "the test page should map");

    target_page.cast::<u8>()
}

/// Change the protection on a test page.
fn protect_page(target_page: *mut u8, target_protection: libc::c_int) {
    // SAFETY: `target_page` is a live page-sized mapping returned by `map_page`.
    let target_code = unsafe {
        libc::mprotect(
            target_page.cast::<libc::c_void>(),
            page_size(),
            target_protection,
        )
    };

    assert_eq!(target_code, 0, "the test page protection should change");
}

/// Release a test page.
fn unmap_page(target_page: *mut u8) {
    // SAFETY: `target_page` is the start of a live page-sized mapping returned by `map_page`.
    let target_code = unsafe { libc::munmap(target_page.cast::<libc::c_void>(), page_size()) };

    assert_eq!(target_code, 0, "the test page should unmap");
}

/// Convert a raw target pointer into a Catalejo virtual address.
fn address_of_pointer<T>(target_pointer: *const T) -> ViAddr {
    ViAddr::new(target_pointer.expose_provenance() as ffi::binding::virtual_address_t)
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
fn writes_a_known_word() {
    let target_page = map_page(libc::PROT_READ | libc::PROT_WRITE);
    let target_slot = target_page.cast::<u64>();
    let target_initial = 0xDEAD_BEEF_CAFE_BABE_u64;
    let target_replacement = 0x0123_4567_89AB_CDEF_u64;

    // SAFETY: The anonymous page is writable and page-aligned, hence aligned for `u64`.
    unsafe { target_slot.write(target_initial) };

    let manager = Memoize::new(engage_self());
    let target_foreign = manager
        .source::<u64>(address_of_pointer(target_slot.cast_const()))
        .expect("the peephole should open")
        .expect("the granule should admit a word")
        .foreign()
        .expect("the handle should validate");

    assert!(
        target_foreign.write(target_replacement),
        "the protected foreign write should succeed",
    );

    // SAFETY: The target page remains mapped and readable. A volatile read observes the memory
    // reached through its original mapping rather than the peephole alias.
    assert_eq!(unsafe { target_slot.read_volatile() }, target_replacement);
    assert_eq!(target_foreign.read(), Some(target_replacement));

    drop(target_foreign);
    drop(manager);
    unmap_page(target_page);
}

#[test]
#[ignore = "requires the mirilla device"]
fn writes_every_primitive_width() {
    let target_page = map_page(libc::PROT_READ | libc::PROT_WRITE);
    let manager = Memoize::new(engage_self());

    macro_rules! assert_write_round_trip {
        ($target_type:ty, $target_offset:expr, $target_value:expr) => {{
            // SAFETY: Every chosen offset is in-page and aligned for its primitive type.
            let target_slot = unsafe { target_page.add($target_offset).cast::<$target_type>() };
            let target_foreign = manager
                .source::<$target_type>(address_of_pointer(target_slot.cast_const()))
                .expect("the peephole should open")
                .expect("the granule should admit the primitive")
                .foreign()
                .expect("the handle should validate");

            assert!(target_foreign.write($target_value));
            // SAFETY: The target slot remains mapped and readable.
            assert_eq!(unsafe { target_slot.read_volatile() }, $target_value);
        }};
    }

    assert_write_round_trip!(u8, 0, 0xA5_u8);
    assert_write_round_trip!(u16, 2, 0xA55A_u16);
    assert_write_round_trip!(u32, 4, 0xDEAD_BEEF_u32);
    assert_write_round_trip!(u64, 8, 0x1234_5678_9ABC_DEF0_u64);

    drop(manager);
    unmap_page(target_page);
}

#[test]
#[ignore = "requires the mirilla device"]
fn write_faults_when_target_becomes_read_only() {
    let target_page = map_page(libc::PROT_READ | libc::PROT_WRITE);
    let target_slot = target_page.cast::<u64>();
    let target_initial = 0x0BAD_F00D_DEAD_C0DE_u64;

    // SAFETY: The anonymous page is writable and aligned for `u64`.
    unsafe { target_slot.write(target_initial) };

    let manager = Memoize::new(engage_self());
    let target_foreign = manager
        .source::<u64>(address_of_pointer(target_slot.cast_const()))
        .expect("the peephole should open")
        .expect("the granule should admit a word")
        .foreign()
        .expect("the handle should validate");

    assert_eq!(
        target_foreign.read(),
        Some(target_initial),
        "the writable view should resolve while the target permits writes",
    );

    protect_page(target_page, libc::PROT_READ);

    assert!(
        !target_foreign.write(!target_initial),
        "the write should fault after target write permission is revoked",
    );
    // SAFETY: The page remains readable after the protection change.
    assert_eq!(unsafe { target_slot.read_volatile() }, target_initial);

    drop(target_foreign);
    drop(manager);
    unmap_page(target_page);
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

#[test]
#[ignore = "requires the mirilla device"]
fn copies_an_arbitrary_byte_span() {
    let manager = Memoize::new(engage_self());
    let target_source = Box::new(*b"runtime-sized-copy-span");
    let target_address = address_of(&target_source[0]);
    let target_access = manager
        .source::<u8>(target_address)
        .expect("the peephole should open")
        .expect("the window should admit a byte");
    let mut target_buffer = [core::mem::MaybeUninit::<u8>::uninit(); 23];
    let target_foreign = target_access.foreign().expect("the handle should validate");
    let target_copy = target_foreign
        .bytes(&mut target_buffer)
        .expect("the runtime byte span should fit");

    assert!(target_copy.complete());
    assert_eq!(target_copy.bytes(), target_source.as_slice());
}

#[test]
#[ignore = "requires the mirilla device"]
fn appends_an_arbitrary_byte_span() {
    let manager = Memoize::new(engage_self());
    let source = Box::new(*b"runtime-sized-copy-span");
    let address = address_of(&source[0]);
    let access = manager
        .source::<u8>(address)
        .expect("the peephole should open")
        .expect("the window should admit a byte");
    let foreign = access.foreign().expect("the handle should validate");
    let mut buffer = b"prefix-".to_vec();
    let copy = foreign
        .append(&mut buffer, source.len())
        .expect("the runtime byte span should fit");

    assert!(copy.complete());
    assert_eq!(copy.bytes(), source.as_slice());
    assert_eq!(buffer, b"prefix-runtime-sized-copy-span");
}
