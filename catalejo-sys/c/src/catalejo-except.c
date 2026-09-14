#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "catalejo-except.h"
#include "catalejo-fixup.h"
#include "catalejo-section.h"

enum catalejo_fault_backend_state {
    /** No backend has been initialized in this process. */
    CATALEJO_FAULT_BACKEND_UNINITIALIZED,
    /** One thread is constructing the process singleton. */
    CATALEJO_FAULT_BACKEND_INITIALIZING,
    /** The singleton owns a published default slab for this pid. */
    CATALEJO_FAULT_BACKEND_INITIALIZED,
    /** The singleton was inherited across fork and must be rebuilt. */
    CATALEJO_FAULT_BACKEND_STALE,
};

/*
 * Process singleton backing the linked fault routines.
 *
 * NOTE(invariant): INITIALIZED means the associated atomic PID is the calling process and
 * record_list names a published default slab owned by exception_fd. A fork changes the observed
 * PID and marks the inherited descriptor and absent VM_DONTCOPY mapping stale before retrieval can
 * succeed.
 */
struct catalejo_fault_backend {
    /** Kernel identifier for the built-in exception context. */
    mirilla_except_id_t except_id;
    /** Owned descriptor for the built-in exception context. */
    int exception_fd;
    /** Published mapping containing the built-in records. */
    struct mirilla_except_record *record_list;
    /** Exact size of the published default slab. */
    virtual_size_t slab_size;
};

/** Process-lifetime storage for the built-in exception-handler capability. */
static struct catalejo_fault_backend catalejo_fault_backend = {
    .exception_fd = -1,
};

/** Initialization state published with acquire and release ordering. */
static atomic_int catalejo_fault_backend_state = CATALEJO_FAULT_BACKEND_UNINITIALIZED;

/** PID associated with the current singleton state. */
static atomic_int catalejo_fault_backend_process_id;

/** Validate the byte size accepted by slab mapping and protection helpers. */
static bool catalejo_except_slab_size_valid(virtual_size_t slab_size)
{
    bool size_present = slab_size != 0;
    bool size_representable = slab_size <= SIZE_MAX;

    return size_present && size_representable;
}

/** Create one fd-owned exception context through the Mirilla ioctl. */
mirilla_command_status_t catalejo_mirilla_except_create(int device_fd, virtual_size_t slab_size,
                                                        mirilla_except_id_t *except_id,
                                                        int *except_fd)
{
    union mirilla_except_create_io io = {
        .argument = {
            .slab_size = slab_size,
        },
    };
    int ioctl_status;
    bool id_output_present = except_id != NULL;
    bool fd_output_present = except_fd != NULL;

    if (!id_output_present || !fd_output_present)
        return -EINVAL;

    *except_id = MIRILLA_ID_NONE;
    *except_fd = -1;
    ioctl_status = ioctl(device_fd,
                         MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_EXCEPT,
                                                MIRILLA_COMMAND_EXCEPT_CREATE),
                         &io);
    if (ioctl_status < 0)
        return -errno;

    if (ioctl_status)
        return ioctl_status;

    *except_id = io.result.id;
    *except_fd = io.result.fd;

    return 0;
}

/** Map one exact-size writable exception slab. */
int catalejo_except_slab_map(int except_fd, virtual_size_t slab_size,
                             struct mirilla_except_record **record_list)
{
    void *mapping;

    if (!record_list)
        return -EINVAL;

    *record_list = NULL;
    if (!catalejo_except_slab_size_valid(slab_size))
        return -EINVAL;

    mapping = mmap(NULL, (size_t)slab_size, PROT_READ | PROT_WRITE, MAP_SHARED, except_fd, 0);
    if (mapping == MAP_FAILED)
        return -errno;

    *record_list = mapping;

    return 0;
}

/** Change one slab VMA from writable editing to read-only publication. */
static int catalejo_except_slab_protect(struct mirilla_except_record *record_list,
                                        virtual_size_t slab_size, int protection)
{
    if (!record_list)
        return -EINVAL;

    if (!catalejo_except_slab_size_valid(slab_size))
        return -EINVAL;

    return mprotect(record_list, (size_t)slab_size, protection) ? -errno : 0;
}

/** Publish one slab's records to the kernel. */
int catalejo_except_slab_publish(struct mirilla_except_record *record_list,
                                 virtual_size_t slab_size)
{
    return catalejo_except_slab_protect(record_list, slab_size, PROT_READ);
}

/** Return one published slab to writable editing. */
int catalejo_except_slab_edit(struct mirilla_except_record *record_list, virtual_size_t slab_size)
{
    return catalejo_except_slab_protect(record_list, slab_size, PROT_READ | PROT_WRITE);
}

/** Unmap one complete exception slab. */
int catalejo_except_slab_unmap(struct mirilla_except_record *record_list, virtual_size_t slab_size)
{
    if (!record_list)
        return -EINVAL;

    if (!catalejo_except_slab_size_valid(slab_size))
        return -EINVAL;

    return munmap(record_list, (size_t)slab_size) ? -errno : 0;
}

/** Sort records by the same key required by kernel validation. */
static int catalejo_except_record_order(const void *left_pointer, const void *right_pointer)
{
    const struct mirilla_except_record *left = left_pointer;
    const struct mirilla_except_record *right = right_pointer;

    if (left->boundary.base_address < right->boundary.base_address)
        return -1;
    if (left->boundary.base_address > right->boundary.base_address)
        return 1;
    if (left->boundary.region_size < right->boundary.region_size)
        return -1;
    if (left->boundary.region_size > right->boundary.region_size)
        return 1;
    if (left->predicate.except_mask < right->predicate.except_mask)
        return -1;
    if (left->predicate.except_mask > right->predicate.except_mask)
        return 1;

    return 0;
}

/** Resolve one linker-relative fixup into its immutable exception record. */
static int catalejo_except_record_load(const catalejo_rollback_record_t *fixup_record,
                                       struct mirilla_except_record *target_record)
{
    virtual_address_t start_address = virtual_relative_resolve(&fixup_record->start_address);
    virtual_address_t end_address = virtual_relative_resolve(&fixup_record->end_address);
    virtual_address_t rollback_address = virtual_relative_resolve(&fixup_record->rollback_address);
    mirilla_except_mask_t except_mask = fixup_record->except_mask;
    virtual_size_t region_size;
    bool start_present = start_address != 0;
    bool rollback_present = rollback_address != 0;
    bool ordered_range = end_address > start_address;
    bool vector_present = except_mask != 0;
    bool vector_supported = !(except_mask & ~MIRILLA_EXCEPT_X86_SUPPORTED_MASK);
    bool address_valid = start_present && rollback_present && ordered_range;
    bool vector_valid = vector_present && vector_supported;

    if (!address_valid)
        return -EINVAL;

    if (!vector_valid)
        return -EINVAL;

    region_size = end_address - start_address;
    if (region_size > UINT32_MAX)
        return -EINVAL;

    *target_record = (struct mirilla_except_record){
        .boundary = {
            .base_address = start_address,
            .region_size = (uint32_t)region_size,
        },
        .predicate = {
            .except_mask = except_mask,
        },
        .action = {
            .context.ip = {
                .address = rollback_address,
            },
            .tag = MIRILLA_EXCEPT_ACTION_IP,
        },
    };

    return 0;
}

/** Load and sort linked fault-fixup metadata into a zeroed slab. */
static int catalejo_except_records_load(struct mirilla_except_record *record_list,
                                        virtual_size_t slab_size)
{
    const catalejo_rollback_record_t *fixup_start =
        (const void *)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START;
    const catalejo_rollback_record_t *fixup_stop =
        (const void *)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP;
    uintptr_t fixup_start_address = (uintptr_t)fixup_start;
    uintptr_t fixup_stop_address = (uintptr_t)fixup_stop;
    size_t record_capacity = (size_t)slab_size / sizeof(*record_list);
    size_t fixup_size;
    size_t record_count;
    size_t record_index;

    catalejo_fault_routines_retain();

    if (fixup_stop_address < fixup_start_address)
        return -EOVERFLOW;

    fixup_size = fixup_stop_address - fixup_start_address;
    if (fixup_size % sizeof(*fixup_start))
        return -EINVAL;

    record_count = fixup_size / sizeof(*fixup_start);
    if (record_count > record_capacity)
        return -EOVERFLOW;

    for (record_index = 0; record_index < record_count; record_index++) {
        const catalejo_rollback_record_t *fixup_record = &fixup_start[record_index];
        int load_status = catalejo_except_record_load(fixup_record, &record_list[record_index]);

        if (load_status)
            return load_status;
    }

    qsort(record_list, record_count, sizeof(*record_list), catalejo_except_record_order);

    return 0;
}

/** Detect a pid change and mark inherited singleton state stale. */
static void catalejo_fault_backend_refresh(pid_t process_id)
{
    int known_process_id =
        atomic_load_explicit(&catalejo_fault_backend_process_id, memory_order_acquire);

    while (known_process_id != process_id) {
        if (!atomic_compare_exchange_weak_explicit(&catalejo_fault_backend_process_id,
                                                   &known_process_id, process_id,
                                                   memory_order_acq_rel, memory_order_acquire))
            continue;

        if (known_process_id != 0)
            atomic_store_explicit(&catalejo_fault_backend_state, CATALEJO_FAULT_BACKEND_STALE,
                                  memory_order_release);
        break;
    }
}

/** Build a fresh process singleton, context, mapping, and published table. */
static int catalejo_fault_backend_build(int device_fd)
{
    struct mirilla_except_record *record_list = NULL;
    mirilla_except_id_t except_id = MIRILLA_ID_NONE;
    int exception_fd = -1;
    int status;

    if (catalejo_fault_backend.exception_fd >= 0) {
        close(catalejo_fault_backend.exception_fd);
        catalejo_fault_backend.exception_fd = -1;
    }

    status = catalejo_mirilla_except_create(device_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, &except_id,
                                            &exception_fd);
    if (status)
        return status;

    status = catalejo_except_slab_map(exception_fd, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE, &record_list);
    if (status)
        goto close_fd;

    status = catalejo_except_records_load(record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE);
    if (status)
        goto unmap_slab;

    status = catalejo_except_slab_publish(record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE);
    if (status)
        goto unmap_slab;

    catalejo_fault_backend = (struct catalejo_fault_backend){
        .except_id = except_id,
        .exception_fd = exception_fd,
        .record_list = record_list,
        .slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE,
    };

    return 0;

unmap_slab:
    catalejo_except_slab_unmap(record_list, MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE);
close_fd:
    close(exception_fd);

    return status;
}

/** Initialize or reuse the singleton for the calling process. */
int catalejo_fault_backend_initialize(int device_fd, const struct catalejo_fault_backend **backend)
{
    pid_t process_id = getpid();

    if (!backend)
        return -EINVAL;

    *backend = NULL;
    catalejo_fault_backend_refresh(process_id);

    for (;;) {
        int state = atomic_load_explicit(&catalejo_fault_backend_state, memory_order_acquire);
        int expected;
        int status;

        if (state == CATALEJO_FAULT_BACKEND_INITIALIZED) {
            *backend = &catalejo_fault_backend;

            return 0;
        }

        if (state == CATALEJO_FAULT_BACKEND_INITIALIZING) {
            sched_yield();
            continue;
        }

        expected = state;
        if (!atomic_compare_exchange_weak_explicit(&catalejo_fault_backend_state, &expected,
                                                   CATALEJO_FAULT_BACKEND_INITIALIZING,
                                                   memory_order_acq_rel, memory_order_acquire))
            continue;

        status = catalejo_fault_backend_build(device_fd);
        atomic_store_explicit(&catalejo_fault_backend_state,
                              status ? state : CATALEJO_FAULT_BACKEND_INITIALIZED,
                              memory_order_release);
        if (status)
            return status;

        *backend = &catalejo_fault_backend;

        return 0;
    }
}

/** Retrieve the singleton only when it is current for the calling process. */
int catalejo_fault_backend_retrieve(const struct catalejo_fault_backend **backend)
{
    int state;

    if (!backend)
        return -EINVAL;

    *backend = NULL;
    catalejo_fault_backend_refresh(getpid());
    state = atomic_load_explicit(&catalejo_fault_backend_state, memory_order_acquire);

    if (state == CATALEJO_FAULT_BACKEND_STALE)
        return -ESTALE;
    if (state != CATALEJO_FAULT_BACKEND_INITIALIZED)
        return -ENOENT;

    *backend = &catalejo_fault_backend;

    return 0;
}
