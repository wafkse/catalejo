/*
 * Mirilla-backed userspace exception fixups.
 */

#ifndef _MIRILLA_EXCEPT_H
#define _MIRILLA_EXCEPT_H

#include "mirilla-command.h"
#include "mirilla-miscellaneous.h"

/* These definitions are part of the userspace ABI. */
#ifdef __KERNEL__

#include <linux/list.h>
#include <linux/mm.h>

#else

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
 * A bitset of architectural exception vectors accepted by one rollback record.
 */
typedef uint64_t mirilla_except_mask_t;

/** Number of architectural vectors representable by mirilla_except_mask_t. */
#define MIRILLA_EXCEPT_VECTOR_LIMIT (64U)

/** Convert one architectural exception vector into its mask bit. */
#define MIRILLA_EXCEPT_MASK(target_vector) (1ULL << (target_vector))

/** x86 invalid-opcode exception vector (#UD). */
#define MIRILLA_EXCEPT_X86_INVALID_OPCODE (6U)

/** x86 general-protection exception vector (#GP). */
#define MIRILLA_EXCEPT_X86_GENERAL_PROTECTION (13U)

/** x86 page-fault exception vector (#PF). */
#define MIRILLA_EXCEPT_X86_PAGE_FAULT (14U)

/** x86 alignment-check exception vector (#AC). */
#define MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK (17U)

/** Architectural exception vectors currently accepted by Mirilla records on x86. */
#define MIRILLA_EXCEPT_X86_SUPPORTED_MASK                         \
    (MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_INVALID_OPCODE) |     \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION) | \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT) |         \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK))

/**
 * An exception record.
 *
 * This provides a stable mechanism to record an exception-fixup site in userspace.
 */
struct mirilla_except_record {
    /**
     * The first instruction address where this record may catch an exception.
     */
    virtual_relative_t start_address;

    /**
     * The first instruction address after the catchable range.
     */
    virtual_relative_t end_address;

    /**
     * The recovery address entered after an accepted exception.
     */
    virtual_relative_t rollback_address;

    /**
     * The architectural exception-vector mask accepted by this record.
     */
    mirilla_except_mask_t except_mask;
};

/**
 * A complete userspace memory region backed by one VMA.
 */
struct mirilla_except_region {
    /**
     * The first byte of the VMA.
     */
    virtual_address_t region_address;

    /**
     * The complete byte length of the VMA.
     */
    virtual_size_t region_size;
};

/**
 * A sealed userspace rollback image and its field-relative exception records.
 */
struct mirilla_except_image {
    /**
     * The executable region containing protected accessors and their rollback paths.
     */
    struct mirilla_except_region rollback_region;

    /**
     * The read-only region containing the exception table.
     */
    struct mirilla_except_region except_table;
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

struct mirilla_device_context;

/** Required VMA flags for the executable rollback region. */
#define MIRILLA_EXCEPT_ROLLBACK_REQUIRED_FLAGS (VM_READ | VM_EXEC | VM_MAYSHARE | VM_SEALED)

/** Forbidden VMA flags for the executable rollback region. */
#define MIRILLA_EXCEPT_ROLLBACK_FORBIDDEN_FLAGS (VM_WRITE | VM_MAYWRITE)

/** Required VMA flags for the read-only exception table. */
#define MIRILLA_EXCEPT_TABLE_REQUIRED_FLAGS (VM_READ | VM_MAYSHARE | VM_SEALED)

/** Forbidden VMA flags for the read-only exception table. */
#define MIRILLA_EXCEPT_TABLE_FORBIDDEN_FLAGS (VM_WRITE | VM_EXEC | VM_MAYWRITE)

/**
 * Required seal flags for each memfd VMA.
 */
#define MIRILLA_EXCEPT_MEMFD_REQUIRED_FLAGS \
    (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)

/**
 * The unique exception image registered for an observer address space.
 */
struct mirilla_except_registration {
    /** Link in the global RCU-published registration list. */
    struct list_head global_node;

    /** Link in the owning device session's registration list. */
    struct list_head device_node;

    /** Address space whose faults may use this registration. */
    struct mm_struct *mm;

    /** Immutable userspace regions consulted directly by the exception lookup path. */
    struct mirilla_except_image image;
};

/*
 * Find a registered userspace recovery address for one instruction and exception class.
 *
 * This is called from the ftrace exception path and therefore must not sleep or allocate.
 */
bool mirilla_except_lookup(struct mm_struct *mm, unsigned long instruction_pointer,
                           mirilla_except_mask_t except_mask, unsigned long *rollback_address);

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
