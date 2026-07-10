/*
 * Ioctl Error-Path Test Suite for the Mirilla Module
 *
 * Negative cases against the command dispatcher and the map command
 * handlers, pinning down the errno of every refusal:
 * - Foreign magic and categories -> `ENOTSUPP`, unknown map commands ->
 *   `ENOTTY`, unreadable argument structures -> `EFAULT`.
 * - Engagement of a nonexistent pid -> `ESRCH`.
 * - Peepholes against nonexistent (or already-disengaged) targets ->
 *   `ENOENT`; malformed address ranges -> `EINVAL`.
 * - Disengagement is single-shot: a second disengage -> `ENOENT`.
 */

#include "test-harness.h"

/*
 * NOTE(errno): `ENOTSUPP` is kernel-internal and absent from userspace
 * errno.h; the dispatcher leaks it verbatim for foreign magic/categories.
 */
#define MIRILLA_ENOTSUPP 524

/* A pid outside any real pid range (`pid_max` caps at 1 << 22). */
#define NONEXISTENT_PID 0x7fffffff

/* A target id no engagement ever produced. */
#define BOGUS_TARGET_ID 0xdeadbeef

/*
 * Expect an ioctl result to be a refusal with the given errno.
 */
static int expect_ioctl_rejection(long ioctl_code, int expected_errno, const char *what)
{
    if (MIRILLA_COMMAND_IS_OK(ioctl_code)) {
        fprintf(stderr, "%s unexpectedly succeeded\n", what);
        return -1;
    }

    if (errno != expected_errno) {
        fprintf(stderr, "%s failed with errno %d (%s), expected %d\n", what, errno, strerror(errno),
                expected_errno);
        return -1;
    }

    return 0;
}

/* Foreign magic must bounce off the dispatcher. */
static int test_bad_magic(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    unsigned int cmd = ('X' << MIRILLA_IOCTL_MAGIC_OFFSET) |
                       (MIRILLA_COMMAND_CATEGORY_MAP << MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET) |
                       MIRILLA_COMMAND_MAP_ENGAGE;

    errno = 0;
    int rejection_status = expect_ioctl_rejection(ioctl(mirilla_fd, cmd, NULL), MIRILLA_ENOTSUPP,
                                                  "foreign-magic ioctl");

    close(mirilla_fd);
    return rejection_status;
}

/* An unknown category must bounce off the dispatcher. */
static int test_bad_category(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    unsigned int cmd = MIRILLA_COMMAND_ENCODE(0xff, MIRILLA_COMMAND_MAP_ENGAGE);

    errno = 0;
    int rejection_status = expect_ioctl_rejection(ioctl(mirilla_fd, cmd, NULL), MIRILLA_ENOTSUPP,
                                                  "foreign-category ioctl");

    close(mirilla_fd);
    return rejection_status;
}

/* An unknown map command enumeration must be refused. */
static int test_bad_map_command(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, 0x42);

    errno = 0;
    int rejection_status =
        expect_ioctl_rejection(ioctl(mirilla_fd, cmd, NULL), ENOTTY, "unknown map command");

    close(mirilla_fd);
    return rejection_status;
}

/* An unreadable argument structure must be refused. */
static int test_bad_argument_pointer(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    unsigned int cmd =
        MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_ENGAGE);

    errno = 0;
    int rejection_status =
        expect_ioctl_rejection(ioctl(mirilla_fd, cmd, NULL), EFAULT, "engage with a NULL argument");

    close(mirilla_fd);
    return rejection_status;
}

/* Engaging a pid no process holds must be refused. */
static int test_engage_nonexistent_pid(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;

    errno = 0;
    int rejection_status =
        expect_ioctl_rejection(mirilla_engage(mirilla_fd, NONEXISTENT_PID, &target_id), ESRCH,
                               "engage of a nonexistent pid");

    close(mirilla_fd);
    return rejection_status;
}

/* A peephole against a never-engaged target id must be refused. */
static int test_peephole_bad_target_id(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SMALL, MAGIC_VALUE_1);
    ASSERT(region != NULL, "failed to allocate test region");

    errno = 0;
    int rejection_status = expect_ioctl_rejection(
        mirilla_peephole(mirilla_fd, BOGUS_TARGET_ID, (virtual_address_t)region,
                         (virtual_address_t)region + TEST_REGION_SMALL, NULL, NULL),
        ENOENT, "peephole against a bogus target id");

    munmap(region, TEST_REGION_SMALL);
    close(mirilla_fd);
    return rejection_status;
}

/* Malformed peephole address ranges must be refused. */
static int test_peephole_malformed_ranges(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
    ASSERT(region != NULL, "failed to allocate test region");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    virtual_address_t start = (virtual_address_t)region;
    virtual_address_t end = start + TEST_REGION_SIZE;

    struct {
        const char *what;
        virtual_address_t start_address, end_address;
    } malformed_ranges[] = {
        { "unaligned start address", start + 4, end },
        { "unaligned end address", start, end - 4 },
        { "empty address range", start, start },
        { "inverted address range", end, start },
    };

    int rejection_status = 0;

    for (size_t i = 0; i < sizeof(malformed_ranges) / sizeof(malformed_ranges[0]); i++) {
        errno = 0;
        if (expect_ioctl_rejection(mirilla_peephole(mirilla_fd, target_id,
                                                    malformed_ranges[i].start_address,
                                                    malformed_ranges[i].end_address, NULL, NULL),
                                   EINVAL, malformed_ranges[i].what) != 0)
            rejection_status = -1;
    }

    mirilla_disengage(mirilla_fd, target_id);
    munmap(region, TEST_REGION_SIZE);
    close(mirilla_fd);
    return rejection_status;
}

/* Disengagement is single-shot; the second one must be refused. */
static int test_double_disengage(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_disengage(mirilla_fd, target_id)), "first disengage "
                                                                            "failed");

    errno = 0;
    int rejection_status = expect_ioctl_rejection(mirilla_disengage(mirilla_fd, target_id), ENOENT,
                                                  "double disengage");

    close(mirilla_fd);
    return rejection_status;
}

/* A stale target id must not open peepholes after disengagement. */
static int test_peephole_after_disengage(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SMALL, MAGIC_VALUE_1);
    ASSERT(region != NULL, "failed to allocate test region");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_disengage(mirilla_fd, target_id)), "disengage failed");

    errno = 0;
    int rejection_status = expect_ioctl_rejection(
        mirilla_peephole(mirilla_fd, target_id, (virtual_address_t)region,
                         (virtual_address_t)region + TEST_REGION_SMALL, NULL, NULL),
        ENOENT, "peephole against a disengaged target");

    munmap(region, TEST_REGION_SMALL);
    close(mirilla_fd);
    return rejection_status;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    print_banner("MIRILLA IOCTL ERROR-PATH SUITE");

    if (harness_fault_initialize() < 0)
        return EXIT_FAILURE;

    RUN_TEST("Reject Foreign Magic", test_bad_magic);
    RUN_TEST("Reject Foreign Category", test_bad_category);
    RUN_TEST("Reject Unknown Map Command", test_bad_map_command);
    RUN_TEST("Reject Unreadable Argument", test_bad_argument_pointer);
    RUN_TEST("Reject Engage of Nonexistent Pid", test_engage_nonexistent_pid);
    RUN_TEST("Reject Peephole Against Bogus Target", test_peephole_bad_target_id);
    RUN_TEST("Reject Malformed Peephole Ranges", test_peephole_malformed_ranges);
    RUN_TEST("Reject Double Disengage", test_double_disengage);
    RUN_TEST("Reject Peephole After Disengage", test_peephole_after_disengage);

    print_summary();

    return suite_status();
}
