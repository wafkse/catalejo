#define _GNU_SOURCE

#include <stdlib.h>
#include <stddef.h>

#include <signal.h>
#include <stdint.h>
#include <sys/ucontext.h>
#include <ucontext.h>

#include "catalejo-signal.h"
#include "catalejo-section.h"

/**
 * The saved signal actions (`struct sigaction`) that existed before the catalejo-specific signal handler was setup.
 */
struct sigaction saved_segmentation_violation_signal_actor;
struct sigaction saved_bus_signal_actor;
struct sigaction saved_illegal_instruction_signal_actor;

/**
 * The primary signal handler for catalejo.
 */
void catalejo_signal_handle(int raised_signal, siginfo_t *signal_info, void *target_context)
{
    ucontext_t *userlevel_context = (ucontext_t *)target_context;

    mcontext_t *machine_context = &userlevel_context->uc_mcontext;

    uintptr_t target_address = machine_context->gregs[REG_RIP];

    if (target_address >= (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_START &&
        target_address < (uintptr_t)CATALEJO_FAULT_SECTION_BOUNDARY_STOP) {
        // NOTE: The shims inside the boundary establish no frame and touch no
        // stack or redzone, using only caller-saved registers. Hence the saved
        // `%rsp` still points exactly at the return address pushed by the `call`
        // that entered the shim, and that single word is all we must unwind.
        uintptr_t return_address = *(uintptr_t *)machine_context->gregs[REG_RSP];

        // NOTE: Simulate a `retq` instruction.
        machine_context->gregs[REG_RSP] += sizeof(uintptr_t);
        machine_context->gregs[REG_RIP] = return_address;

        // NOTE: Override the `%rax` register, as it is used for the first return value as per the System V ABI.
        machine_context->gregs[REG_RAX] = CATALEJO_OUTCOME_ERROR;

        // NOTE: Preserve the copy counter only while the string instruction is active.
        // Other protected routines receive the raised signal in the second return register.
        if (target_address >= (uintptr_t)catalejo_copy_instruction_start &&
            target_address < (uintptr_t)catalejo_copy_instruction_stop)
            machine_context->gregs[REG_RDX] = machine_context->gregs[REG_RCX];
        else
            machine_context->gregs[REG_RDX] = raised_signal;

        return;
    }

    // NOTE: Not in our memory range, handle signal as normal.

    struct sigaction *saved_signal_actor = NULL;

    switch (raised_signal) {
    case SIGSEGV:
        saved_signal_actor = &saved_segmentation_violation_signal_actor;

        break;
    case SIGBUS:
        saved_signal_actor = &saved_bus_signal_actor;

        break;
    case SIGILL:
        saved_signal_actor = &saved_illegal_instruction_signal_actor;

        break;
    default:
        // NOTE(abort): Somehow handled signal that we did not register for.
        abort();
    }

    if (saved_signal_actor->sa_flags & SA_SIGINFO)
        saved_signal_actor->sa_sigaction(raised_signal, signal_info, target_context);
    else if (saved_signal_actor->sa_handler == SIG_DFL) {
        // NOTE: This is the OS-level default handler. We need to unregister ourselves and re-raise the same signal.
        signal(raised_signal, SIG_DFL);

        // NOTE: This signal handler is setup in `SA_NODEFER` mode.
        raise(raised_signal);
    } else if (saved_signal_actor->sa_handler != SIG_IGN)
        saved_signal_actor->sa_handler(raised_signal);
}
