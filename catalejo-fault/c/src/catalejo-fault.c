#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <cpuid.h>
#include <signal.h>

#include "catalejo-macro.h"
#include "catalejo-section.h"
#include "catalejo-fault.h"
#include "catalejo-fixup.h"
#include "catalejo-image.h"

/**
 * The cached runtime monitor backend.
 */
static atomic_int catalejo_monitor_backend = -1;

/**
 * The maximum cycle interval used by one hardware wait.
 */
#define CATALEJO_MONITOR_WAIT_CYCLES 65536

/**
 * The CPUID feature bits used for monitor selection.
 */
#define CATALEJO_CPUID_WAITPKG (1U << 5)
#define CATALEJO_CPUID_MONITORX (1U << 29)

/**
 * Detect the available hardware monitor implementation.
 */
static catalejo_monitor_backend_t catalejo_monitor_detect()
{
    unsigned int target_eax;
    unsigned int target_ebx;
    unsigned int target_ecx;
    unsigned int target_edx;

    if (__get_cpuid_max(0, NULL) >= 7 &&
        __get_cpuid_count(7, 0, &target_eax, &target_ebx, &target_ecx, &target_edx) &&
        (target_ecx & CATALEJO_CPUID_WAITPKG) != 0)
        return CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR;

    if (__get_cpuid_max(0x80000000, NULL) >= 0x80000001 &&
        __get_cpuid(0x80000001, &target_eax, &target_ebx, &target_ecx, &target_edx) &&
        (target_ecx & CATALEJO_CPUID_MONITORX) != 0)
        return CATALEJO_MONITOR_BACKEND_AMD_MONITORX;

    return CATALEJO_MONITOR_BACKEND_UNSUPPORTED;
}

catalejo_monitor_backend_t catalejo_monitor_select()
{
    int target_backend = atomic_load_explicit(&catalejo_monitor_backend, memory_order_acquire);

    if (target_backend >= 0)
        return (catalejo_monitor_backend_t)target_backend;

    int detected_backend = catalejo_monitor_detect();
    int expected_backend = -1;

    if (atomic_compare_exchange_strong_explicit(&catalejo_monitor_backend, &expected_backend,
                                                detected_backend, memory_order_acq_rel,
                                                memory_order_acquire))
        return (catalejo_monitor_backend_t)detected_backend;

    return (catalejo_monitor_backend_t)expected_backend;
}

/**
 * Permanently downgrade a backend after an illegal instruction.
 */
static void catalejo_monitor_downgrade(catalejo_monitor_backend_t target_backend)
{
    int expected_backend = target_backend;

    atomic_compare_exchange_strong_explicit(&catalejo_monitor_backend, &expected_backend,
                                            CATALEJO_MONITOR_BACKEND_UNSUPPORTED,
                                            memory_order_acq_rel, memory_order_acquire);
}

catalejo_monitor_arm_outcome_t catalejo_monitor_arm(const uint8_t *target_address)
{
    catalejo_monitor_backend_t target_backend = catalejo_monitor_select();
    catalejo_faultable_instruction_outcome_t target_outcome;

    switch (target_backend) {
    case CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR:
        target_outcome = catalejo_monitor_intel_arm(target_address);
        break;
    case CATALEJO_MONITOR_BACKEND_AMD_MONITORX:
        target_outcome = catalejo_monitor_amd_arm(target_address);
        break;
    case CATALEJO_MONITOR_BACKEND_UNSUPPORTED:
        return CATALEJO_MONITOR_ARM_UNSUPPORTED;
    default:
        return CATALEJO_MONITOR_ARM_UNSUPPORTED;
    }

    if (target_outcome.outcome_status == CATALEJO_OUTCOME_SUCCESS)
        return target_backend == CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR ?
                   CATALEJO_MONITOR_ARM_INTEL_UMONITOR :
                   CATALEJO_MONITOR_ARM_AMD_MONITORX;

    if (target_outcome.fault_signal == SIGILL) {
        catalejo_monitor_downgrade(target_backend);

        return CATALEJO_MONITOR_ARM_UNSUPPORTED;
    }

    return CATALEJO_MONITOR_ARM_FAULT;
}

catalejo_faultable_outcome_t catalejo_monitor_wait(catalejo_monitor_backend_t target_backend)
{
    if (target_backend != catalejo_monitor_select())
        return CATALEJO_OUTCOME_INVALID_VALUE;

    catalejo_faultable_instruction_outcome_t target_outcome;

    switch (target_backend) {
    case CATALEJO_MONITOR_BACKEND_INTEL_UMONITOR:
        target_outcome = catalejo_monitor_intel_wait();
        break;
    case CATALEJO_MONITOR_BACKEND_AMD_MONITORX:
        target_outcome = catalejo_monitor_amd_wait();
        break;
    case CATALEJO_MONITOR_BACKEND_UNSUPPORTED:
        return CATALEJO_OUTCOME_INVALID_VALUE;
    default:
        return CATALEJO_OUTCOME_INVALID_VALUE;
    }

    if (target_outcome.outcome_status == CATALEJO_OUTCOME_SUCCESS)
        return CATALEJO_OUTCOME_SUCCESS;

    // NOTE A wait instruction has no memory operand that can explain a fault.
    catalejo_monitor_downgrade(target_backend);

    return CATALEJO_OUTCOME_INVALID_VALUE;
}

// clang-format off
// The naked routine bodies below are hand-formatted assembly. clang-format
// cannot lay out the backslash-continued `__asm__` template stably, so it is
// held off across the macro definitions.
#define X(target_typename, target_type, target_mnemonic, target_register, target_register32)         \
    CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t                                             \
        CATALEJO_CONCAT(catalejo_image_read_, target_typename)(CATALEJO_UNUSED const target_type *target_source, \
                                                         CATALEJO_UNUSED target_type *target_value)  \
    {                                                                                                \
        __asm__ volatile(                                                                            \
            /* %rdi = target_source, %rsi = target_value */                                          \
            /* NOTE: Forcefully clear the register for the read. */                                  \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"                              \
                                                                                                     \
            /* NOTE: Read the value and move it to the out-pointer. */                               \
            "1:\n\t"                                                                                \
            #target_mnemonic " (%rdi), %" #target_register "\n\t"                                    \
            #target_mnemonic " %" #target_register ", (%rsi)\n\t"                                    \
            "2:\n\t"                                                                                \
                                                                                                     \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0). */                  \
            "xorl %eax, %eax\n\t"                                                                    \
            "ret\n\t"                                                                                \
                                                                                                     \
            /* NOTE: Report a recoverable memory fault through the ordinary return ABI. */          \
            "3:\n\t"                                                                                \
            "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"                         \
            "ret\n\t"                                                                                \
                                                                                                     \
            CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_MEMORY));            \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32)         \
    CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t                                             \
        CATALEJO_CONCAT(catalejo_image_write_, target_typename)(CATALEJO_UNUSED target_type *target_value, \
                                                          CATALEJO_UNUSED const target_type *target_source) \
    {                                                                                                \
        __asm__ volatile(                                                                            \
            /* %rdi = target_value, %rsi = target_source */                                          \
            /* NOTE: Forcefully clear the register for the write. */                                 \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"                              \
                                                                                                     \
            /* NOTE: Read the source value and write it to the target address. */                    \
            "1:\n\t"                                                                                \
            #target_mnemonic " (%rsi), %" #target_register "\n\t"                                    \
            #target_mnemonic " %" #target_register ", (%rdi)\n\t"                                    \
            "2:\n\t"                                                                                \
                                                                                                     \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0). */                  \
            "xorl %eax, %eax\n\t"                                                                    \
            "ret\n\t"                                                                                \
                                                                                                     \
            /* NOTE: Report a recoverable memory fault through the ordinary return ABI. */          \
            "3:\n\t"                                                                                \
            "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"                         \
            "ret\n\t"                                                                                \
                                                                                                     \
            CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_MEMORY));            \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X
// clang-format on

CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_intel_arm(CATALEJO_UNUSED const uint8_t *target_address)
{
    // clang-format off
    __asm__ volatile(
        /* Move the local downstream address into the UMONITOR register. */
        "movq %rdi, %rax\n\t"

        /* UMONITOR %rax */
        "1:\n\t"
        "umonitor %rax\n\t"
        "2:\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t"

        /* Report the fault signal supplied by Mirilla in %rdx. */
        "3:\n\t"
        "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"
        "ret\n\t"

        CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_ALL));
    // clang-format on
}

CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_image_monitor_intel_wait()
{
    // clang-format off
    __asm__ volatile(
        /* Build a bounded absolute TSC deadline in EDX and EAX. */
        "rdtsc\n\t"
        "addl $" CATALEJO_STR(CATALEJO_MONITOR_WAIT_CYCLES) ", %eax\n\t"
        "adcl $0, %edx\n\t"
        "xorl %ecx, %ecx\n\t"

        /* UMWAIT %ecx */
        "1:\n\t"
        "umwait %ecx\n\t"
        "2:\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t"

        /* Report the fault signal supplied by Mirilla in %rdx. */
        "3:\n\t"
        "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"
        "ret\n\t"

        CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_ALL));
    // clang-format on
}

CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_amd_arm(CATALEJO_UNUSED const uint8_t *target_address)
{
    // clang-format off
    __asm__ volatile(
        /* MONITORX uses RAX for the local downstream address. */
        "movq %rdi, %rax\n\t"
        "xorl %ecx, %ecx\n\t"
        "xorl %edx, %edx\n\t"

        /* MONITORX */
        "1:\n\t"
        "monitorx\n\t"
        "2:\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t"

        /* Report the fault signal supplied by Mirilla in %rdx. */
        "3:\n\t"
        "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"
        "ret\n\t"

        CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_ALL));
    // clang-format on
}

CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_image_monitor_amd_wait()
{
    // clang-format off
    __asm__ volatile(
        /* MWAITX receives hints in EAX, extensions in ECX, and a finite cycle count in EBX. Preserve
           the callee-saved RBX value in the caller-saved R8 register across both exit paths. */
        "movq %rbx, %r8\n\t"
        "xorl %eax, %eax\n\t"
        /* NOTE: Bit 1 enables the EBX timeout expressed in Software P0 clocks, the same clocks
           counted by the TSC. */
        "movl $0b10, %ecx\n\t"
        "movl $" CATALEJO_STR(CATALEJO_MONITOR_WAIT_CYCLES) ", %ebx\n\t"

        /* MWAITX */
        "1:\n\t"
        "mwaitx\n\t"
        "2:\n\t"

        /* Restore RBX and report success with no fault signal. */
        "movq %r8, %rbx\n\t"
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t"

        /* Restore RBX and report the fault signal supplied by Mirilla in %rdx. */
        "3:\n\t"
        "movq %r8, %rbx\n\t"
        "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"
        "ret\n\t"

        CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_ALL));
    // clang-format on
}

CATALEJO_FAULT_ROUTINE catalejo_faultable_copy_outcome_t catalejo_image_copy(
    CATALEJO_UNUSED uint8_t *target_address, CATALEJO_UNUSED const uint8_t *target_source,
    CATALEJO_UNUSED size_t target_count)
{
    // clang-format off
    __asm__ volatile(
        /* %rdi = target_destination, %rsi = target_source, %rdx = target_count.
           This is already the register layout `rep movsb` expects (destination
           in %rdi, source in %rsi, count in %rcx), so no shuffling is needed
           beyond seeding the counter. */

        /* NOTE: The System V ABI guarantees a clear direction flag (RFLAGS.DF=0) on entry, but
         * we re-assert it so the copy advances forward. */
        "cld\n\t"

        /* NOTE: Seed the string-operation counter with the byte count. */
        "movq %rdx, %rcx\n\t"

        /* NOTE: [%rdi] = [%rsi] for %rcx bytes. A page fault on either side is
           restartable, as %rcx holds the remaining count at the faulting byte. */
        "1:\n\t"
        "rep movsb\n\t"
        "2:\n\t"

        /* NOTE: Reached only on full success, where %rcx has drained to zero, we
           surface it as the (zero) remaining count in the second return value. */
        "movq %rcx, %rdx\n\t"

        /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0). */
        "xorl %eax, %eax\n\t"
        "ret\n\t"

        /* NOTE: Preserve the remaining string-operation count on rollback. */
        "3:\n\t"
        "movq %rcx, %rdx\n\t"
        "movl $" CATALEJO_STR(CATALEJO_OUTCOME_ERROR_VALUE) ", %eax\n\t"
        "ret\n\t"

        CATALEJO_ROLLBACK_RECORD("1b", "2b", "3b", CATALEJO_FAULT_SIGNAL_MEMORY));
    // clang-format on
}
