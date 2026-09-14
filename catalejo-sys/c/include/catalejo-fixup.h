#ifndef _CATALEJO_FIXUP_H_
#define _CATALEJO_FIXUP_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "catalejo-macro.h"
#include "catalejo-section.h"
#include "mirilla-except.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Architectural exceptions accepted by ordinary protected memory operations.
 */
#define CATALEJO_FAULT_EXCEPTION_MEMORY                           \
    (MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION) | \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT) |         \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK))

/**
 * Architectural exceptions accepted by protected operations using optional instructions.
 */
#define CATALEJO_FAULT_EXCEPTION_ALL \
    (CATALEJO_FAULT_EXCEPTION_MEMORY | MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_INVALID_OPCODE))

/**
 * A rollback record found inside the fixup program section.
 *
 * NOTE(invariant): Each relative-address field is a signed byte displacement from its own location
 * to the target. The record therefore remains valid in position-independent executables and shared objects
 * without dynamic pointer relocations. `start_address` precedes `end_address`, and `except_mask`
 * contains the architectural vectors accepted by the protected instruction range.
 */
typedef struct {
    /** Signed displacement from this field to the first protected instruction. */
    virtual_relative_t start_address;

    /** Signed displacement from this field to the exclusive protected-instruction bound. */
    virtual_relative_t end_address;

    /** Signed displacement from this field to the recovery label used by the published table. */
    virtual_relative_t rollback_address;

    /** Architectural exception vectors accepted by the protected instruction range. */
    mirilla_except_mask_t except_mask;
} catalejo_rollback_record_t;

/**
 * Emit a rollback record from a basic inline-assembly template.
 *
 * The three addresses are assembly expressions supplied as string literals. Each displacement is
 * evaluated relative to its own 8-byte field. The allocated GNU-retained (applies `SHF_GNU_RETAIN`,
 * see https://maskray.me/blog/2021-02-28-linker-garbage-collection#gnu-ld) section remains
 * available to the built-in record loader even when linker garbage collection is enabled.
 */
// clang-format off
#define CATALEJO_ROLLBACK_RECORD(target_start, target_end, target_rollback, target_exception_mask) \
    ".pushsection " CATALEJO_FAULT_FIXUP_SECTION_NAME ",\"aR\",@progbits\n\t"                   \
    ".balign 8\n\t"                                                                             \
    ".8byte " target_start " - .\n\t"                                                           \
    ".8byte " target_end " - .\n\t"                                                             \
    ".8byte " target_rollback " - .\n\t"                                                        \
    ".8byte " CATALEJO_STR(target_exception_mask) "\n\t"                                           \
    ".popsection\n\t"
// clang-format on

/** Emit vector-specific monitor recovery trampolines and their rollback records. */
// clang-format off
#define CATALEJO_MONITOR_RECOVERY \
    "4:\n\tmovl $" CATALEJO_STR(MIRILLA_EXCEPT_X86_INVALID_OPCODE) ", %edx\n\tjmp 3b\n\t" \
    "5:\n\tmovl $" CATALEJO_STR(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION) ", %edx\n\tjmp 3b\n\t" \
    "6:\n\tmovl $" CATALEJO_STR(MIRILLA_EXCEPT_X86_PAGE_FAULT) ", %edx\n\tjmp 3b\n\t" \
    "7:\n\tmovl $" CATALEJO_STR(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK) ", %edx\n\tjmp 3b\n\t" \
    CATALEJO_ROLLBACK_RECORD("1b", "2b", "4b", MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_INVALID_OPCODE)) \
    CATALEJO_ROLLBACK_RECORD("1b", "2b", "5b", MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION)) \
    CATALEJO_ROLLBACK_RECORD("1b", "2b", "6b", MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT)) \
    CATALEJO_ROLLBACK_RECORD("1b", "2b", "7b", MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK))
// clang-format on

#ifdef __cplusplus
}
#endif

#endif
