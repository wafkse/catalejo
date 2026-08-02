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
 * The available hardware monitor implementations.
 */
typedef enum catalejo_monitor_backend {
    CATALEJO_MONITOR_BACKEND_UNSUPPORTED,
    CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR,
    CATALEJO_MONITOR_BACKEND_AMD_MONITORX,
} catalejo_monitor_backend_t;

/**
 * The result of arming a hardware monitor.
 */
typedef enum catalejo_monitor_arm_outcome {
    CATALEJO_MONITOR_ARM_FAULT = -1,
    CATALEJO_MONITOR_ARM_UNSUPPORTED,
    CATALEJO_MONITOR_ARM_INTEL_UMONITOR,
    CATALEJO_MONITOR_ARM_AMD_MONITORX,
} catalejo_monitor_arm_outcome_t;

/**
 * Select a hardware monitor implementation with runtime CPUID checks.
 */
extern catalejo_monitor_backend_t catalejo_monitor_select();

/**
 * Arm monitoring for a local downstream address.
 */
extern catalejo_monitor_arm_outcome_t catalejo_monitor_arm(const uint8_t *target_address);

/**
 * Wait for one bounded hardware interval.
 */
extern catalejo_faultable_outcome_t
catalejo_monitor_wait(catalejo_monitor_backend_t target_backend);

/**
 * X-macro for the fault routines.
 *
 * NOTE: The "up-size" (the 32-bit register window) register is used to
 * unconditionally zero-extend the register, even if we are performing a much
 * smaller load or store where the processor does not perform automatic
 * zero-extension due to legacy concerns. This also manages to break a pipeline
 * dependency.
 */
#define CATALEJO_FAULT_ROUTINE_SPECIFICATION \
    X(u64, uint64_t, movq, rcx, ecx)         \
    X(u32, uint32_t, movl, ecx, ecx)         \
    X(u16, uint16_t, movw, cx, ecx)          \
    X(u8, uint8_t, movb, cl, ecx)

/* NOTE: Individual protected-read routines. */

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(              \
        catalejo_read_, target_typename)(CATALEJO_UNUSED const target_type *target_source,   \
                                         CATALEJO_UNUSED target_type *target_value);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

/* NOTE: Individual protected-write routines. */

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(              \
        catalejo_write_, target_typename)(CATALEJO_UNUSED target_type * target_value,        \
                                          CATALEJO_UNUSED const target_type *target_source);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

/**
 * Structure used to report a protected instruction result.
 */
typedef struct catalejo_faultable_instruction_outcome {
    /**
     * The status of the instruction.
     *
     */
    // NOTE(invariant): This should be contained in the `%rax` register.
    catalejo_faultable_outcome_t outcome_status;

    /**
     * The signal raised by a faulting instruction.
     */
    // NOTE(invariant): This should be contained in the `%rax` register.
    size_t fault_signal;
} catalejo_faultable_instruction_outcome_t;

/**
 * Arm an Intel user monitor with fault protection.
 */
extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_monitor_intel_arm(CATALEJO_UNUSED const uint8_t *target_address);

/**
 * Wait with Intel user wait support for a bounded interval.
 */
extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_monitor_intel_wait();

/**
 * Arm an AMD extended monitor with fault protection.
 */
extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_monitor_amd_arm(CATALEJO_UNUSED const uint8_t *target_address);

/**
 * Wait with AMD extended wait support for a bounded interval.
 */
extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_monitor_amd_wait();

/**
 * Structure to be used to report back after a bulk read-write operation.
 */
typedef struct catalejo_faultable_copy_outcome {
    /**
    * The status of the operation.
    */
    // NOTE(invariant): This should be contained in the `%rax` register.
    catalejo_faultable_outcome_t outcome_status;

    /**
   * The count of affected bytes, depending on the operation performed.
   */
    // NOTE(invariant): This should be contained in the `%rdx` register.
    size_t byte_count;
} catalejo_faultable_copy_outcome_t;

/**
 * Perform a bulk-copy that is fault-protected.
 */
extern CATALEJO_FAULT_ROUTINE catalejo_faultable_copy_outcome_t
catalejo_copy(CATALEJO_UNUSED uint8_t *target_address, CATALEJO_UNUSED const uint8_t *target_source,
              CATALEJO_UNUSED size_t target_count);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef _CATALEJO_FAULT_H_ */
