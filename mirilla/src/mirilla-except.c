#include <asm/vdso.h>

#include <linux/ftrace.h>
#include <linux/panic.h>
#include <linux/sched.h>
#include <linux/stringify.h>
#include <linux/uaccess.h>

#include "mirilla-except.h"
#include "mirilla-log.h"

/* Apply a checked exception action to a saved userspace register frame. */
notrace bool mirilla_except_action_apply(struct pt_regs *user_registers,
                                         const struct mirilla_except_action *action)
{
    bool frame_present = user_registers != NULL;
    bool action_present = action != NULL;

    if (!frame_present || !action_present)
        return false;

    switch (action->tag) {
    case MIRILLA_EXCEPT_ACTION_NONE:
        return false;

    case MIRILLA_EXCEPT_ACTION_IP: {
        virtual_address_t target_instruction_pointer = action->context.ip.address;
        bool address_present = target_instruction_pointer != 0;
        bool address_accessible =
            access_ok((void __user *)(unsigned long)target_instruction_pointer, 1);

        if (!address_present || !address_accessible)
            return false;

        user_registers->ip = target_instruction_pointer;

        return true;
    }

    case MIRILLA_EXCEPT_ACTION_RETRY:
        return true;

    default:
        return false;
    }
}

/* Find and apply a recovery action for one userspace trap. */
static notrace bool mirilla_except_try_fixup(struct pt_regs *user_registers, int trap_index,
                                             unsigned long error_code)
{
    struct mm_struct *address_space = current->mm;
    mirilla_except_mask_t except_mask;
    struct mirilla_except_action action;
    bool frame_present, userspace_frame, address_space_present;
    bool fixup_possible, vector_nonnegative, vector_in_range;
    bool vector_supported, action_found;

    frame_present = user_registers != NULL;
    userspace_frame = frame_present && user_mode(user_registers);
    address_space_present = address_space != NULL;

    fixup_possible = frame_present && userspace_frame && address_space_present;
    if (!fixup_possible)
        return false;

    vector_nonnegative = trap_index >= 0;
    vector_in_range = trap_index < MIRILLA_EXCEPT_VECTOR_LIMIT;
    if (!vector_nonnegative || !vector_in_range)
        return false;

    except_mask = MIRILLA_EXCEPT_MASK(trap_index);

    vector_supported = except_mask & MIRILLA_EXCEPT_X86_SUPPORTED_MASK;
    action_found = vector_supported && mirilla_except_lookup(address_space, user_registers->ip,
                                                             except_mask, error_code, &action);

    if (!action_found)
        return false;

    return mirilla_except_action_apply(user_registers, &action);
}

/* Return the handled result expected by callers of fixup_vdso_exception. */
static notrace bool mirilla_except_handled(void)
{
    return true;
}

/*
 * Redirect the native exception-fixup hook to immutable slab lookup.
 *
 * NOTE(ordering): A miss leaves fixup_vdso_exception and every later trap-specific recovery path
 * untouched. A match returns handled from this call site and takes precedence over later
 * recovery, including the supported kernel's post-vDSO general-protection fixups.
 */
static notrace void mirilla_except_ftrace(unsigned long instruction_pointer,
                                          unsigned long parent_instruction_pointer,
                                          struct ftrace_ops *operations,
                                          struct ftrace_regs *ftrace_registers)
{
    struct pt_regs *user_registers;
    int trap_number;

    if (!ftrace_regs_has_args(ftrace_registers))
        return;

    user_registers = (struct pt_regs *)ftrace_regs_get_argument(ftrace_registers, 0);
    trap_number = (int)ftrace_regs_get_argument(ftrace_registers, 1);
    if (!mirilla_except_try_fixup(user_registers, trap_number,
                                  ftrace_regs_get_argument(ftrace_registers, 2)))
        return;

    ftrace_regs_set_instruction_pointer(ftrace_registers, (unsigned long)mirilla_except_handled);
}

/* Configure and register the module-lifetime ftrace exception hook. */
static int mirilla_except_backend_initialize(void)
{
    struct ftrace_ops *operations = &mirilla_except_context.ftrace_operations;
    unsigned char target_name[] = __stringify(fixup_vdso_exception);
    int error_code;

    *operations = (struct ftrace_ops){
        .func = mirilla_except_ftrace,
        .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY | FTRACE_OPS_FL_RECURSION |
                 FTRACE_OPS_FL_RCU | FTRACE_OPS_FL_PERMANENT,
    };

    error_code = ftrace_set_filter(operations, target_name, sizeof(target_name) - 1, 0);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to configure exception ftrace filter");

    error_code = register_ftrace_function(operations);
    if (error_code) {
        ftrace_free_filter(operations);

        MIRILLA_ERROR_AND_RETURN(error_code, "failed to register exception ftrace hook");
    }

    return MIRILLA_COMMAND_OK;
}

/* Remove the module-lifetime ftrace hook and its filter. */
static void mirilla_except_backend_deinitialize(void)
{
    struct ftrace_ops *operations = &mirilla_except_context.ftrace_operations;
    /*
     * NOTE(invariant): Module text must never be released while ftrace can still branch into it.
     * An unregister failure is therefore a kernel invariant violation, not a recoverable teardown
     * error.
     */
    if (unregister_ftrace_function(operations))
        panic("ftrace callback unregister failed");

    ftrace_free_filter(operations);
}

/* Initialize registry state and install the exception hook. */
int mirilla_except_initialize(void)
{
    int error_code;

    if ((error_code = mirilla_except_registry_initialize()))
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to initialize exception registry");

    if ((error_code = mirilla_except_backend_initialize())) {
        mirilla_except_registry_deinitialize();

        MIRILLA_ERROR_AND_RETURN(error_code, "failed to initialize exception backend");
    }

    return MIRILLA_COMMAND_OK;
}

/* Remove the exception hook and verify registry teardown. */
void mirilla_except_deinitialize(void)
{
    mirilla_except_backend_deinitialize();
    mirilla_except_registry_deinitialize();
}
