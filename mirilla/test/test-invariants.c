/*
 * Mapping Invariant Test Suite for the Mirilla Module
 *
 * The peephole mmap declares its access intent while remaining private. Writable mappings resolve
 * the target with `FOLL_WRITE`; `pfn_mkwrite` then upgrades the mixed-PFN PTE instead of allowing
 * observer-side COW. Every view remains whole-length, non-executable, and fixed in protection after
 * creation. Shared, partial-length, and non-zero-offset mappings remain invalid.
 * - The VMA operations refuse relocation and reprotection (`EPERM`).
 * - Core mm backs the rest before the module hook is even consulted:
 *   `SB_I_NOEXEC` on the anon-inode mount blocks executable mappings
 *   (`EPERM`), `VM_DONTEXPAND` blocks growth (`EFAULT`) and the cleared
 *   `VM_MAYWRITE` blocks writable reprotection (`EACCES`).
 */

#define _GNU_SOURCE

#include "test-harness.h"

/*
 * Shared fixture: a live self-peephole the cases try to map badly.
 */
typedef struct {
    int mirilla_fd;
    mirilla_map_target_id_t target_id;
    int peephole_fd;
    void *region;
} invariant_fixture_t;

static invariant_fixture_t fixture = { -1, 0, -1, NULL };

static int fixture_construct(void)
{
    fixture.mirilla_fd = mirilla_open_device();
    if (fixture.mirilla_fd < 0)
        return -1;

    fixture.region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
    if (!fixture.region)
        return -1;

    if (!MIRILLA_COMMAND_IS_OK(mirilla_engage(fixture.mirilla_fd, getpid(), &fixture.target_id)))
        return -1;

    mirilla_map_peephole_id_t peephole_id = 0;

    if (!MIRILLA_COMMAND_IS_OK(mirilla_peephole(
            fixture.mirilla_fd, fixture.target_id, (virtual_address_t)fixture.region,
            (virtual_address_t)fixture.region + TEST_REGION_SIZE, &peephole_id,
            &fixture.peephole_fd)))
        return -1;

    return 0;
}

static void fixture_destruct(void)
{
    if (fixture.peephole_fd >= 0)
        close(fixture.peephole_fd);
    if (fixture.region)
        munmap(fixture.region, TEST_REGION_SIZE);
    if (fixture.mirilla_fd >= 0) {
        mirilla_disengage(fixture.mirilla_fd, fixture.target_id);
        close(fixture.mirilla_fd);
    }
}

/*
 * Attempt a view mapping expected to be refused, asserting the errno.
 */
static int expect_mmap_rejection(int prot, int flags, size_t length, off_t offset,
                                 int expected_errno)
{
    errno = 0;

    void *view = mmap(NULL, length, prot, flags, fixture.peephole_fd, offset);

    if (view != MAP_FAILED) {
        munmap(view, length);
        fprintf(stderr, "mmap unexpectedly succeeded\n");
        return -1;
    }

    if (errno != expected_errno) {
        fprintf(stderr, "mmap failed with errno %d (%s), expected %d (%s)\n", errno,
                strerror(errno), expected_errno, strerror(expected_errno));
        return -1;
    }

    return 0;
}

/*
 * An executable view is refused by core mm before the module is consulted:
 * the peephole file lives on the anon-inode pseudo mount, whose
 * `SB_I_NOEXEC` makes `do_mmap` fail `PROT_EXEC` with `EPERM`. The
 * module's own `VM_EXEC` check (`EACCES`) remains as defense in depth.
 */
static int test_reject_prot_exec(void)
{
    return expect_mmap_rejection(PROT_READ | PROT_EXEC, MAP_PRIVATE, TEST_REGION_SIZE, 0, EPERM);
}

/* NOTE(invariant): Disallow shared mappings. */
static int test_reject_map_shared(void)
{
    return expect_mmap_rejection(PROT_READ, MAP_SHARED, TEST_REGION_SIZE, 0, EINVAL);
}

static int test_reject_writable_map_shared(void)
{
    return expect_mmap_rejection(PROT_READ | PROT_WRITE, MAP_SHARED, TEST_REGION_SIZE, 0, EINVAL);
}

/* NOTE(invariant): Must map the entire peephole, no partial mappings. */
static int test_reject_partial_length(void)
{
    return expect_mmap_rejection(PROT_READ, MAP_PRIVATE, TEST_REGION_SIZE - 4096, 0, EINVAL);
}

/* NOTE(invariant): Reject a non-zero file offset. */
static int test_reject_nonzero_offset(void)
{
    return expect_mmap_rejection(PROT_READ, MAP_PRIVATE, TEST_REGION_SIZE, 4096, EINVAL);
}

/*
 * Growth is refused by core mm before the module is consulted:
 * `VM_DONTEXPAND` makes an expanding mremap fail with `EFAULT`.
 */
static int test_reject_mremap_grow(void)
{
    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, fixture.peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    errno = 0;
    void *grown = mremap(view, TEST_REGION_SIZE, 2 * TEST_REGION_SIZE, 0);

    int rejection_status = 0;
    if (grown != MAP_FAILED) {
        fprintf(stderr, "mremap grow unexpectedly succeeded\n");
        rejection_status = -1;
    } else if (errno != EFAULT) {
        fprintf(stderr, "mremap grow failed with errno %d (%s), expected EFAULT\n", errno,
                strerror(errno));
        rejection_status = -1;
    }

    munmap(view, TEST_REGION_SIZE);
    return rejection_status;
}

/* NOTE(invariant): Disallow remapping the peephole VMA. */
static int test_reject_mremap_move(void)
{
    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, fixture.peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    /* Reserve a destination so the move is forced through `move_vma`. */
    void *scratch = mmap(NULL, TEST_REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(scratch != MAP_FAILED, "failed to reserve scratch range");

    errno = 0;
    void *moved =
        mremap(view, TEST_REGION_SIZE, TEST_REGION_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, scratch);

    int rejection_status = 0;
    if (moved != MAP_FAILED) {
        fprintf(stderr, "mremap move unexpectedly succeeded\n");
        munmap(moved, TEST_REGION_SIZE);
        rejection_status = -1;
    } else if (errno != EPERM) {
        fprintf(stderr, "mremap move failed with errno %d (%s), expected EPERM\n", errno,
                strerror(errno));
        rejection_status = -1;
    } else {
        munmap(scratch, TEST_REGION_SIZE);
    }

    munmap(view, TEST_REGION_SIZE);
    return rejection_status;
}

/*
 * A writable reprotection is refused by core mm: the mmap handler cleared
 * `VM_MAYWRITE`, so the access check fails with `EACCES` before the module
 * hook runs.
 */
static int test_reject_mprotect_write(void)
{
    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, fixture.peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    errno = 0;
    int mprotect_code = mprotect(view, TEST_REGION_SIZE, PROT_READ | PROT_WRITE);

    int rejection_status = 0;
    if (mprotect_code == 0) {
        fprintf(stderr, "mprotect to writable unexpectedly succeeded\n");
        rejection_status = -1;
    } else if (errno != EACCES) {
        fprintf(stderr,
                "mprotect to writable failed with errno %d (%s), expected "
                "EACCES\n",
                errno, strerror(errno));
        rejection_status = -1;
    }

    munmap(view, TEST_REGION_SIZE);
    return rejection_status;
}

/*
 * NOTE(invariant): Disallow changing page protections. `PROT_NONE` passes
 * the core access check (it adds no access), so this one reaches the
 * module's `mprotect` operation and its `EPERM`.
 */
static int test_reject_mprotect_none(void)
{
    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, fixture.peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    errno = 0;
    int mprotect_code = mprotect(view, TEST_REGION_SIZE, PROT_NONE);

    int rejection_status = 0;
    if (mprotect_code == 0) {
        fprintf(stderr, "mprotect to PROT_NONE unexpectedly succeeded\n");
        rejection_status = -1;
    } else if (errno != EPERM) {
        fprintf(stderr,
                "mprotect to PROT_NONE failed with errno %d (%s), expected "
                "EPERM\n",
                errno, strerror(errno));
        rejection_status = -1;
    }

    munmap(view, TEST_REGION_SIZE);
    return rejection_status;
}

/*
 * A writable private peephole view bypasses observer-side COW and mutates the target PFN directly.
 * The target value is restored so the shared fixture remains reusable.
 */
static int test_explicit_writable_mapping_updates_target(void)
{
    mirilla_map_peephole_id_t peephole_id = 0;
    int peephole_fd = -1;

    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
               fixture.mirilla_fd, fixture.target_id, (virtual_address_t)fixture.region,
               (virtual_address_t)fixture.region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)),
           "failed to create peephole");

    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap writable peephole view");

    uint32_t replacement = MAGIC_VALUE_5;
    ASSERT(catalejo_write_u32(harness_exception_runtime, (uint32_t *)view, &replacement) ==
               CATALEJO_OUTCOME_SUCCESS,
           "protected write through writable peephole failed");
    ASSERT(((uint32_t *)fixture.region)[0] == replacement, "writable peephole did not update "
                                                           "target memory");

    ((uint32_t *)fixture.region)[0] = MAGIC_VALUE_1;

    munmap(view, TEST_REGION_SIZE);
    close(peephole_fd);
    return 0;
}

/*
 * Writable mmap intent is not debugger-style forced write. If the target VMA no longer permits
 * writes, a refault is refused even though the observer view itself is writable.
 */
static int test_writable_mapping_respects_target_permissions(void)
{
    mirilla_map_peephole_id_t peephole_id = 0;
    int peephole_fd = -1;
    void *view = MAP_FAILED;
    int status = -1;

    if (!MIRILLA_COMMAND_IS_OK(mirilla_peephole(
            fixture.mirilla_fd, fixture.target_id, (virtual_address_t)fixture.region,
            (virtual_address_t)fixture.region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)))
        goto out;

    view = mmap(NULL, TEST_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, peephole_fd, 0);
    if (view == MAP_FAILED)
        goto out;

    if (mprotect(fixture.region, TEST_REGION_SIZE, PROT_READ) != 0)
        goto out;

    uint32_t replacement = MAGIC_VALUE_4;
    if (catalejo_write_u32(harness_exception_runtime, (uint32_t *)view, &replacement) !=
        CATALEJO_OUTCOME_ERROR) {
        fprintf(stderr, "write unexpectedly succeeded against read-only target VMA\n");
        goto restore;
    }

    status = 0;

restore:
    if (mprotect(fixture.region, TEST_REGION_SIZE, PROT_READ | PROT_WRITE) != 0)
        status = -1;
out:
    if (view != MAP_FAILED)
        munmap(view, TEST_REGION_SIZE);
    if (peephole_fd >= 0)
        close(peephole_fd);
    return status;
}

/* A well-formed mapping must still succeed after all the rejections. */
static int test_wellformed_mapping_still_works(void)
{
    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, fixture.peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    ASSERT(verify_region_faultable(view, TEST_REGION_SIZE, MAGIC_VALUE_1) == 0, "view does not "
                                                                                "mirror the "
                                                                                "observed region");

    munmap(view, TEST_REGION_SIZE);
    return 0;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    print_banner("MIRILLA MAPPING INVARIANT SUITE");

    if (harness_fault_initialize() < 0)
        return EXIT_FAILURE;

    if (fixture_construct() < 0) {
        fprintf(stderr, COLOR_RED "Failed to construct the peephole fixture\n" COLOR_RESET);
        fixture_destruct();
        return EXIT_FAILURE;
    }

    RUN_TEST("Reject PROT_EXEC View", test_reject_prot_exec);
    RUN_TEST("Reject MAP_SHARED View", test_reject_map_shared);
    RUN_TEST("Reject Writable MAP_SHARED View", test_reject_writable_map_shared);
    RUN_TEST("Reject Partial-Length View", test_reject_partial_length);
    RUN_TEST("Reject Non-Zero Offset View", test_reject_nonzero_offset);
    RUN_TEST("Reject mremap Growth", test_reject_mremap_grow);
    RUN_TEST("Reject mremap Move", test_reject_mremap_move);
    RUN_TEST("Reject mprotect to Writable", test_reject_mprotect_write);
    RUN_TEST("Reject mprotect to PROT_NONE", test_reject_mprotect_none);
    RUN_TEST("Writable Private View Updates Target", test_explicit_writable_mapping_updates_target);
    RUN_TEST("Writable View Respects Target Permissions",
             test_writable_mapping_respects_target_permissions);
    RUN_TEST("Well-Formed View Still Works", test_wellformed_mapping_still_works);

    fixture_destruct();

    print_summary();

    return suite_status();
}
