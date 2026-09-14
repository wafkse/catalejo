#define _GNU_SOURCE

#include "test-harness.h"
#include "test-exception.h"

#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>

/** Create one exception context fixture with no mapped slab. */
static struct exception_fixture exception_fixture_create(void)
{
    struct exception_fixture fixture = {
        .device_fd = open(MIRILLA_DEVICE, O_RDWR | O_CLOEXEC),
        .exception_fd = -1,
    };

    assert(fixture.device_fd >= 0);
    assert(catalejo_mirilla_except_create(fixture.device_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
                                          &fixture.exception_id, &fixture.exception_fd) == 0);
    assert(fixture.exception_id != MIRILLA_ID_NONE && fixture.exception_fd >= 0);

    return fixture;
}

/** Release every descriptor and mapping owned by an exception fixture. */
static void exception_fixture_destroy(struct exception_fixture *fixture)
{
    if (fixture->record_list) {
        assert(catalejo_except_slab_unmap(fixture->record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE) ==
               0);
        fixture->record_list = NULL;
    }
    if (fixture->exception_fd >= 0) {
        close(fixture->exception_fd);
        fixture->exception_fd = -1;
    }
    if (fixture->device_fd >= 0) {
        close(fixture->device_fd);
        fixture->device_fd = -1;
    }
}

/** Construct one valid retry record over a userspace interval. */
static struct mirilla_except_record exception_record_create(const void *base_address,
                                                            uint32_t region_size)
{
    return (struct mirilla_except_record){
        .boundary = {
            .base_address = (virtual_address_t)(uintptr_t)base_address,
            .region_size = region_size,
        },
        .predicate = {
            .except_mask = MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT),
        },
        .action = {
            .tag = MIRILLA_EXCEPT_ACTION_RETRY,
        },
    };
}

/** Verify CREATE validation, one-context admission, mapping shape, and fd lifetime. */
static void creation_and_mapping_test(void)
{
    struct exception_fixture fixture;
    int device_fd = open(MIRILLA_DEVICE, O_RDWR | O_CLOEXEC);
    mirilla_except_id_t exception_id;
    int exception_fd;

    assert(device_fd >= 0);
    assert(catalejo_mirilla_except_create(device_fd, 0, &exception_id, &exception_fd) == -EINVAL);
    assert(catalejo_mirilla_except_create(device_fd, 4096, &exception_id, &exception_fd) ==
           -EINVAL);
    assert(catalejo_mirilla_except_create(device_fd, MIRILLA_EXCEPT_SLAB_SIZE_LIMIT + 4096,
                                          &exception_id, &exception_fd) == -EINVAL);

    fixture = exception_fixture_create();
    assert(catalejo_mirilla_except_create(device_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
                                          &exception_id, &exception_fd) == -EEXIST);

    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE / 2, PROT_READ | PROT_WRITE, MAP_SHARED,
                fixture.exception_fd, 0) == MAP_FAILED &&
           errno == EINVAL);
    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                fixture.exception_fd, 0) == MAP_FAILED &&
           errno == EINVAL);
    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ, MAP_SHARED, fixture.exception_fd,
                0) == MAP_FAILED &&
           errno == EINVAL);
    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_SHARED, fixture.exception_fd, 0) == MAP_FAILED &&
           errno == EPERM);
    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                fixture.exception_fd, 4096) == MAP_FAILED &&
           errno == EINVAL);

    assert(catalejo_except_slab_map(fixture.exception_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
                                    &fixture.record_list) == 0);
    assert(fixture.record_list[0].boundary.base_address == 0);

    close(fixture.device_fd);
    fixture.device_fd = -1;
    assert(catalejo_except_slab_publish(fixture.record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE) ==
           0);
    assert(catalejo_except_slab_edit(fixture.record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE) == 0);

    exception_fixture_destroy(&fixture);
    close(device_fd);
}

/** Verify publication rollback and forbidden VMA mutations. */
static void publication_and_vma_test(void)
{
    struct exception_fixture fixture = exception_fixture_create();
    size_t slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE;

    assert(catalejo_except_slab_map(fixture.exception_fd, slab_size, &fixture.record_list) == 0);

    fixture.record_list[0].boundary.base_address = 1;
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == -EINVAL);
    fixture.record_list[0] = (struct mirilla_except_record){ 0 };
    fixture.record_list[1].boundary.base_address = 1;
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == -EINVAL);
    fixture.record_list[1] = (struct mirilla_except_record){ 0 };

    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);

    struct mirilla_except_record forced_record = exception_record_create(fixture.record_list, 8);
    struct iovec local_iovec = {
        .iov_base = &forced_record,
        .iov_len = sizeof(forced_record),
    };
    struct iovec remote_iovec = {
        .iov_base = fixture.record_list,
        .iov_len = sizeof(forced_record),
    };

    errno = 0;
    assert(process_vm_writev(getpid(), &local_iovec, 1, &remote_iovec, 1, 0) == -1 &&
           errno == EFAULT);
    assert(fixture.record_list[0].boundary.base_address == 0);

#if defined(__x86_64__) || defined(__i386__)
    int protection_key = pkey_alloc(0, 0);
    if (protection_key >= 0) {
        errno = 0;
        assert(pkey_mprotect(fixture.record_list, slab_size, PROT_READ, protection_key) == -1 &&
               errno == EPERM);
        assert(pkey_free(protection_key) == 0);
    }
#endif

    errno = 0;
    assert(mprotect(fixture.record_list, slab_size, PROT_NONE) == -1 && errno == EPERM);
    errno = 0;
    assert(mprotect(fixture.record_list, slab_size, PROT_READ | PROT_EXEC) == -1 &&
           errno == EACCES);
    errno = 0;
    assert(mprotect((char *)fixture.record_list + 4096, 4096, PROT_READ | PROT_WRITE) == -1 &&
           errno == EPERM);

    assert(catalejo_except_slab_edit(fixture.record_list, slab_size) == 0);
    assert(catalejo_except_slab_edit(fixture.record_list, slab_size) == 0);

    for (unsigned int toggle_index = 0; toggle_index < 64; toggle_index++) {
        assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);
        assert(catalejo_except_slab_edit(fixture.record_list, slab_size) == 0);
    }

    errno = 0;
    assert(munmap((char *)fixture.record_list + 4096, 4096) == -1 && errno == EPERM);
    errno = 0;
    assert(mremap(fixture.record_list, slab_size, slab_size * 2, MREMAP_MAYMOVE) == MAP_FAILED &&
           errno == EFAULT);

    close(fixture.exception_fd);
    fixture.exception_fd = -1;
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);
    assert(catalejo_except_slab_edit(fixture.record_list, slab_size) == 0);
    exception_fixture_destroy(&fixture);
}

/** Verify the per-context hard slab limit and slot reuse after unmapping. */
static void hard_limit_test(void)
{
    struct exception_fixture fixture = exception_fixture_create();
    void *slab_list[MIRILLA_EXCEPT_SLAB_LIMIT] = { 0 };
    size_t slab_index;

    for (slab_index = 0; slab_index < MIRILLA_EXCEPT_SLAB_LIMIT; slab_index++) {
        slab_list[slab_index] = mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fixture.exception_fd, 0);
        assert(slab_list[slab_index] != MAP_FAILED);
    }

    errno = 0;
    assert(mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                fixture.exception_fd, 0) == MAP_FAILED &&
           errno == ENOSPC);
    assert(munmap(slab_list[0], MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE) == 0);
    slab_list[0] = mmap(NULL, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fixture.exception_fd, 0);
    assert(slab_list[0] != MAP_FAILED);

    for (slab_index = 0; slab_index < MIRILLA_EXCEPT_SLAB_LIMIT; slab_index++)
        assert(munmap(slab_list[slab_index], MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE) == 0);

    exception_fixture_destroy(&fixture);
}

/** Verify conflict rollback and replacement across independently mapped slabs. */
static void table_replacement_test(void)
{
    struct exception_fixture fixture = exception_fixture_create();
    struct mirilla_except_record *second_record_list = NULL;
    size_t slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE;
    void *target_page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(target_page != MAP_FAILED);
    assert(catalejo_except_slab_map(fixture.exception_fd, slab_size, &fixture.record_list) == 0);
    assert(catalejo_except_slab_map(fixture.exception_fd, slab_size, &second_record_list) == 0);

    fixture.record_list[0] = exception_record_create(target_page, 64);
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);

    second_record_list[0] = exception_record_create((char *)target_page + 32, 64);
    assert(catalejo_except_slab_publish(second_record_list, slab_size) == -EEXIST);

    second_record_list[0] = exception_record_create((char *)target_page + 128, 64);
    assert(catalejo_except_slab_publish(second_record_list, slab_size) == 0);

    assert(catalejo_except_slab_edit(fixture.record_list, slab_size) == 0);
    fixture.record_list[0] = exception_record_create((char *)target_page + 256, 64);
    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);

    assert(catalejo_except_slab_unmap(second_record_list, slab_size) == 0);
    assert(munmap(target_page, 4096) == 0);
    exception_fixture_destroy(&fixture);
}

/** Verify a sealed published slab remains active and cannot return to editing. */
static void mseal_interaction_test(void)
{
#if defined(SYS_mseal)
    pid_t child_pid = fork();
    int child_status;

    assert(child_pid >= 0);
    if (child_pid == 0) {
        struct exception_fixture child_fixture = exception_fixture_create();
        size_t slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE;
        long seal_status;

        if (catalejo_except_slab_map(child_fixture.exception_fd, slab_size,
                                     &child_fixture.record_list))
            _exit(2);
        if (catalejo_except_slab_publish(child_fixture.record_list, slab_size))
            _exit(3);

        errno = 0;
        seal_status = syscall(SYS_mseal, child_fixture.record_list, slab_size, 0);
        if (seal_status == 0) {
            if (catalejo_except_slab_edit(child_fixture.record_list, slab_size) != -EPERM)
                _exit(4);
        } else if (catalejo_except_slab_edit(child_fixture.record_list, slab_size)) {
            _exit(5);
        }

        _exit(0);
    }

    assert(waitpid(child_pid, &child_status, 0) == child_pid && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0);
#endif
}

/** Thread-local recovery point for the expected post-publication write fault. */
static _Thread_local sigjmp_buf freeze_jump;

/** Whether the current writer thread may recover a protection fault. */
static _Thread_local volatile sig_atomic_t freeze_armed;

/** Shared state for one PTE-freeze writer. */
struct freeze_worker {
    /** Atomically written boundary address inside the editable slab. */
    _Atomic virtual_address_t *base_address;
    /** First valid userspace boundary value. */
    virtual_address_t first_address;
    /** Second valid userspace boundary value. */
    virtual_address_t second_address;
    /** Publication barrier indicating that the writer loop has started. */
    atomic_bool writer_started;
    /** Number of stores completed by the writer. */
    atomic_ulong store_count;
};

/** Recover the writer thread from its expected read-only slab fault. */
static void freeze_signal_handle(int signal_number)
{
    if (freeze_armed)
        siglongjmp(freeze_jump, 1);

    _exit(128 + signal_number);
}

/** Continuously write valid record addresses until publication removes write access. */
static void *freeze_worker_run(void *worker_pointer)
{
    struct freeze_worker *target_worker = worker_pointer;
    virtual_address_t next_address = target_worker->first_address;

    if (sigsetjmp(freeze_jump, 1)) {
        freeze_armed = 0;

        return NULL;
    }

    freeze_armed = 1;
    atomic_store_explicit(&target_worker->writer_started, true, memory_order_release);
    for (;;) {
        atomic_store_explicit(target_worker->base_address, next_address, memory_order_relaxed);
        atomic_fetch_add_explicit(&target_worker->store_count, 1, memory_order_relaxed);
        next_address = next_address == target_worker->first_address ?
                           target_worker->second_address :
                           target_worker->first_address;
    }
}

/** Verify PTE invalidation prevents any ordinary store after publication completes. */
static void pte_freeze_stress_test(void)
{
    struct exception_fixture fixture = exception_fixture_create();
    struct sigaction fault_action = {
        .sa_handler = freeze_signal_handle,
    };
    struct sigaction prior_action;
    struct freeze_worker target_worker = { 0 };
    pthread_t writer_thread;
    virtual_address_t published_address;
    virtual_address_t final_address;
    size_t slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE;
    void *target_page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(target_page != MAP_FAILED);
    assert(catalejo_except_slab_map(fixture.exception_fd, slab_size, &fixture.record_list) == 0);
    fixture.record_list[0] = exception_record_create(target_page, 64);

    target_worker.base_address =
        (_Atomic virtual_address_t *)&fixture.record_list[0].boundary.base_address;
    target_worker.first_address = (virtual_address_t)(uintptr_t)target_page;
    target_worker.second_address = (virtual_address_t)(uintptr_t)((char *)target_page + 128);
    atomic_init(&target_worker.writer_started, false);
    atomic_init(&target_worker.store_count, 0);

    sigemptyset(&fault_action.sa_mask);
    assert(sigaction(SIGSEGV, &fault_action, &prior_action) == 0);
    assert(pthread_create(&writer_thread, NULL, freeze_worker_run, &target_worker) == 0);

    while (!atomic_load_explicit(&target_worker.writer_started, memory_order_acquire))
        sched_yield();
    while (!atomic_load_explicit(&target_worker.store_count, memory_order_acquire))
        sched_yield();

    assert(catalejo_except_slab_publish(fixture.record_list, slab_size) == 0);
    published_address = atomic_load_explicit(target_worker.base_address, memory_order_acquire);
    assert(pthread_join(writer_thread, NULL) == 0);
    final_address = atomic_load_explicit(target_worker.base_address, memory_order_acquire);

    assert(final_address == published_address);
    assert(sigaction(SIGSEGV, &prior_action, NULL) == 0);
    assert(munmap(target_page, 4096) == 0);
    exception_fixture_destroy(&fixture);
}

/** Send one descriptor over a connected Unix-domain socket. */
static int exception_descriptor_send(int socket_fd, int transfer_fd)
{
    char payload_byte = 0;
    struct iovec payload_iovec = {
        .iov_base = &payload_byte,
        .iov_len = sizeof(payload_byte),
    };
    char control_buffer[CMSG_SPACE(sizeof(transfer_fd))] = { 0 };
    struct msghdr message_header = {
        .msg_iov = &payload_iovec,
        .msg_iovlen = 1,
        .msg_control = control_buffer,
        .msg_controllen = sizeof(control_buffer),
    };
    struct cmsghdr *control_header = CMSG_FIRSTHDR(&message_header);

    control_header->cmsg_level = SOL_SOCKET;
    control_header->cmsg_type = SCM_RIGHTS;
    control_header->cmsg_len = CMSG_LEN(sizeof(transfer_fd));
    memcpy(CMSG_DATA(control_header), &transfer_fd, sizeof(transfer_fd));

    return sendmsg(socket_fd, &message_header, 0) == 1 ? 0 : -errno;
}

/** Receive one descriptor from a connected Unix-domain socket. */
static int exception_descriptor_receive(int socket_fd)
{
    char payload_byte;
    struct iovec payload_iovec = {
        .iov_base = &payload_byte,
        .iov_len = sizeof(payload_byte),
    };
    char control_buffer[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr message_header = {
        .msg_iov = &payload_iovec,
        .msg_iovlen = 1,
        .msg_control = control_buffer,
        .msg_controllen = sizeof(control_buffer),
    };
    struct cmsghdr *control_header;
    int received_fd = -1;
    ssize_t byte_count;
    bool socket_level;
    bool rights_message;
    bool complete_length;
    bool complete_control;

    byte_count = recvmsg(socket_fd, &message_header, 0);
    if (byte_count != 1)
        return byte_count < 0 ? -errno : -EBADMSG;

    control_header = CMSG_FIRSTHDR(&message_header);
    if (!control_header)
        return -EBADMSG;

    socket_level = control_header->cmsg_level == SOL_SOCKET;
    rights_message = control_header->cmsg_type == SCM_RIGHTS;
    complete_length = control_header->cmsg_len == CMSG_LEN(sizeof(received_fd));
    complete_control = !(message_header.msg_flags & MSG_CTRUNC);

    if (!socket_level || !rights_message || !complete_length || !complete_control)
        return -EBADMSG;

    memcpy(&received_fd, CMSG_DATA(control_header), sizeof(received_fd));

    return received_fd;
}

/** Verify inherited descriptors become stale and the child singleton rebuilds. */
static void fork_and_backend_test(void)
{
    struct exception_fixture fixture = exception_fixture_create();
    const struct catalejo_fault_backend *fault_backend = NULL;
    pid_t child_pid;
    int child_status;
    int device_fd;
    int socket_list[2];

    assert(catalejo_except_slab_map(fixture.exception_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
                                    &fixture.record_list) == 0);
    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        struct mirilla_except_record *record_list = NULL;

        if (catalejo_except_slab_map(fixture.exception_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
                                     &record_list) != -ESTALE)
            _exit(2);
        _exit(0);
    }
    assert(waitpid(child_pid, &child_status, 0) == child_pid && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_list) == 0);
    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        struct mirilla_except_record *record_list = NULL;
        int received_fd;

        close(socket_list[0]);
        close(fixture.exception_fd);
        received_fd = exception_descriptor_receive(socket_list[1]);
        if (received_fd < 0)
            _exit(6);
        if (catalejo_except_slab_map(received_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, &record_list) !=
            -ESTALE)
            _exit(7);
        close(received_fd);
        _exit(0);
    }

    close(socket_list[1]);
    assert(exception_descriptor_send(socket_list[0], fixture.exception_fd) == 0);
    close(socket_list[0]);
    assert(waitpid(child_pid, &child_status, 0) == child_pid && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0);
    exception_fixture_destroy(&fixture);

    device_fd = open(MIRILLA_DEVICE, O_RDWR | O_CLOEXEC);
    assert(device_fd >= 0);
    assert(catalejo_fault_backend_initialize(device_fd, &fault_backend) == 0 && fault_backend);

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        const struct catalejo_fault_backend *child_backend = NULL;
        uint32_t target_value = 0;

        if (catalejo_fault_backend_retrieve(&child_backend) != -ESTALE || child_backend)
            _exit(3);
        if (catalejo_fault_backend_initialize(device_fd, &child_backend) || !child_backend)
            _exit(4);
        if (catalejo_read_u32((const uint32_t *)0x50, &target_value) != CATALEJO_OUTCOME_ERROR)
            _exit(5);
        _exit(0);
    }
    assert(waitpid(child_pid, &child_status, 0) == child_pid && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0);
    close(device_fd);
}

/** Per-thread result for concurrent backend initialization. */
struct backend_worker {
    /** Shared Mirilla device descriptor. */
    int device_fd;
    /** Backend returned to this worker. */
    const struct catalejo_fault_backend *fault_backend;
    /** Negative errno status returned to this worker. */
    int init_status;
};

/** Initialize the process backend from one worker thread. */
static void *backend_worker_run(void *worker_pointer)
{
    struct backend_worker *target_worker = worker_pointer;

    target_worker->init_status =
        catalejo_fault_backend_initialize(target_worker->device_fd, &target_worker->fault_backend);

    return NULL;
}

/** Verify concurrent initialization publishes exactly one process backend handle. */
static void concurrent_backend_test(void)
{
    enum { WORKER_COUNT = 8 };
    struct backend_worker worker_list[WORKER_COUNT] = { 0 };
    pthread_t thread_list[WORKER_COUNT];
    int device_fd = open(MIRILLA_DEVICE, O_RDWR | O_CLOEXEC);

    assert(device_fd >= 0);
    for (unsigned int worker_index = 0; worker_index < WORKER_COUNT; worker_index++) {
        worker_list[worker_index].device_fd = device_fd;
        assert(pthread_create(&thread_list[worker_index], NULL, backend_worker_run,
                              &worker_list[worker_index]) == 0);
    }

    for (unsigned int worker_index = 0; worker_index < WORKER_COUNT; worker_index++) {
        assert(pthread_join(thread_list[worker_index], NULL) == 0);
        assert(worker_list[worker_index].init_status == 0);
        assert(worker_list[worker_index].fault_backend == worker_list[0].fault_backend);
    }

    close(device_fd);
}

/** Verify linked protected routines succeed and recover direct memory faults. */
static void direct_fault_routine_test(void)
{
    const struct catalejo_fault_backend *fault_backend = NULL;
    int device_fd = open(MIRILLA_DEVICE, O_RDWR | O_CLOEXEC);
    uint64_t source_value = 0x123456789abcdef0ULL;
    uint64_t target_value = 0;

    assert(device_fd >= 0);
    assert(catalejo_fault_backend_initialize(device_fd, &fault_backend) == 0 && fault_backend);
    assert(catalejo_read_u64(&source_value, &target_value) == CATALEJO_OUTCOME_SUCCESS &&
           target_value == source_value);
    assert(catalejo_read_u64((const uint64_t *)0x50, &target_value) == CATALEJO_OUTCOME_ERROR);
    assert(catalejo_write_u64(&target_value, &source_value) == CATALEJO_OUTCOME_SUCCESS &&
           target_value == source_value);
    assert(catalejo_write_u64((uint64_t *)0x50, &source_value) == CATALEJO_OUTCOME_ERROR);
    close(device_fd);
}

/** Run only the PTE-freeze stress case. */
int mirilla_test_exception_atomic(void)
{
    alarm(60);
    pte_freeze_stress_test();
    puts("exception slab PTE freeze passed");

    return 0;
}

/** Run all userspace exception-slab integration cases. */
int mirilla_test_exception(void)
{
    alarm(60);
    creation_and_mapping_test();
    publication_and_vma_test();
    hard_limit_test();
    table_replacement_test();
    mseal_interaction_test();
    pte_freeze_stress_test();
    concurrent_backend_test();
    fork_and_backend_test();
    direct_fault_routine_test();
    puts("exception slab publication passed");

    return 0;
}
