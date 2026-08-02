#define _GNU_SOURCE

#include <stdlib.h>
#include <stddef.h>

#include <signal.h>
#include <stdint.h>
#include <sys/ucontext.h>
#include <ucontext.h>

#include "catalejo-fixup.h"
#include "catalejo-signal.h"

/**
 * The saved signal actions (`struct sigaction`) that existed before the catalejo-specific signal handler was setup.
 */
struct sigaction saved_segmentation_violation_signal_actor;
struct sigaction saved_bus_signal_actor;
struct sigaction saved_illegal_instruction_signal_actor;

/**
 * Convert a delivered signal number to the stable mask used by rollback records.
 */
static uint64_t catalejo_signal_mask(int raised_signal)
{
    switch (raised_signal) {
    case SIGSEGV:
        return CATALEJO_FAULT_SIGNAL_SEGV;
    case SIGBUS:
        return CATALEJO_FAULT_SIGNAL_BUS;
    case SIGILL:
        return CATALEJO_FAULT_SIGNAL_ILL;
    default:
        return 0;
    }
}

/**
 * Resolve a signed field-relative pointer from a rollback record.
 */
static uintptr_t catalejo_fixup_address(const relative_pointer_t *target_value)
{
    return (uintptr_t)target_value + (uintptr_t)*target_value;
}

/**
 * Find the rollback record that accepts a signal at the supplied instruction pointer.
 */
static const struct catalejo_rollback_record *catalejo_find_rollback(uintptr_t target_address,
                                                                     uint64_t signal_mask)
{
    uintptr_t target_record = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START;
    uintptr_t target_stop = (uintptr_t)CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP;

    while (target_record < target_stop &&
           target_stop - target_record >= sizeof(struct catalejo_rollback_record)) {
        const struct catalejo_rollback_record *rollback_record =
            (const struct catalejo_rollback_record *)target_record;
        uintptr_t start_address = catalejo_fixup_address(&rollback_record->start_address);
        uintptr_t end_address = catalejo_fixup_address(&rollback_record->end_address);

        if (end_address >= start_address && target_address >= start_address &&
            target_address < end_address && (rollback_record->signal_mask & signal_mask) != 0)
            return rollback_record;

        target_record += sizeof(struct catalejo_rollback_record);
    }

    return NULL;
}

/**
 * The primary signal handler for catalejo.
 */
void catalejo_signal_handle(int raised_signal, siginfo_t *signal_info, void *target_context)
{
    ucontext_t *userlevel_context = (ucontext_t *)target_context;
    mcontext_t *machine_context = &userlevel_context->uc_mcontext;
    uintptr_t target_address = machine_context->gregs[REG_RIP];

    const struct catalejo_rollback_record *rollback_record =
        catalejo_find_rollback(target_address, catalejo_signal_mask(raised_signal));

    if (rollback_record != NULL) {
        // NOTE: Rollback routines execute in the interrupted register state and return through the
        // original naked routine's call frame. The second return register carries the actual signal
        // unless the selected rollback routine deliberately replaces it.
        machine_context->gregs[REG_RDX] = raised_signal;
        machine_context->gregs[REG_RIP] =
            catalejo_fixup_address(&rollback_record->rollback_routine);

        return;
    }

    // NOTE: No rollback record accepted this signal, so handle it as normal.

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
