#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/memfd.h>

#include "catalejo-fixup.h"
#include "catalejo-image.h"

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

static atomic_int catalejo_image_state = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;
static struct catalejo_image_runtime catalejo_image_runtime;
static int catalejo_image_error = EIO;

/**
 * Round a nonzero byte count up to a page boundary.
 */
static bool catalejo_page_align(size_t target_size, size_t target_page_size, size_t *target_result)
{
    size_t target_remainder = target_size % target_page_size;

    if (!target_remainder) {
        *target_result = target_size;
        return true;
    }

    target_remainder = target_page_size - target_remainder;
    if (target_size > SIZE_MAX - target_remainder)
        return false;

    *target_result = target_size + target_remainder;
    return true;
}

/**
 * Write an entire byte range to a backing object.
 */
static int catalejo_write_all(int target_fd, const void *target_buffer, size_t target_count,
                              off_t target_offset)
{
    const uint8_t *target_bytes = target_buffer;
    size_t target_written = 0;

    while (target_written < target_count) {
        ssize_t target_step = pwrite(target_fd, target_bytes + target_written,
                                     target_count - target_written,
                                     target_offset + (off_t)target_written);

        if (target_step < 0) {
            if (errno == EINTR)
                continue;

            return -errno;
        }

        if (!target_step)
            return -EIO;

        target_written += (size_t)target_step;
    }

    return 0;
}

/**
 * Convert an absolute target and field address into the shared signed displacement format.
 */
static bool catalejo_relative_encode(uintptr_t target_address, uintptr_t target_field,
                                     virtual_relative_t *target_displacement)
{
    uintptr_t target_distance;

    if (target_address >= target_field) {
        target_distance = target_address - target_field;
        if (target_distance > INT64_MAX)
            return false;

        *target_displacement = (virtual_relative_t)target_distance;
    } else {
        target_distance = target_field - target_address;
        if (target_distance > (uint64_t)INT64_MAX + 1)
            return false;

        *target_displacement = target_distance == (uint64_t)INT64_MAX + 1 ?
                                   INT64_MIN :
                                   -(virtual_relative_t)target_distance;
    }

    return true;
}

/**
 * Rebase one original accessor address onto the copied image.
 */
static bool catalejo_image_rebase(uintptr_t target_original, uintptr_t target_original_start,
                                  uintptr_t target_original_stop, uintptr_t target_runtime_start,
                                  bool target_allow_stop, uintptr_t *target_runtime)
{
    bool target_in_range = target_original >= target_original_start &&
                           (target_original < target_original_stop ||
                            (target_allow_stop && target_original == target_original_stop));

    if (!target_in_range)
        return false;

    *target_runtime = target_runtime_start + (target_original - target_original_start);
    return true;
}

/**
 * Rebase and write every retained field-relative exception record.
 */
static int catalejo_image_write_records(int target_fd, off_t target_table_offset,
                                        uintptr_t target_runtime_table,
                                        uintptr_t target_runtime_accessor)
{
    uintptr_t target_original_start = (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_START;
    uintptr_t target_original_stop = (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_STOP;
    uintptr_t target_record_start = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START;
    uintptr_t target_record_stop = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP;
    size_t target_record_count =
        (target_record_stop - target_record_start) / sizeof(struct mirilla_except_record);
    size_t target_index;

    for (target_index = 0; target_index < target_record_count; ++target_index) {
        const struct mirilla_except_record *target_original_record =
            &((const struct mirilla_except_record *)target_record_start)[target_index];
        struct mirilla_except_record target_record;
        uintptr_t target_runtime_record_address = target_runtime_table +
                                                  sizeof(struct mirilla_except_table_header) +
                                                  target_index * sizeof(target_record);
        uintptr_t target_start;
        uintptr_t target_end;
        uintptr_t target_fixup;

        target_start = virtual_relative_resolve(&target_original_record->start_address);
        target_end = virtual_relative_resolve(&target_original_record->end_address);
        target_fixup = virtual_relative_resolve(&target_original_record->fixup_address);

        if (!catalejo_image_rebase(target_start, target_original_start, target_original_stop,
                                   target_runtime_accessor, false, &target_start) ||
            !catalejo_image_rebase(target_end, target_original_start, target_original_stop,
                                   target_runtime_accessor, true, &target_end) ||
            !catalejo_image_rebase(target_fixup, target_original_start, target_original_stop,
                                   target_runtime_accessor, false, &target_fixup) ||
            target_end <= target_start ||
            !catalejo_relative_encode(target_start,
                                      target_runtime_record_address +
                                          offsetof(struct mirilla_except_record, start_address),
                                      &target_record.start_address) ||
            !catalejo_relative_encode(target_end,
                                      target_runtime_record_address +
                                          offsetof(struct mirilla_except_record, end_address),
                                      &target_record.end_address) ||
            !catalejo_relative_encode(target_fixup,
                                      target_runtime_record_address +
                                          offsetof(struct mirilla_except_record, fixup_address),
                                      &target_record.fixup_address))
            return -EINVAL;

        target_record.except_mask = target_original_record->except_mask;

        int target_status = catalejo_write_all(
            target_fd, &target_record, sizeof(target_record),
            target_table_offset + (off_t)sizeof(struct mirilla_except_table_header) +
                (off_t)(target_index * sizeof(target_record)));
        if (target_status)
            return target_status;
    }

    return 0;
}

/**
 * Project one original function entry into the copied accessor mapping.
 */
static void *catalejo_image_entry(void *target_accessor, const void *target_original)
{
    uintptr_t target_offset =
        (uintptr_t)target_original - (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_START;

    return (void *)((uintptr_t)target_accessor + target_offset);
}

/**
 * Build the immutable runtime image before publishing any callable entry point.
 */
static int catalejo_image_construct(struct catalejo_image_runtime *target_runtime)
{
    uintptr_t target_code_start = (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_START;
    uintptr_t target_code_stop = (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_STOP;
    uintptr_t target_table_start = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START;
    uintptr_t target_table_stop = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP;
    size_t target_code_size = target_code_stop - target_code_start;
    size_t target_record_table_size = target_table_stop - target_table_start;
    size_t target_table_size;
    size_t target_page_size;
    size_t target_accessor_length;
    size_t target_table_length;
    size_t target_backing_length;
    size_t target_record_count;
    char target_fd_path[64];
    void *target_accessor = MAP_FAILED;
    void *target_table = MAP_FAILED;
    int target_fd = -1;
    int target_read_fd = -1;
    int target_status = -EINVAL;
    long target_page_size_raw = sysconf(_SC_PAGESIZE);

    if (target_page_size_raw <= 0 || !target_code_size || !target_record_table_size ||
        target_record_table_size % sizeof(struct mirilla_except_record) ||
        target_record_table_size > SIZE_MAX - sizeof(struct mirilla_except_table_header))
        return -EINVAL;

    target_page_size = (size_t)target_page_size_raw;
    target_record_count = target_record_table_size / sizeof(struct mirilla_except_record);
    target_table_size = sizeof(struct mirilla_except_table_header) + target_record_table_size;

    if (!catalejo_page_align(target_code_size, target_page_size, &target_accessor_length) ||
        !catalejo_page_align(target_table_size, target_page_size, &target_table_length) ||
        target_accessor_length > SIZE_MAX - target_table_length)
        return -EOVERFLOW;

    target_backing_length = target_accessor_length + target_table_length;
    target_fd = (int)syscall(SYS_memfd_create, "catalejo-fault",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_EXEC);
    if (target_fd < 0)
        return -errno;

    if (ftruncate(target_fd, (off_t)target_backing_length)) {
        target_status = -errno;
        goto release;
    }

    target_status =
        catalejo_write_all(target_fd, (const void *)target_code_start, target_code_size, 0);
    if (target_status)
        goto release;

    int target_path_length =
        snprintf(target_fd_path, sizeof(target_fd_path), "/proc/self/fd/%d", target_fd);
    if (target_path_length < 0 || (size_t)target_path_length >= sizeof(target_fd_path)) {
        target_status = -EOVERFLOW;
        goto release;
    }

    target_read_fd = open(target_fd_path, O_RDONLY | O_CLOEXEC);
    if (target_read_fd < 0) {
        target_status = -errno;
        goto release;
    }

    target_accessor =
        mmap(NULL, target_accessor_length, PROT_READ | PROT_EXEC, MAP_SHARED, target_read_fd, 0);
    if (target_accessor == MAP_FAILED) {
        target_status = -errno;
        goto release;
    }

    target_table = mmap(NULL, target_table_length, PROT_READ, MAP_SHARED, target_read_fd,
                        (off_t)target_accessor_length);
    if (target_table == MAP_FAILED) {
        target_status = -errno;
        goto release;
    }

    struct mirilla_except_table_header target_table_header = {
        .record_count = target_record_count,
    };

    target_status = catalejo_write_all(target_fd, &target_table_header, sizeof(target_table_header),
                                       (off_t)target_accessor_length);
    if (target_status)
        goto release;

    target_status = catalejo_image_write_records(target_fd, (off_t)target_accessor_length,
                                                 (uintptr_t)target_table,
                                                 (uintptr_t)target_accessor);
    if (target_status)
        goto release;

    if (fcntl(target_fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) <
        0) {
        target_status = -errno;
        goto release;
    }

    close(target_fd);
    target_fd = -1;

    if (syscall(__NR_mseal, target_accessor, target_accessor_length, 0) < 0 ||
        syscall(__NR_mseal, target_table, target_table_length, 0) < 0) {
        target_status = -errno;
        goto release;
    }

    target_runtime->image = (struct mirilla_except_image){
        .accessor_address = (virtual_address_t)(uintptr_t)target_accessor,
        .accessor_length = target_accessor_length,
        .table_address = (virtual_address_t)(uintptr_t)target_table,
        .table_length = target_table_length,
    };

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    target_runtime->entries.CATALEJO_CONCAT(read_, target_typename) = catalejo_image_entry(  \
        target_accessor, CATALEJO_CONCAT(catalejo_image_read_, target_typename));

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    target_runtime->entries.CATALEJO_CONCAT(write_, target_typename) = catalejo_image_entry( \
        target_accessor, CATALEJO_CONCAT(catalejo_image_write_, target_typename));

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

    target_runtime->entries.monitor_intel_arm =
        catalejo_image_entry(target_accessor, catalejo_image_monitor_intel_arm);
    target_runtime->entries.monitor_intel_wait =
        catalejo_image_entry(target_accessor, catalejo_image_monitor_intel_wait);
    target_runtime->entries.monitor_amd_arm =
        catalejo_image_entry(target_accessor, catalejo_image_monitor_amd_arm);
    target_runtime->entries.monitor_amd_wait =
        catalejo_image_entry(target_accessor, catalejo_image_monitor_amd_wait);
    target_runtime->entries.copy = catalejo_image_entry(target_accessor, catalejo_image_copy);

    target_accessor = MAP_FAILED;
    target_table = MAP_FAILED;
    target_status = 0;

release:
    if (target_accessor != MAP_FAILED)
        munmap(target_accessor, target_accessor_length);
    if (target_table != MAP_FAILED)
        munmap(target_table, target_table_length);
    if (target_read_fd >= 0)
        close(target_read_fd);
    if (target_fd >= 0)
        close(target_fd);

    return target_status;
}

int catalejo_fault_image_initialize(struct mirilla_except_image *target_image)
{
    int target_state = atomic_load_explicit(&catalejo_image_state, memory_order_acquire);

    if (target_state == CATALEJO_INITIALIZE_STATE_UNINITIALIZED) {
        int target_expected = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;

        if (atomic_compare_exchange_strong_explicit(&catalejo_image_state, &target_expected,
                                                    CATALEJO_INITIALIZE_STATE_INITIALIZING,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            int target_status = catalejo_image_construct(&catalejo_image_runtime);

            if (target_status) {
                catalejo_image_error = -target_status;
                atomic_store_explicit(&catalejo_image_state, CATALEJO_INITIALIZE_STATE_FAILED,
                                      memory_order_release);
            } else
                atomic_store_explicit(&catalejo_image_state, CATALEJO_INITIALIZE_STATE_INITIALIZED,
                                      memory_order_release);
        }
    }

    while ((target_state = atomic_load_explicit(&catalejo_image_state, memory_order_acquire)) ==
           CATALEJO_INITIALIZE_STATE_INITIALIZING)
        sched_yield();

    if (target_state != CATALEJO_INITIALIZE_STATE_INITIALIZED)
        return -catalejo_image_error;

    if (target_image)
        *target_image = catalejo_image_runtime.image;

    return 0;
}

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    catalejo_faultable_outcome_t CATALEJO_CONCAT(catalejo_read_, target_typename)(           \
        const target_type *target_source, target_type *target_value)                         \
    {                                                                                        \
        return catalejo_image_runtime.entries.CATALEJO_CONCAT(read_, target_typename)(       \
            target_source, target_value);                                                    \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    catalejo_faultable_outcome_t CATALEJO_CONCAT(catalejo_write_, target_typename)(          \
        target_type * target_value, const target_type *target_source)                        \
    {                                                                                        \
        return catalejo_image_runtime.entries.CATALEJO_CONCAT(write_, target_typename)(      \
            target_value, target_source);                                                    \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

catalejo_faultable_instruction_outcome_t catalejo_monitor_intel_arm(const uint8_t *target_address)
{
    return catalejo_image_runtime.entries.monitor_intel_arm(target_address);
}

catalejo_faultable_instruction_outcome_t catalejo_monitor_intel_wait(void)
{
    return catalejo_image_runtime.entries.monitor_intel_wait();
}

catalejo_faultable_instruction_outcome_t catalejo_monitor_amd_arm(const uint8_t *target_address)
{
    return catalejo_image_runtime.entries.monitor_amd_arm(target_address);
}

catalejo_faultable_instruction_outcome_t catalejo_monitor_amd_wait(void)
{
    return catalejo_image_runtime.entries.monitor_amd_wait();
}

catalejo_faultable_copy_outcome_t catalejo_copy(uint8_t *target_address,
                                                const uint8_t *target_source, size_t target_count)
{
    return catalejo_image_runtime.entries.copy(target_address, target_source, target_count);
}
