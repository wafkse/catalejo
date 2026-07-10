/*
 * Concurrency and Race Test Suite for the Mirilla Module
 *
 * These cases stress the fault path under contention:
 * - Concurrent self-peephole faults from many threads, exercising the
 *   `mmap_lock` reuse in the self path and the per-VMA-lock bounce to
 *   `VM_FAULT_RETRY`.
 * - An observer racing its own unmap of the observed range against
 *   in-flight faults.
 * - A target exiting while the observer keeps reading; the release
 *   notification kills the peephole and further accesses fault.
 *
 * All view accesses ride the `catalejo-fault` protected routines, so a
 * fault the module raises surfaces as `CATALEJO_OUTCOME_ERROR` instead of
 * taking the suite down. The `SIGALRM` watchdog in `main` converts a
 * deadlocked fault path into a suite failure rather than a hung runner.
 */

#include "test-harness.h"

#include <pthread.h>
#include <stdatomic.h>

/* Watchdog budget for the entire suite */
#define SUITE_TIMEOUT_SECONDS 120

/* Concurrent-fault sizing */
#define FAULT_THREAD_LIMIT 8
#define FAULT_ROUNDS 32

/* Race sizing */
#define RACE_READER_ITERATIONS 20000
#define RACE_UNMAP_DELAY_MICROSECONDS 1000

/* Target-exit probing budget */
#define EXIT_PROBE_ATTEMPTS 200
#define EXIT_PROBE_DELAY_MICROSECONDS 10000

typedef struct {
    pthread_barrier_t *barrier;
    int peephole_fd;
    size_t view_size;
    unsigned int expected_base;
    atomic_int *failure_count;
} fault_worker_argument_t;

/*
 * Worker body for the concurrent-fault case: each round maps a private
 * view of the shared peephole, pulls every page through the protected
 * copy, and unmaps again so the next round faults afresh.
 */
static void *fault_worker(void *argument)
{
    fault_worker_argument_t *worker = argument;

    pthread_barrier_wait(worker->barrier);

    for (int round = 0; round < FAULT_ROUNDS; round++) {
        void *view = mmap(NULL, worker->view_size, PROT_READ, MAP_PRIVATE, worker->peephole_fd, 0);
        if (view == MAP_FAILED) {
            atomic_fetch_add(worker->failure_count, 1);
            return NULL;
        }

        if (verify_region_faultable(view, worker->view_size, worker->expected_base) != 0)
            atomic_fetch_add(worker->failure_count, 1);

        munmap(view, worker->view_size);
    }

    return NULL;
}

/*
 * Fault a self-peephole from many threads at once. Every round tears the
 * views down again, so the faults keep hitting the self path (`mmap_lock`
 * reuse, per-VMA-lock retry bounce) concurrently instead of settling into
 * installed pages.
 */
static int test_self_multithread_fault(void)
{
    long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    int thread_count = processor_count < FAULT_THREAD_LIMIT ? (int)processor_count :
                                                              FAULT_THREAD_LIMIT;
    if (thread_count < 2)
        thread_count = 2;

    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
    ASSERT(region != NULL, "failed to allocate test region");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    mirilla_map_peephole_id_t peephole_id = 0;
    int peephole_fd = -1;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(mirilla_fd, target_id, (virtual_address_t)region,
                                                  (virtual_address_t)region + TEST_REGION_SIZE,
                                                  &peephole_id, &peephole_fd)),
           "self-peephole failed");

    print_with_timestamp("[RACE] Faulting the self-peephole from %d threads, %d rounds\n",
                         thread_count, FAULT_ROUNDS);

    pthread_barrier_t barrier;
    ASSERT(pthread_barrier_init(&barrier, NULL, thread_count) == 0, "failed to initialize barrier");

    atomic_int failure_count = 0;

    fault_worker_argument_t worker = {
        .barrier = &barrier,
        .peephole_fd = peephole_fd,
        .view_size = TEST_REGION_SIZE,
        .expected_base = MAGIC_VALUE_1,
        .failure_count = &failure_count,
    };

    pthread_t threads[FAULT_THREAD_LIMIT];
    for (int i = 0; i < thread_count; i++)
        ASSERT(pthread_create(&threads[i], NULL, fault_worker, &worker) == 0, "failed to spawn "
                                                                              "fault worker");

    for (int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&barrier);

    ASSERT(atomic_load(&failure_count) == 0, "concurrent fault workers observed failures");

    close(peephole_fd);
    munmap(region, TEST_REGION_SIZE);
    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);
    return 0;
}

typedef struct {
    void *view;
    size_t view_size;
    atomic_int *stop_flag;
} race_reader_argument_t;

/*
 * Reader body for the unmap race: spin protected probes over the view.
 * Any per-read outcome is legitimate while the observed range is being
 * pulled away; the case only demands that nothing crashes or wedges.
 */
static void *race_reader(void *argument)
{
    race_reader_argument_t *reader = argument;

    for (int i = 0; i < RACE_READER_ITERATIONS && !atomic_load(reader->stop_flag); i++) {
        for (size_t offset = 0; offset < reader->view_size; offset += 4096) {
            unsigned int probe_value = 0;
            (void)faultable_probe_u32((char *)reader->view + offset, &probe_value);
        }
    }

    return NULL;
}

/*
 * Race an unmap of the observed range against in-flight faults. The
 * per-read outcomes are nondeterministic (valid data before the unmap,
 * faults after); the assertion is survival: the readers terminate and the
 * module stays healthy.
 */
static int test_race_unmap_vs_fault(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_2);
    ASSERT(region != NULL, "failed to allocate test region");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    mirilla_map_peephole_id_t peephole_id = 0;
    int peephole_fd = -1;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(mirilla_fd, target_id, (virtual_address_t)region,
                                                  (virtual_address_t)region + TEST_REGION_SIZE,
                                                  &peephole_id, &peephole_fd)),
           "self-peephole failed");

    void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    atomic_int stop_flag = 0;

    race_reader_argument_t reader = {
        .view = view,
        .view_size = TEST_REGION_SIZE,
        .stop_flag = &stop_flag,
    };

    pthread_t reader_thread;
    ASSERT(pthread_create(&reader_thread, NULL, race_reader, &reader) == 0, "failed to spawn race "
                                                                            "reader");

    /* Pull the observed range away mid-read. */
    usleep(RACE_UNMAP_DELAY_MICROSECONDS);
    ASSERT(munmap(region, TEST_REGION_SIZE) == 0, "failed to unmap the observed range");

    print_with_timestamp("[RACE] Observed range unmapped while the reader was faulting\n");

    pthread_join(reader_thread, NULL);

    munmap(view, TEST_REGION_SIZE);
    close(peephole_fd);
    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);

    /* The module must remain healthy after the race. */
    mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "device is unusable after the race");
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed after "
                                                                                    "the race");
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_disengage(mirilla_fd, target_id)), "self-disengage failed "
                                                                            "after the race");
    close(mirilla_fd);
    return 0;
}

/*
 * Target child body: allocate a region, report it, and block until the
 * observer commands the exit.
 */
static int target_child_process(int pipe_in, int pipe_out)
{
    void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_3);
    if (!region)
        return -1;

    if (send_message(pipe_out, MSG_READY, (unsigned long)region, TEST_REGION_SIZE) < 0)
        return -1;

    ipc_message_t msg;
    while (recv_message(pipe_in, &msg) == 0) {
        if (msg.type == MSG_EXIT)
            return 0;
    }

    return -1;
}

/*
 * Keep reading while the target exits. The `mm` teardown emits
 * `MMU_NOTIFY_RELEASE`, the one event that marks the peephole dead, and
 * every access from then on faults (SIGBUS from the module, an error
 * outcome through the protected read).
 */
static int test_target_exit_while_reading(void)
{
    int pipe_to_child[2], pipe_from_child[2];
    ASSERT(pipe(pipe_to_child) == 0, "failed to create pipe");
    ASSERT(pipe(pipe_from_child) == 0, "failed to create pipe");

    pid_t child_pid = fork();
    ASSERT(child_pid >= 0, "failed to fork target child");

    if (child_pid == 0) {
        close(pipe_to_child[PIPE_WRITE]);
        close(pipe_from_child[PIPE_READ]);
        _exit(target_child_process(pipe_to_child[PIPE_READ], pipe_from_child[PIPE_WRITE]) == 0 ?
                  EXIT_SUCCESS :
                  EXIT_FAILURE);
    }

    close(pipe_to_child[PIPE_READ]);
    close(pipe_from_child[PIPE_WRITE]);

    ipc_message_t msg;
    ASSERT(recv_message(pipe_from_child[PIPE_READ], &msg) == 0 && msg.type == MSG_READY, "target "
                                                                                         "child "
                                                                                         "never "
                                                                                         "became "
                                                                                         "ready");

    void *target_region = (void *)msg.data;
    size_t target_size = (size_t)msg.data2;

    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, child_pid, &target_id)), "failed to "
                                                                                     "engage the "
                                                                                     "target "
                                                                                     "child");

    mirilla_map_peephole_id_t peephole_id = 0;
    int peephole_fd = -1;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
               mirilla_fd, target_id, (virtual_address_t)target_region,
               (virtual_address_t)target_region + target_size, &peephole_id, &peephole_fd)),
           "failed to peephole the target child");

    void *view = mmap(NULL, target_size, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
    ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

    /* The peephole is live and mirrors the target before the exit. */
    ASSERT(verify_region_faultable(view, target_size, MAGIC_VALUE_3) == 0, "view does not mirror "
                                                                           "the target region");

    print_with_timestamp("[RACE] Commanding the target (pid %d) to exit mid-observation\n",
                         child_pid);

    /* Keep a reader faulting on the view across the whole teardown. */
    atomic_int stop_flag = 0;

    race_reader_argument_t reader = {
        .view = view,
        .view_size = target_size,
        .stop_flag = &stop_flag,
    };

    pthread_t reader_thread;
    ASSERT(pthread_create(&reader_thread, NULL, race_reader, &reader) == 0, "failed to spawn race "
                                                                            "reader");

    ASSERT(send_message(pipe_to_child[PIPE_WRITE], MSG_EXIT, 0, 0) == 0, "failed to command the "
                                                                         "target exit");

    int child_status;
    ASSERT(waitpid(child_pid, &child_status, 0) == child_pid, "failed to reap the target");
    ASSERT(WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS, "target child "
                                                                                 "exited "
                                                                                 "abnormally");

    atomic_store(&stop_flag, 1);
    pthread_join(reader_thread, NULL);

    /*
	 * The release notification races the reap; keep probing until the
	 * dead peephole faults. Installed view pages are zapped by the exit
	 * unmap invalidations, so a fresh fault against the dead peephole is
	 * guaranteed within the budget.
	 */
    int fault_observed = 0;
    for (int attempt = 0; attempt < EXIT_PROBE_ATTEMPTS; attempt++) {
        unsigned int probe_value = 0;
        if (faultable_probe_u32(view, &probe_value) == CATALEJO_OUTCOME_ERROR) {
            fault_observed = 1;
            break;
        }
        usleep(EXIT_PROBE_DELAY_MICROSECONDS);
    }

    ASSERT(fault_observed, "reads kept succeeding after the target exited");

    print_with_timestamp("[RACE] Dead peephole faulted as expected after the target exit\n");

    munmap(view, target_size);
    close(peephole_fd);
    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);
    close(pipe_to_child[PIPE_WRITE]);
    close(pipe_from_child[PIPE_READ]);
    return 0;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    print_banner("MIRILLA CONCURRENCY SUITE");

    if (harness_fault_initialize() < 0)
        return EXIT_FAILURE;

    /*
	 * NOTE(watchdog): A deadlock in the fault path would otherwise hang
	 * the runner; the default SIGALRM disposition turns it into a loud
	 * failure.
	 */
    alarm(SUITE_TIMEOUT_SECONDS);

    RUN_TEST("Self Peephole - Concurrent Multi-Thread Faults", test_self_multithread_fault);
    RUN_TEST("Race - Unmap vs In-Flight Faults", test_race_unmap_vs_fault);
    RUN_TEST("Target Exit While Reading - Fault Expected", test_target_exit_while_reading);

    print_summary();

    return suite_status();
}
