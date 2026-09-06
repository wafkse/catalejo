/*
 * Mirilla-backed userspace exception fixups.
 */

#ifndef _MIRILLA_EXCEPT_H
#define _MIRILLA_EXCEPT_H

#include "mirilla-command.h"
#include "mirilla-miscellaneous.h"

/* These definitions are part of the userspace ABI. */
#ifndef __KERNEL__

#include <stddef.h>
#include <stdint.h>

#endif /* __KERNEL__ */

/*
 * The top-level command category for exception-fixup functionality.
 */
#define MIRILLA_COMMAND_CATEGORY_EXCEPT 0x01

#define MIRILLA_EXCEPT_COMMANDS X(REGISTER, 0x00, register)

#define X(name, val, io) MIRILLA_COMMAND_EXCEPT_##name = val,
enum { MIRILLA_EXCEPT_COMMANDS MIRILLA_EXCEPT_NR_COMMANDS };
#undef X

#define X(name, val, io) "MIRILLA_COMMAND_EXCEPT_" #name,
__attribute__((unused)) static const char *MIRILLA_COMMAND_NAME_EXCEPT_TABLE[] = {
    MIRILLA_EXCEPT_COMMANDS NULL
};
#undef X

/*
 * Determine the human-readable name of an exception command.
 */
#define MIRILLA_COMMAND_NAME_EXCEPT(except_command) \
    (MIRILLA_COMMAND_NAME_EXCEPT_TABLE[except_command])

/**
 * An except signal mask.
 *
 * Used to filter the exception set in an except-fixup site.
 */
typedef uint64_t mirilla_except_signal_mask_t;

#define MIRILLA_EXCEPT_SIGNAL_SEGMENTATION_FAULT (1U << 0)

#define MIRILLA_EXCEPT_SIGNAL_BUS_ERROR (1U << 1)

#define MIRILLA_EXCEPT_SIGNAL_ILLEGAL_INSTRUCTION (1U << 2)

/**
 * An exception record.
 *
 * This provides a stable mechanism to record an exception-fixup site in userspace.
 */
struct mirilla_except_record {
    /**
     * The address range [@start_address, @end_address) where an exception may be caught.
     */
    virtual_relative_t start_address, end_address;

    /**
     * The recovery address entered after an accepted exception.
     */
    virtual_relative_t rollback_address;

    /**
     * The exception signal mask in use.
     */
    mirilla_except_signal_mask_t except_mask;
};

/**
 * Header stored at the beginning of a self-contained exception-table VMA.
 *
 * The valid `struct mirilla_except_record` array immediately follows this header. Remaining bytes in
 * the page-aligned VMA are padding and are not part of the table.
 */
struct mirilla_except_table_header {
    /**
     * The number of valid records following this header.
     */
    virtual_size_t record_count;
};

/**
 * A sealed userspace accessor image and its field-relative exception records.
 */
struct mirilla_except_image {
    /**
     * The first byte of the executable accessor VMA.
     */
    virtual_address_t accessor_address;

    /**
     * The complete length of the executable accessor VMA.
     */
    virtual_size_t accessor_length;

    /**
     * The first byte of the read-only exception-table VMA.
     */
    virtual_address_t table_address;

    /**
     * The complete length of the exception-table VMA.
     */
    virtual_size_t table_length;
};

struct mirilla_except_register_argument {
    /**
     * The immutable image to register for the calling address space.
     */
    struct mirilla_except_image image;
};

struct mirilla_except_register_result {};

#define MIRILLA_EXCEPT_DEFINE_COMMAND_IO(name)            \
    union mirilla_except_##name##_io {                    \
        struct mirilla_except_##name##_argument argument; \
        struct mirilla_except_##name##_result result;     \
    };                                                    \
    MIRILLA_ASSERT_IO_SIZE(except_##name)

MIRILLA_EXCEPT_DEFINE_COMMAND_IO(register);

#ifdef __KERNEL__

#include <linux/list.h>
#include <linux/mm_types.h>

struct mirilla_device_context;

/*
 * An absolute kernel-owned exception record.
 */
struct mirilla_except_kernel_record {
    unsigned long start_address;
    unsigned long end_address;
    unsigned long fixup_address;
    mirilla_except_signal_mask_t except_mask;
};

/*
 * One immutable image registered by a device session for an observer address space.
 */
struct mirilla_except_registration {
    struct list_head global_node;
    struct list_head device_node;
    struct mm_struct *mm;
    struct mirilla_except_image image;
    size_t record_count;
    struct mirilla_except_kernel_record records[];
};

/*
 * Install the exception interception backend.
 */
int mirilla_except_initialize(void);

/*
 * Remove the exception interception backend.
 */
void mirilla_except_deinitialize(void);

/*
 * Handle an exception command for one device session.
 */
mirilla_command_status_t
mirilla_except_handle_command(struct mirilla_device_context *device_context,
                              mirilla_command_t command, mirilla_command_argument_t argument);

/*
 * Remove all exception images owned by one device session.
 */
void mirilla_except_remove_device(struct mirilla_device_context *device_context);

#endif /* __KERNEL__ */

#endif /* ifndef _MIRILLA_EXCEPT_H */
