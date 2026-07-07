#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <signal.h>
#include <sched.h>

#include "catalejo-macro.h"
#include "catalejo-section.h"
#include "catalejo-fault.h"
#include "catalejo-signal.h"

static atomic_int catalejo_initialize_state = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;

/**
 * Initialize the catalejo-fault environment.
 *
 * This attempts to initialize the environment in which the fault-handling semantics are to be performed.
 *
 * Particularly, this sets up the signal handlers in a lazy yet thread-safe manner.
 */
catalejo_faultable_outcome_t catalejo_fault_initialize() {
    if(atomic_load_explicit(&catalejo_initialize_state, memory_order_acquire) == CATALEJO_INITIALIZE_STATE_INITIALIZED)
        return CATALEJO_OUTCOME_SUCCESS;

   int expected_state = CATALEJO_INITIALIZE_STATE_UNINITIALIZED;

   if (atomic_compare_exchange_strong_explicit(&catalejo_initialize_state, &expected_state, CATALEJO_INITIALIZE_STATE_INITIALIZING, memory_order_acq_rel, memory_order_acquire)) {
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
            // * SA_ONSTACK: the fault path itself needs no alternate stack -- the
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

        if (r0 == 0 && r1 == 0) {
            atomic_store_explicit(&catalejo_initialize_state, CATALEJO_INITIALIZE_STATE_INITIALIZED, memory_order_release);

            return CATALEJO_OUTCOME_SUCCESS;
        } else {
            atomic_store_explicit(&catalejo_initialize_state, CATALEJO_INITIALIZE_STATE_FAILED, memory_order_release);

            return CATALEJO_OUTCOME_ERROR;
        }
   } else
       while (true)
       {
           int target_state = atomic_load_explicit(&catalejo_initialize_state, memory_order_acquire);

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

#define X(target_typename, target_type, target_mnemonic, target_register,      \
          target_register32) \
    FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(catalejo_read_, target_typename) \
                           (CATALEJO_UNUSED const target_type *target_source,         \
                            CATALEJO_UNUSED target_type *target_value) { \
        __asm__ volatile( \
            /* %rdi = target_source, %rsi = target_value */ \
            /* NOTE: Forcefully clear the register for the read. */ \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"  \
            \
            /* NOTE: Read the value. */ \
            #target_mnemonic " (%rdi), %" #target_register "\n\t" \
            \
            /* NOTE: Move read value to out-pointer. */ \
            #target_mnemonic " %" #target_register ", (%rsi)\n\t" \
            \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */ \
            "xorl %eax, %eax\n\t" \
            "ret\n\t" \
        ); \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register,      \
          target_register32) \
    FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(catalejo_write_, target_typename) \
                          (CATALEJO_UNUSED target_type * target_value,             \
                           CATALEJO_UNUSED const target_type *target_source) { \
        __asm__ volatile( \
            /* %rdi = target_value, %rsi = target_source */ \
            /* NOTE: Forcefully clear the register for the write. */ \
            "xorl %" #target_register32 ", %" #target_register32 "\n\t"  \
            \
            /* NOTE: Read the value from the source into a register. */ \
            #target_mnemonic " (%rsi), %" #target_register "\n\t" \
            \
            /* NOTE: Write the value to the target address. */ \
            #target_mnemonic " %" #target_register ", (%rdi)\n\t" \
            \
            /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */ \
            "xorl %eax, %eax\n\t" \
            "ret\n\t" \
        ); \
    }

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

FAULT_ROUTINE catalejo_faultable_copy_outcome_t catalejo_copy(CATALEJO_UNUSED uint8_t *target_destination,
                                                                CATALEJO_UNUSED const uint8_t *target_source,
                                                                CATALEJO_UNUSED size_t target_count) {
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
        "rep movsb\n\t"

        /* NOTE: Reached only on full success, where %rcx has drained to zero, we
           surface it as the (zero) remaining count in the second return value. */
        "movq %rcx, %rdx\n\t"

        /* NOTE: Signal a successful outcome (CATALEJO_OUTCOME_SUCCESS = 0) */
        "xorl %eax, %eax\n\t"
        "ret\n\t"
    );
}
