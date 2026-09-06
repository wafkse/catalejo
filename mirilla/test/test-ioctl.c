/*
 * Ioctl Error-Path Test Suite for the Mirilla Module
 *
 * Negative cases against the command dispatcher and the map command
 * handlers, pinning down the errno of every refusal:
 * - Foreign magic and categories -> `ENOTSUPP`, unknown map commands ->
 *   `ENOTTY`, unreadable argument structures -> `EFAULT`.
 * - Engagement of a nonexistent pid -> `ESRCH`.
 * - Engagement is a capability stored in the device open-file description: it follows the target
 *   PID across exec and remains usable when delegated through `SCM_RIGHTS`.
 * - Peepholes against nonexistent (or already-disengaged) targets ->
 *   `ENOENT`; malformed address ranges -> `EINVAL`.
 * - Disengagement is single-shot: a second disengage -> `ENOENT`.
 */

#include "test-harness.h"

#include <stdbool.h>
#include <sys/resource.h>
#include <sys/socket.h>

/*
 * NOTE(errno): `ENOTSUPP` is kernel-internal and absent from userspace
 * errno.h; the dispatcher leaks it verbatim for foreign magic/categories.
 */
#define MIRILLA_ENOTSUPP 524

/* A pid outside any real pid range (`pid_max` caps at 1 << 22). */
#define NONEXISTENT_PID 0x7fffffff

/* An exception image must describe complete page-aligned VMAs. */
static int test_exception_registration_rejects_misaligned_image(void)
{
    int mirilla_fd = mirilla_open_device();
    struct mirilla_except_image image = harness_exception_runtime.image;

    ASSERT(mirilla_fd >= 0, "failed to open device");

    image.except_table.region_address++;
    errno = 0;
    int rejection_status = expect_ioctl_rejection(
        catalejo_mirilla_except_register(mirilla_fd, &image), EINVAL, "misaligned exception table");

    close(mirilla_fd);
    return rejection_status;
}

/* The declared table range may not extend beyond its immutable VMA. */
static int test_exception_registration_rejects_oversized_table(void)
{
    int mirilla_fd = mirilla_open_device();
    struct mirilla_except_image image = harness_exception_runtime.image;

    ASSERT(mirilla_fd >= 0, "failed to open device");

    image.except_table.region_size += (virtual_size_t)sysconf(_SC_PAGESIZE);
    errno = 0;
    int rejection_status = expect_ioctl_rejection(
        catalejo_mirilla_except_register(mirilla_fd, &image), EINVAL, "oversized exception table");

    close(mirilla_fd);
    return rejection_status;
}

/* One observer address space may publish only one exception image. */
static int test_exception_registration_is_unique_per_mm(void)
{
    int mirilla_fd = mirilla_open_device();
    int duplicate_fd = open(MIRILLA_DEVICE, O_RDWR);
    struct mirilla_except_image image = harness_exception_runtime.image;

    ASSERT(mirilla_fd >= 0, "failed to open registered device session");
    ASSERT(duplicate_fd >= 0, "failed to open duplicate device session");

    errno = 0;
    int rejection_status =
        expect_ioctl_rejection(catalejo_mirilla_except_register(duplicate_fd, &image), EEXIST,
                               "second exception image for one address space");

    close(duplicate_fd);
    close(mirilla_fd);
    return rejection_status;
}

/* A forked child has a distinct mm and receives protection only after its own registration. */
static int test_exception_registration_is_scoped_to_mm(void)
{
    int mirilla_fd = mirilla_open_device();
    struct mirilla_except_image image = harness_exception_runtime.image;
    pid_t child;
    int child_status;

    ASSERT(mirilla_fd >= 0, "failed to open device");

    child = fork();
    ASSERT(child >= 0, "failed to fork unregistered child");
    if (child == 0) {
        uint32_t target_value = 0;
        struct rlimit target_limit = { .rlim_cur = 0, .rlim_max = 0 };

        setrlimit(RLIMIT_CORE, &target_limit);
        catalejo_read_u32(&harness_exception_runtime, (const uint32_t *)0x50, &target_value);
        _exit(2);
    }

    ASSERT(waitpid(child, &child_status, 0) == child, "failed to wait for unregistered child");
    ASSERT(WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGSEGV, "an unregistered child "
                                                                           "fault must retain "
                                                                           "native SIGSEGV "
                                                                           "behavior");

    child = fork();
    ASSERT(child >= 0, "failed to fork registered child");
    if (child == 0) {
        uint32_t target_value = 0;

        if (!MIRILLA_COMMAND_IS_OK(catalejo_mirilla_except_register(mirilla_fd, &image)))
            _exit(3);
        if (catalejo_read_u32(&harness_exception_runtime, (const uint32_t *)0x50, &target_value) !=
            CATALEJO_OUTCOME_ERROR)
            _exit(4);
        _exit(0);
    }

    ASSERT(waitpid(child, &child_status, 0) == child, "failed to wait for registered child");
    ASSERT(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0, "a child registration must "
                                                                      "protect its distinct mm");

    close(mirilla_fd);
    return 0;
}

/* Closing the final descriptor for a session retires its exception registration. */
static int test_exception_registration_follows_session_lifetime(void)
{
    struct mirilla_except_image image = harness_exception_runtime.image;
    pid_t child;
    int child_status;

    child = fork();
    ASSERT(child >= 0, "failed to fork session-lifetime child");
    if (child == 0) {
        uint32_t target_value = 0;
        struct rlimit target_limit = { .rlim_cur = 0, .rlim_max = 0 };
        int mirilla_fd = open(MIRILLA_DEVICE, O_RDWR);

        setrlimit(RLIMIT_CORE, &target_limit);
        if (mirilla_fd < 0)
            _exit(2);
        if (!MIRILLA_COMMAND_IS_OK(catalejo_mirilla_except_register(mirilla_fd, &image)))
            _exit(3);
        close(mirilla_fd);

        catalejo_read_u32(&harness_exception_runtime, (const uint32_t *)0x50, &target_value);
        _exit(4);
    }

    ASSERT(waitpid(child, &child_status, 0) == child, "failed to wait for session-lifetime child");
    ASSERT(WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGSEGV, "closing the final "
                                                                           "session descriptor "
                                                                           "must restore native "
                                                                           "SIGSEGV behavior");

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

/* Send one file descriptor over an AF_UNIX socket. */
static int send_descriptor(int socket_fd, int descriptor)
{
    char payload = 0;
    struct iovec iov = { .iov_base = &payload, .iov_len = sizeof(payload) };
    char control[CMSG_SPACE(sizeof(descriptor))] = { 0 };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };

    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(descriptor));
    memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));

    return sendmsg(socket_fd, &message, 0) == (ssize_t)sizeof(payload) ? 0 : -1;
}

/* Receive one file descriptor sent with SCM_RIGHTS. */
static int receive_descriptor(int socket_fd)
{
    char payload = 0;
    struct iovec iov = { .iov_base = &payload, .iov_len = sizeof(payload) };
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };

    if (recvmsg(socket_fd, &message, 0) != (ssize_t)sizeof(payload))
        return -1;

    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(sizeof(int)))
        return -1;

    int descriptor = -1;
    memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));

    return descriptor;
}

/*
 * Engagement targets the process identity, not one particular image. The same target id must remain
 * usable after that PID crosses exec and acquires a replacement address space.
 */
static int test_engagement_follows_exec(void)
{
    int status = -1;
    int mirilla_fd = -1;
    int command_pipe[2] = { -1, -1 };
    int exec_pipe[2] = { -1, -1 };
    pid_t child = -1;
    bool engaged = false;
    mirilla_map_target_id_t target_id = 0;

    mirilla_fd = mirilla_open_device();
    if (mirilla_fd < 0 || pipe(command_pipe) != 0 || pipe(exec_pipe) != 0)
        goto out;

    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0)
        goto out;

    child = fork();
    if (child < 0)
        goto out;

    if (child == 0) {
        close(command_pipe[1]);
        close(exec_pipe[0]);

        char ready = 1;
        if (write(exec_pipe[1], &ready, sizeof(ready)) != (ssize_t)sizeof(ready))
            _exit(2);

        char command = 0;
        if (read(command_pipe[0], &command, sizeof(command)) != (ssize_t)sizeof(command))
            _exit(3);

        close(command_pipe[0]);
        execl("/proc/self/exe", "test-ioctl", "--exec-target", (char *)NULL);
        _exit(4);
    }

    close(command_pipe[0]);
    command_pipe[0] = -1;
    close(exec_pipe[1]);
    exec_pipe[1] = -1;

    char ready = 0;
    if (read(exec_pipe[0], &ready, sizeof(ready)) != (ssize_t)sizeof(ready))
        goto out;

    if (!MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, child, &target_id)))
        goto out;
    engaged = true;

    char command = 1;
    if (write(command_pipe[1], &command, sizeof(command)) != (ssize_t)sizeof(command))
        goto out;
    close(command_pipe[1]);
    command_pipe[1] = -1;

    /* The CLOEXEC pipe reaches EOF only after the child has crossed exec (or exited). */
    char marker = 0;
    if (read(exec_pipe[0], &marker, sizeof(marker)) != 0)
        goto out;

    int child_status = 0;
    if (waitpid(child, &child_status, WNOHANG) != 0) {
        fprintf(stderr, "exec target exited instead of entering its replacement image\n");
        goto out;
    }

    struct mirilla_outside_list layout_list = {
        .list_address = 0,
        .list_size = 0,
        .element_size = sizeof(struct mirilla_map_address_space_layout),
        .list_attribute = MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };
    struct mirilla_outside_list auxiliary_vector_list = {
        .list_address = 0,
        .list_size = 0,
        .element_size = sizeof(struct mirilla_auxiliary_vector_entry),
        .list_attribute = MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
    };
    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    if (!MIRILLA_COMMAND_IS_OK(mirilla_address_space_layout(
            mirilla_fd, target_id, &layout_list, &auxiliary_vector_list, &metadata, &layout_outcome,
            &auxiliary_vector_outcome))) {
        fprintf(stderr, "engagement did not follow target across exec: %s\n", strerror(errno));
        goto out;
    }

    if (layout_outcome.total_count == 0 || auxiliary_vector_outcome.total_count == 0) {
        fprintf(stderr, "post-exec target returned an empty address-space description\n");
        goto out;
    }

    status = 0;

out:
    if (engaged)
        mirilla_disengage(mirilla_fd, target_id);
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    for (size_t i = 0; i < 2; i++) {
        if (command_pipe[i] >= 0)
            close(command_pipe[i]);
        if (exec_pipe[i] >= 0)
            close(exec_pipe[i]);
    }
    if (mirilla_fd >= 0)
        close(mirilla_fd);

    return status;
}

/*
 * The open file description is the capability. A privileged supervisor may engage a target and
 * delegate that already-authorized session through SCM_RIGHTS to an unprivileged observer.
 */
static int test_scm_rights_delegates_authority(void)
{
    int status = -1;
    int mirilla_fd = -1;
    int target_pipe[2] = { -1, -1 };
    int sockets[2] = { -1, -1 };
    pid_t target = -1;
    pid_t observer = -1;
    bool engaged = false;
    mirilla_map_target_id_t target_id = 0;
    virtual_address_t target_address = 0;

    mirilla_fd = mirilla_open_device();
    if (mirilla_fd < 0 || pipe(target_pipe) != 0)
        goto out;

    target = fork();
    if (target < 0)
        goto out;

    if (target == 0) {
        close(target_pipe[0]);

        void *region = allocate_test_region(TEST_REGION_SMALL, MAGIC_VALUE_1);
        if (!region)
            _exit(2);

        virtual_address_t address = (virtual_address_t)region;
        if (write(target_pipe[1], &address, sizeof(address)) != (ssize_t)sizeof(address))
            _exit(3);

        for (;;)
            pause();
    }

    close(target_pipe[1]);
    target_pipe[1] = -1;

    if (read(target_pipe[0], &target_address, sizeof(target_address)) !=
        (ssize_t)sizeof(target_address))
        goto out;

    if (!MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, target, &target_id)))
        goto out;
    engaged = true;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) != 0)
        goto out;

    observer = fork();
    if (observer < 0)
        goto out;

    if (observer == 0) {
        close(sockets[0]);
        close(mirilla_fd);

        int delegated_fd = receive_descriptor(sockets[1]);
        close(sockets[1]);
        if (delegated_fd < 0)
            _exit(2);

        if (setgid(65534) != 0 || setuid(65534) != 0)
            _exit(3);

        int peephole_fd = -1;
        if (!MIRILLA_COMMAND_IS_OK(mirilla_peephole(delegated_fd, target_id, target_address,
                                                    target_address + TEST_REGION_SMALL, NULL,
                                                    &peephole_fd)))
            _exit(4);

        void *view = mmap(NULL, TEST_REGION_SMALL, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
        if (view == MAP_FAILED)
            _exit(5);

        unsigned int value = 0;
        int access_ok = faultable_probe_u32(view, &value) == CATALEJO_OUTCOME_SUCCESS &&
                        value == MAGIC_VALUE_1;

        munmap(view, TEST_REGION_SMALL);
        close(peephole_fd);
        close(delegated_fd);

        _exit(access_ok ? 0 : 6);
    }

    close(sockets[1]);
    sockets[1] = -1;
    if (send_descriptor(sockets[0], mirilla_fd) != 0)
        goto out;
    close(sockets[0]);
    sockets[0] = -1;

    int observer_status = 0;
    if (waitpid(observer, &observer_status, 0) != observer || !WIFEXITED(observer_status) ||
        WEXITSTATUS(observer_status) != 0) {
        fprintf(stderr, "SCM_RIGHTS-delegated authorized session could not access its target\n");
        goto out;
    }
    observer = -1;

    status = 0;

out:
    if (observer > 0) {
        kill(observer, SIGKILL);
        waitpid(observer, NULL, 0);
    }
    if (engaged)
        mirilla_disengage(mirilla_fd, target_id);
    if (target > 0) {
        kill(target, SIGKILL);
        waitpid(target, NULL, 0);
    }
    for (size_t i = 0; i < 2; i++) {
        if (target_pipe[i] >= 0)
            close(target_pipe[i]);
        if (sockets[i] >= 0)
            close(sockets[i]);
    }
    if (mirilla_fd >= 0)
        close(mirilla_fd);

    return status;
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

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--exec-target") == 0)
        for (;;)
            pause();

    print_banner("MIRILLA IOCTL ERROR-PATH SUITE");

    if (harness_fault_initialize() < 0)
        return EXIT_FAILURE;

    RUN_TEST("Reject Foreign Magic", test_bad_magic);
    RUN_TEST("Reject Foreign Category", test_bad_category);
    RUN_TEST("Reject Misaligned Exception Image",
             test_exception_registration_rejects_misaligned_image);
    RUN_TEST("Reject Oversized Exception Table",
             test_exception_registration_rejects_oversized_table);
    RUN_TEST("Exception Registration Is Unique Per MM",
             test_exception_registration_is_unique_per_mm);
    RUN_TEST("Exception Registration Is Scoped To MM", test_exception_registration_is_scoped_to_mm);
    RUN_TEST("Exception Registration Follows Session Lifetime",
             test_exception_registration_follows_session_lifetime);
    RUN_TEST("Reject Unknown Map Command", test_bad_map_command);
    RUN_TEST("Reject Unreadable Argument", test_bad_argument_pointer);
    RUN_TEST("Reject Engage of Nonexistent Pid", test_engage_nonexistent_pid);
    RUN_TEST("Reject Peephole Against Bogus Target", test_peephole_bad_target_id);
    RUN_TEST("Reject Malformed Peephole Ranges", test_peephole_malformed_ranges);
    RUN_TEST("Engagement Follows Exec", test_engagement_follows_exec);
    RUN_TEST("SCM_RIGHTS Delegates Authority", test_scm_rights_delegates_authority);
    RUN_TEST("Reject Double Disengage", test_double_disengage);
    RUN_TEST("Reject Peephole After Disengage", test_peephole_after_disengage);

    print_summary();

    return suite_status();
}
