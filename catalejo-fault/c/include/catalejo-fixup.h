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
 * Signal classes accepted by a rollback record.
 *
 * These are independent of the platform's numeric signal values so the record format remains
 * simple and stable.
 */
#define CATALEJO_FAULT_SIGNAL_MEMORY \
    (MIRILLA_EXCEPT_SIGNAL_SEGMENTATION_FAULT | MIRILLA_EXCEPT_SIGNAL_BUS_ERROR)
#define CATALEJO_FAULT_SIGNAL_ALL \
    (CATALEJO_FAULT_SIGNAL_MEMORY | MIRILLA_EXCEPT_SIGNAL_ILLEGAL_INSTRUCTION)

/**
 * A rollback record found inside the fixup program section.
 *
 * Each relative pointer is a signed byte displacement from its own field to the target. This
 * permits the record to be used in position-independent executables and shared objects without
 * dynamic pointer relocations.
 */
typedef struct mirilla_except_record catalejo_rollback_record_t;

/**
 * Emit a rollback record from a basic inline-assembly template.
 *
 * The three addresses are assembly expressions supplied as string literals. Each displacement is
 * evaluated relative to its own 8-byte field. The allocated GNU-retained (applies `SHF_GNU_RETAIN`,
 * see https://maskray.me/blog/2021-02-28-linker-garbage-collection#gnu-ld) section remains
 * available to the runtime image builder even when linker garbage collection is enabled.
 */
// clang-format off
#define CATALEJO_ROLLBACK_RECORD(target_start, target_end, target_rollback, target_signal_mask) \
    ".pushsection " CATALEJO_FAULT_FIXUP_SECTION_NAME ",\"aR\",@progbits\n\t"                   \
    ".balign 8\n\t"                                                                             \
    ".8byte " target_start " - .\n\t"                                                           \
    ".8byte " target_end " - .\n\t"                                                             \
    ".8byte " target_rollback " - .\n\t"                                                        \
    ".8byte " CATALEJO_STR(target_signal_mask) "\n\t"                                           \
    ".popsection\n\t"
// clang-format on

#ifdef __cplusplus
}
#endif

#endif
