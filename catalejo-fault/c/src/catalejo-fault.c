#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <cpuid.h>
#include <signal.h>
#include <sched.h>

#include "catalejo-macro.h"
#include "catalejo-section.h"
#include "catalejo-fault.h"
#include "catalejo-signal.h"

/**
 * The atomic word initialization state for the fault-catching subsystem.
 */
static atomic_int catalejo_initialize_state = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;

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
 * Initialize the catalejo-fault environment.
 *
 * This attempts to initialize the environment in which the fault-handling semantics are to be performed.
 *
 * Particularly, this sets up the signal handlers in a lazy yet thread-safe manner.
 */
catalejo_faultable_outcome_t catalejo_fault_initialize()
{
    if (atomic_load_explicit(&catalejo_initialize_state, memory_order_acquire) ==
        CATALEJO_INITIALIZE_STATE_INITIALIZED)
        return CATALEJO_OUTCOME_SUCCESS;

    int expected_state = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;

    if (atomic_compare_exchange_strong_explicit(&catalejo_initialize_state, &expected_state,
                                                CATALEJO_INITIALIZE_STATE_INITIALIZING,
                                                memory_order_acq_rel, memory_order_acquire)) {
        struct sigaction install_signal;

        {
            // NOTE: Use the signal handler for catalejo.
            install_signal.sa_sigaction = catalejo_signal_handle;

            // NOTE: Do not mask any individual signal during handling.
            sigemptyset(&install_signal.sa_mask);

            // NOTE: Flag rationale:
            //
            // * SA_SIGINFO: we need the three-argument form to reach the
            //   `ucontext_t`, whose `mcontext` we rewrite to simulate the
            //   faulting routine returning CATALEJO_OUTCOME_ERROR.
            //
            // * SA_NODEFER: the same signal must not be masked while we handle
            //   it. The chaining path re-`raise()`s the signal against the
            //   default disposition, and a nested fault must still be deliverable.
            //
            // * SA_ONSTACK: the fault path itself needs no alternate stack, as the
            //   naked routines touch no stack, so the faulting `%rsp` points
            //   straight at the return address and the kernel builds the signal
            //   frame harmlessly below it. We deliberately install no altstack of
            //   our own. We keep this flag only to honor a host-provided altstack
            //   (e.g. one installed for stack-overflow detection) so that the
            //   *chaining* path remains deliverable when the main stack is exhausted.
            install_signal.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        }

        int r0 = sigaction(SIGSEGV, &install_signal, &saved_segmentation_violation_signal_actor);
        int r1 = sigaction(SIGBUS, &install_signal, &saved_bus_signal_actor);
        int r2 = sigaction(SIGILL, &install_signal, &saved_illegal_instruction_signal_actor);

        if (r0 == 0 && r1 == 0 && r2 == 0) {
            atomic_store_explicit(&catalejo_initialize_state, CATALEJO_INITIALIZE_STATE_INITIALIZED,
                                  memory_order_release);

            return CATALEJO_OUTCOME_SUCCESS;
        } else {
            // NOTE Preserve every action that was replaced before a later install failed.
            if (r0 == 0)
                sigaction(SIGSEGV, &saved_segmentation_violation_signal_actor, NULL);
            if (r1 == 0)
                sigaction(SIGBUS, &saved_bus_signal_actor, NULL);
            if (r2 == 0)
                sigaction(SIGILL, &saved_illegal_instruction_signal_actor, NULL);

            atomic_store_explicit(&catalejo_initialize_state, CATALEJO_INITIALIZE_STATE_FAILED,
                                  memory_order_release);

            return CATALEJO_OUTCOME_ERROR;
        }
    } else
        while (true) {
            int target_state =
                atomic_load_explicit(&catalejo_initialize_state, memory_order_acquire);

            switch (target_state) {
            case CATALEJO_INITIALIZE_STATE_INITIALIZED:
                return CATALEJO_OUTCOME_SUCCESS;
            case CATALEJO_INITIALIZE_STATE_FAILED:
                return CATALEJO_OUTCOME_ERROR;
            default:
                sched_yield();
            }
        }

    return CATALEJO_OUTCOME_SUCCESS;
}

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
    FAULT_ROUTINE catalejo_faultable_outcome_t                                                       \
        CATALEJO_CONCAT(catalejo_read_, target_typename)(CATALEJO_UNUSED const target_type *target_source, \
                                                         CATALEJO_UNUSED target_type *target_value)  \
    {                                                                                                \
        __asm__ volatile(                                                                            \
            /* %rdi = target_source, %rsi = target_value */                                          \
            /* NOTE: Forcefully clear the register for the read. */                                  \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"                              \
                                                                                                     \
            /* NOTE: Read the value. */                                                              \
            #target_mnemonic " (%rdi), %" #target_register "\n\t"                                    \
                                                                                                     \
            /* NOTE: Move read value to out-pointer. */                                              \
            #target_mnemonic " %" #target_register ", (%rsi)\n\t"                                    \
                                                                                                     \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */                   \
            "xorl %eax, %eax\n\t"                                                                    \
            "ret\n\t");                                                                              \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32)         \
    FAULT_ROUTINE catalejo_faultable_outcome_t                                                       \
        CATALEJO_CONCAT(catalejo_write_, target_typename)(CATALEJO_UNUSED target_type *target_value, \
                                                          CATALEJO_UNUSED const target_type *target_source) \
    {                                                                                                \
        __asm__ volatile(                                                                            \
            /* %rdi = target_value, %rsi = target_source */                                          \
            /* NOTE: Forcefully clear the register for the write. */                                 \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"                              \
                                                                                                     \
            /* NOTE: Read the value from the source into a register. */                              \
            #target_mnemonic " (%rsi), %" #target_register "\n\t"                                    \
                                                                                                     \
            /* NOTE: Write the value to the target address. */                                       \
            #target_mnemonic " %" #target_register ", (%rdi)\n\t"                                    \
                                                                                                     \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */                   \
            "xorl %eax, %eax\n\t"                                                                    \
            "ret\n\t");                                                                              \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_monitor_intel_arm(CATALEJO_UNUSED const uint8_t *target_address)
{
    __asm__ volatile(
        /* Move the local downstream address into the UMONITOR register. */
        "movq %rdi, %rax\n\t"

        /* UMONITOR %rax */
        ".byte 0xf3, 0x0f, 0xae, 0xf0\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t");
}

FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_monitor_intel_wait()
{
    __asm__ volatile(
        /* Build a bounded absolute TSC deadline in EDX and EAX. */
        "rdtsc\n\t"
        "addl $" CATALEJO_STR(CATALEJO_MONITOR_WAIT_CYCLES) ", %eax\n\t"
        "adcl $0, %edx\n\t"
        "xorl %ecx, %ecx\n\t"

        /* UMWAIT %ecx */
        ".byte 0xf2, 0x0f, 0xae, 0xf1\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t");
}

FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_monitor_amd_arm(CATALEJO_UNUSED const uint8_t *target_address)
{
    __asm__ volatile(
        /* MONITORX uses RAX for the local downstream address. */
        "movq %rdi, %rax\n\t"
        "xorl %ecx, %ecx\n\t"
        "xorl %edx, %edx\n\t"

        /* MONITORX */
        ".byte 0x0f, 0x01, 0xfa\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t");
}

FAULT_ROUTINE catalejo_faultable_instruction_outcome_t catalejo_monitor_amd_wait()
{
    __asm__ volatile(
        /* MWAITX receives a finite cycle count with timer operation enabled. */
        "xorl %eax, %eax\n\t"
        "movl $2, %ecx\n\t"
        "movl $" CATALEJO_STR(CATALEJO_MONITOR_WAIT_CYCLES) ", %edx\n\t"

        /* MWAITX */
        ".byte 0x0f, 0x01, 0xfb\n\t"

        /* Report success with no fault signal. */
        "xorl %eax, %eax\n\t"
        "xorl %edx, %edx\n\t"
        "ret\n\t");
}
// clang-format on

FAULT_ROUTINE catalejo_faultable_copy_outcome_t
catalejo_copy(CATALEJO_UNUSED uint8_t *target_address, CATALEJO_UNUSED const uint8_t *target_source,
              CATALEJO_UNUSED size_t target_count)
{
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
        ".global catalejo_copy_instruction_start\n\t"
        "catalejo_copy_instruction_start:\n\t"
        "rep movsb\n\t"
        ".global catalejo_copy_instruction_stop\n\t"
        "catalejo_copy_instruction_stop:\n\t"

        /* NOTE: Reached only on full success, where %rcx has drained to zero, we
           surface it as the (zero) remaining count in the second return value. */
        "movq %rcx, %rdx\n\t"

        /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */
        "xorl %eax, %eax\n\t"
        "ret\n\t");
}
