#ifndef _CATALEJO_FAULT_H_
#define _CATALEJO_FAULT_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "catalejo-macro.h"
#include "catalejo-section.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The states of the atomic initialization word for the signal handler.
 */
typedef enum catalejo_initialize_state {
  CATALEJO_INITIALIZE_STATE_UNINITIALIZED,
  CATALEJO_INITIALIZE_STATE_INITIALIZING,
  CATALEJO_INITIALIZE_STATE_INITIALIZED,
  CATALEJO_INITIALIZE_STATE_FAILED,
  NR_CATALEJO_INITIALIZE_STATES
} catalejo_initialize_state_t;

/**
 * The primary initialization function.
 *
 * NOTE(invariant): This is required to be called before any protected read or
 * write is performed.
 */
extern catalejo_faultable_outcome_t catalejo_fault_initialize();

/**
 * X-macro for the fault routines.
 *
 * NOTE: The "up-size" (the 32-bit register window) register is used to
 * unconditionally zero-extend the register, even if we are performing a much
 * smaller load or store where the processor does not perform automatic
 * zero-extension due to legacy concerns. This also manages to break a pipeline
 * dependency.
 */
#define CATALEJO_FAULT_ROUTINE_SPECIFICATION                                   \
  X(u64, uint64_t, movq, rcx, ecx)                                             \
  X(u32, uint32_t, movl, ecx, ecx)                                             \
  X(u16, uint16_t, movw, cx, ecx)                                              \
  X(u8, uint8_t, movb, cl, ecx)

/* NOTE: Individual protected-read routines. */

#define X(target_typename, target_type, target_mnemonic, target_register,      \
          target_register32)                                                   \
  extern FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(           \
      catalejo_read_,                                                          \
      target_typename)(CATALEJO_UNUSED const target_type *target_source,       \
                       CATALEJO_UNUSED target_type *target_value);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

/* NOTE: Individual protected-write routines. */

#define X(target_typename, target_type, target_mnemonic, target_register,      \
          target_register32)                                                   \
  extern FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(           \
      catalejo_write_,                                                         \
      target_typename)(CATALEJO_UNUSED target_type * target_value,             \
                       CATALEJO_UNUSED const target_type *target_source);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

/**
 * Structure to be used to report back after a stream read-write operation.
 */
typedef struct catalejo_faultable_copy_outcome {
  /**
   * The status of the operation.
   *
   * NOTE(invariant): This should be contained in the `%rax` register.
   */
  catalejo_faultable_outcome_t outcome_status;

  /**
   * The count of affected bytes, depending on the operation performed.
   *
   * NOTE(invariant): This should be contained in the `%rdx` register.
   */
  uint64_t byte_count;
} catalejo_faultable_copy_outcome_t;

/**
 * Perform a bulk-copy that is fault-protected.
 */
extern FAULT_ROUTINE catalejo_faultable_copy_outcome_t
catalejo_copy(CATALEJO_UNUSED uint8_t *target_destination,
              CATALEJO_UNUSED const uint8_t *target_source,
              CATALEJO_UNUSED size_t target_count);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef _CATALEJO_FAULT_H_ */
