#include <asm/trapnr.h>
#include <asm/vdso.h>

#include <linux/ftrace.h>
#include <linux/sched.h>
#include <linux/stringify.h>

#include "mirilla-except.h"

/* Apply one registered userspace exception rollback. */
static notrace bool mirilla_except_try_fixup(struct pt_regs *user_registers, int trap_indice)
{
    struct mm_struct *mm = current->mm;
    mirilla_except_mask_t except_mask;
    unsigned long rollback_address;

    if (!user_registers || !user_mode(user_registers) || !mm)
        return false;

    if (trap_indice < 0 || trap_indice >= MIRILLA_EXCEPT_VECTOR_LIMIT)
        return false;

    except_mask = MIRILLA_EXCEPT_MASK(trap_indice);

    if (!mirilla_except_lookup(mm, user_registers->ip, except_mask, &rollback_address))
        return false;

    user_registers->ip = rollback_address;
    user_registers->dx = (unsigned long)trap_indice;

    return true;
}

/* Return the handled result expected by callers of fixup_vdso_exception(). */
static notrace bool mirilla_except_handled(void)
{
    return true;
}

/* Intercept the native vDSO fixup choke point for registered protected instructions. */
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

    if (!mirilla_except_try_fixup(user_registers, trap_number))
        return;

    ftrace_regs_set_instruction_pointer(ftrace_registers, (unsigned long)mirilla_except_handled);
}

/* Permanent IP-modifying hook installed on the x86 userspace exception-fixup choke point. */
static struct ftrace_ops mirilla_except_ftrace_operations = {
    .func = mirilla_except_ftrace,
    .flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY | FTRACE_OPS_FL_RECURSION |
             FTRACE_OPS_FL_RCU | FTRACE_OPS_FL_PERMANENT,
};

int mirilla_except_initialize(void)
{
    int initialize_status;

    unsigned char target_name[] = __stringify(fixup_vdso_exception);

    /*
     * The target is declared by <asm/vdso.h> but is not exported to modules, so ftrace's
     * name-filter interface is deliberately used instead of creating a forbidden symbol relocation.
     */
    initialize_status = ftrace_set_filter(&mirilla_except_ftrace_operations, target_name,
                                          sizeof(target_name) - 1, 0);
    if (initialize_status)
        return initialize_status;

    initialize_status = register_ftrace_function(&mirilla_except_ftrace_operations);
    if (initialize_status)
        ftrace_set_filter(&mirilla_except_ftrace_operations, NULL, 0, 1);

    return initialize_status;
}

void mirilla_except_deinitialize(void)
{
    unregister_ftrace_function(&mirilla_except_ftrace_operations);
    ftrace_set_filter(&mirilla_except_ftrace_operations, NULL, 0, 1);
}
