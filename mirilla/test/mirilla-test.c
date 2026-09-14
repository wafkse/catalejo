/*
 * Unified userspace integration test for the Mirilla device ABI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-exception.h"

/** One independently isolated userspace integration test group. */
struct mirilla_test_group {
    /** Human-readable group name. */
    const char *name;
    /** Group entry point returning an exit status. */
    int (*run)(void);
};

/** Run the baseline map integration group. */
int mirilla_test_basic(void);
/** Run self-observation integration coverage. */
int mirilla_test_self(void);
/** Run map concurrency integration coverage. */
int mirilla_test_concurrency(void);
/** Run map invariant integration coverage. */
int mirilla_test_invariants(void);
/** Run ioctl validation integration coverage. */
int mirilla_test_ioctl(void);
/** Run address-space layout integration coverage. */
int mirilla_test_layout(void);
/** Ordered userspace integration group list. */
static const struct mirilla_test_group mirilla_test_group_list[] = {
    { "basic", mirilla_test_basic },
    { "self", mirilla_test_self },
    { "concurrency", mirilla_test_concurrency },
    { "invariants", mirilla_test_invariants },
    { "ioctl", mirilla_test_ioctl },
    { "layout", mirilla_test_layout },
    { "exception", mirilla_test_exception },
};

/** Run one group in a child so process-global state cannot leak between groups. */
static int mirilla_test_group_run(const struct mirilla_test_group *test_group)
{
    pid_t child_pid;
    int child_status;

    fflush(NULL);
    child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (child_pid == 0)
        exit(test_group->run());

    if (waitpid(child_pid, &child_status, 0) != child_pid) {
        perror("waitpid");
        return EXIT_FAILURE;
    }

    if (!WIFEXITED(child_status)) {
        if (WIFSIGNALED(child_status))
            fprintf(stderr, "%s terminated by signal %d\n", test_group->name,
                    WTERMSIG(child_status));
        return EXIT_FAILURE;
    }

    return WEXITSTATUS(child_status);
}

/** Dispatch the complete integration suite or one dedicated helper mode. */
int main(int argc, char **argv)
{
    unsigned int failed_count = 0;

    if (argc == 2 && strcmp(argv[1], "--exec-target") == 0)
        for (;;)
            pause();

    if (argc == 2 && strcmp(argv[1], "--atomic") == 0)
        return mirilla_test_exception_atomic();

    if (argc != 1) {
        fprintf(stderr, "usage: %s [--atomic]\n", argv[0]);
        return EXIT_FAILURE;
    }

    for (size_t group_index = 0;
         group_index < sizeof(mirilla_test_group_list) / sizeof(mirilla_test_group_list[0]);
         group_index++) {
        const struct mirilla_test_group *test_group = &mirilla_test_group_list[group_index];

        printf("\n==> %s\n", test_group->name);
        if (mirilla_test_group_run(test_group) != EXIT_SUCCESS)
            failed_count++;
    }

    if (failed_count) {
        fprintf(stderr, "\n%u Mirilla test group(s) failed\n", failed_count);
        return EXIT_FAILURE;
    }

    puts("\nMirilla userspace integration test passed");
    return EXIT_SUCCESS;
}
