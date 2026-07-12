/*
 * Shared harness for the mirilla test suites.
 *
 * Every suite is a standalone binary that includes this header first,
 * exercises its scenarios through the helpers below, and exits nonzero
 * when any case failed so a runner can aggregate results.
 *
 * Faulting accesses ride the `catalejo-fault` subsystem end to end, as reads
 * against peephole views go through the fault-protected routines, so an
 * access the module refuses (SIGSEGV on an unresolvable range, SIGBUS on a
 * dead peephole) surfaces as `CATALEJO_OUTCOME_ERROR` instead of taking the
 * suite down. This exercises the same recovery path the userspace crates
 * rely on.
 */

#ifndef _MIRILLA_TEST_HARNESS_H_
#define _MIRILLA_TEST_HARNESS_H_

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdarg.h>

#include "../include/mirilla-command.h"
#include "../include/mirilla-map.h"

#include "catalejo-fault.h"
#include "catalejo-mirilla.h"

/* Configuration */
#define MIRILLA_DEVICE "/dev/mirilla"
#define TEST_REGION_SIZE (4096 * 4) /* 16KB */
#define TEST_REGION_SMALL (4096) /* 4KB */
#define MAGIC_VALUE_1 0xDEADBEEF
#define MAGIC_VALUE_2 0xCAFEBABE
#define MAGIC_VALUE_3 0xFEEDFACE
#define MAGIC_VALUE_4 0xBAADF00D
#define MAGIC_VALUE_5 0x13371337

/* IPC pipe indices */
#define PIPE_READ 0
#define PIPE_WRITE 1

/* Color codes for output */
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"

/* Test result tracking */
typedef struct {
    int total;
    int passed;
    int failed;
} test_results_t;

static test_results_t results = { 0, 0, 0 };

/* Helper macros */
#define TEST_START(name)                                      \
    do {                                                      \
        printf(COLOR_CYAN "[ TEST ] %s\n" COLOR_RESET, name); \
        results.total++;                                      \
    } while (0)

#define TEST_PASS(name)                                        \
    do {                                                       \
        printf(COLOR_GREEN "[ PASS ] %s\n" COLOR_RESET, name); \
        results.passed++;                                      \
    } while (0)

#define TEST_FAIL(name, reason)                                          \
    do {                                                                 \
        printf(COLOR_RED "[ FAIL ] %s: %s\n" COLOR_RESET, name, reason); \
        results.failed++;                                                \
    } while (0)

#define ASSERT(condition, msg)                                                             \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, COLOR_RED "ASSERTION FAILED: %s (line %d)\n" COLOR_RESET, msg, \
                    __LINE__);                                                             \
            return -1;                                                                     \
        }                                                                                  \
    } while (0)

/* Run a case function returning 0 on success under the result tracking */
#define RUN_TEST(name, fn)                       \
    do {                                         \
        TEST_START(name);                        \
        if (fn() == 0)                           \
            TEST_PASS(name);                     \
        else                                     \
            TEST_FAIL(name, "see output above"); \
    } while (0)

/* IPC message types */
typedef enum {
    MSG_READY = 1,
    MSG_WRITE_DATA,
    MSG_VERIFY,
    MSG_MODIFY,
    MSG_ALLOCATE_REGION,
    MSG_UNMAP_REGION,
    MSG_REMAP_REGION,
    MSG_EXIT,
    MSG_ACK,
    MSG_ERROR
} msg_type_t;

typedef struct {
    msg_type_t type;
    unsigned long data;
    unsigned long data2; /* For passing additional info */
} ipc_message_t;

/* Send message through pipe */
static inline int send_message(int fd, msg_type_t type, unsigned long data, unsigned long data2)
{
    ipc_message_t msg = { type, data, data2 };
    ssize_t written = write(fd, &msg, sizeof(msg));
    if (written != sizeof(msg)) {
        perror("send_message");
        return -1;
    }
    return 0;
}

/* Receive message from pipe */
static inline int recv_message(int fd, ipc_message_t *msg)
{
    ssize_t bytes_read = read(fd, msg, sizeof(*msg));
    if (bytes_read != sizeof(*msg)) {
        perror("recv_message");
        return -1;
    }
    return 0;
}

/* Get boot timestamp for correlation with dmesg */
static inline double get_boot_time(void)
{
    struct sysinfo info;
    struct timespec ts;

    if (sysinfo(&info) != 0 || clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Print with timestamp */
static inline void print_with_timestamp(const char *format, ...)
{
    double boot_time = get_boot_time();
    printf("[%10.6f] ", boot_time);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/* Open the mirilla device, or explain why the suite cannot run */
static inline int mirilla_open_device(void)
{
    int mirilla_fd = open(MIRILLA_DEVICE, O_RDWR);
    if (mirilla_fd < 0) {
        perror("Failed to open mirilla device");
        fprintf(stderr, "Make sure the mirilla module is loaded (sudo insmod "
                        "mirilla.ko)\n");
    }
    return mirilla_fd;
}

/*
 * Thin ioctl wrappers over the map commands.
 *
 * Each returns the raw ioctl result (`MIRILLA_COMMAND_OK` on success, -1 with
 * `errno` set on failure) so negative tests can assert the exact errno.
 */
static inline long mirilla_engage(int mirilla_fd, pid_t process_id,
                                  mirilla_map_target_id_t *target_id)
{
    union mirilla_map_engage_io engage_io = { 0 };
    engage_io.argument.process_id = process_id;

    unsigned int cmd =
        MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_ENGAGE);
    long ret = ioctl(mirilla_fd, cmd, &engage_io);

    if (MIRILLA_COMMAND_IS_OK(ret) && target_id)
        *target_id = engage_io.result.target_id;

    return ret;
}

static inline long mirilla_disengage(int mirilla_fd, mirilla_map_target_id_t target_id)
{
    union mirilla_map_disengage_io disengage_io = { 0 };
    disengage_io.argument.target_id = target_id;

    unsigned int cmd =
        MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_DISENGAGE);
    return ioctl(mirilla_fd, cmd, &disengage_io);
}

static inline long mirilla_peephole(int mirilla_fd, mirilla_map_target_id_t target_id,
                                    virtual_address_t start_address, virtual_address_t end_address,
                                    mirilla_map_peephole_id_t *peephole_id, int *peephole_fd)
{
    union mirilla_map_peephole_io peephole_io = { 0 };
    peephole_io.argument.target_id = target_id;
    peephole_io.argument.start_address = start_address;
    peephole_io.argument.end_address = end_address;

    unsigned int cmd =
        MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_PEEPHOLE);
    long ret = ioctl(mirilla_fd, cmd, &peephole_io);

    if (MIRILLA_COMMAND_IS_OK(ret)) {
        if (peephole_id)
            *peephole_id = peephole_io.result.id;
        if (peephole_fd)
            *peephole_fd = peephole_io.result.fd;
    }

    return ret;
}

/*
 * Retrieve the address space layout, auxiliary vector, and metadata for an
 * engaged target.
 *
 * This delegates to the `catalejo-sys` C wrapper, which recovers `-errno`
 * from the raw ioctl so the caller receives the full kernel status directly.
 */
static inline long
mirilla_address_space_layout(int mirilla_fd, mirilla_map_target_id_t target_id,
                             struct mirilla_outside_list *layout_list,
                             struct mirilla_outside_list *auxiliary_vector_list,
                             struct mirilla_map_address_space_metadata *metadata,
                             struct mirilla_outside_list_outcome *layout_outcome,
                             struct mirilla_outside_list_outcome *auxiliary_vector_outcome)
{
    return catalejo_mirilla_address_space_layout(mirilla_fd, target_id, layout_list,
                                                 auxiliary_vector_list, metadata, layout_outcome,
                                                 auxiliary_vector_outcome);
}

/* A target id no engagement ever produced. */
#define BOGUS_TARGET_ID 0xdeadbeef

/*
 * Expect an ioctl result to be a refusal with the given errno.
 */
static inline int expect_ioctl_rejection(long ioctl_code, int expected_errno, const char *what)
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

/* Allocate and initialize a test memory region */
static inline void *allocate_test_region(size_t size, unsigned int initial_value)
{
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    /* Initialize with pattern */
    unsigned int *data = (unsigned int *)addr;
    for (size_t i = 0; i < size / sizeof(unsigned int); i++) {
        data[i] = initial_value + i;
    }

    return addr;
}

/* Verify memory region contents */
static inline int verify_region(void *addr, size_t size, unsigned int expected_base)
{
    unsigned int *data = (unsigned int *)addr;
    for (size_t i = 0; i < size / sizeof(unsigned int); i++) {
        if (data[i] != expected_base + i) {
            fprintf(stderr, "Verification failed at offset %zu: expected 0x%x, got 0x%x\n",
                    i * sizeof(unsigned int), expected_base + (unsigned int)i, data[i]);
            return -1;
        }
    }
    return 0;
}

/*
 * Set up the `catalejo-fault` environment for the suite.
 *
 * NOTE(invariant): This must run before any protected read is performed.
 * The chaining signal handler it installs is process-wide, so forked
 * children and spawned threads are covered as well.
 */
static inline int harness_fault_initialize(void)
{
    if (catalejo_fault_initialize() != CATALEJO_OUTCOME_SUCCESS) {
        fprintf(stderr, COLOR_RED "Failed to initialize the catalejo-fault "
                                  "subsystem\n" COLOR_RESET);
        return -1;
    }
    return 0;
}

/*
 * Probe a single word through the fault-protected read routine.
 *
 * Returns `CATALEJO_OUTCOME_SUCCESS` and stores the value, or
 * `CATALEJO_OUTCOME_ERROR` when the access faulted (the module answered the
 * page fault with SIGSEGV or SIGBUS and the chaining handler recovered).
 */
static inline catalejo_faultable_outcome_t faultable_probe_u32(const void *addr,
                                                               unsigned int *value)
{
    uint32_t probe_value = 0;

    catalejo_faultable_outcome_t outcome = catalejo_read_u32((const uint32_t *)addr, &probe_value);

    if (outcome == CATALEJO_OUTCOME_SUCCESS && value)
        *value = probe_value;

    return outcome;
}

/*
 * Read a whole region through the fault-protected bulk copy.
 *
 * Returns 0 when every byte was read, -1 when the copy faulted partway.
 */
static inline int faultable_read_region(void *destination, const void *source, size_t size)
{
    catalejo_faultable_copy_outcome_t copy_outcome =
        catalejo_copy((uint8_t *)destination, (const uint8_t *)source, size);

    return copy_outcome.outcome_status == CATALEJO_OUTCOME_SUCCESS ? 0 : -1;
}

/*
 * Verify region contents end to end: the bytes are pulled through the
 * fault-protected copy before comparison, so a faulting view reports as a
 * verification failure rather than a crash.
 */
static inline int verify_region_faultable(const void *addr, size_t size, unsigned int expected_base)
{
    int verify_status = -1;

    void *snapshot = malloc(size);
    if (!snapshot) {
        perror("malloc");
        return -1;
    }

    if (faultable_read_region(snapshot, addr, size) == 0)
        verify_status = verify_region(snapshot, size, expected_base);
    else
        fprintf(stderr, "Verification failed: protected copy faulted at %p\n", addr);

    free(snapshot);
    return verify_status;
}

/* Print test summary */
static inline void print_summary(void)
{
    printf("\n");
    printf(COLOR_CYAN "========================================\n");
    printf("         TEST SUMMARY\n");
    printf("========================================\n" COLOR_RESET);
    printf("Total tests:  %d\n", results.total);
    printf(COLOR_GREEN "Passed:       %d\n" COLOR_RESET, results.passed);
    printf(COLOR_RED "Failed:       %d\n" COLOR_RESET, results.failed);
    printf(COLOR_CYAN "========================================\n" COLOR_RESET);

    if (results.failed == 0 && results.total > 0) {
        printf(COLOR_GREEN "\n✓ All tests passed!\n" COLOR_RESET);
    } else if (results.failed > 0) {
        printf(COLOR_RED "\n✗ Some tests failed.\n" COLOR_RESET);
    }
}

/* Print a suite banner */
static inline void print_banner(const char *title)
{
    printf(COLOR_MAGENTA);
    printf("╔════════════════════════════════════════╗\n");
    printf("║   %-37s║\n", title);
    printf("╚════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
    printf("\n");
}

/* Suite exit status from the accumulated results */
static inline int suite_status(void)
{
    return (results.failed == 0 && results.total > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* _MIRILLA_TEST_HARNESS_H_ */
