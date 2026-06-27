#ifndef _CATALEJO_SIGNAL_H_
#define _CATALEJO_SIGNAL_H_

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The saved signal actions (`struct sigaction`) that existed before the
 * catalejo-specific signal handler was setup.
 */
extern struct sigaction saved_segmentation_violation_signal_actor;
extern struct sigaction saved_bus_signal_actor;

#define CATALEJO_FAULTABLE_TYPES_X                                             \
  X(u64, movq, rcx)                                                            \
  X(u32, movl, ecx)                                                            \
  X(u16, movw, cx)                                                             \
  X(u8, movb, cl)

/**
 * The primary signal handler for catalejo.
 */
void catalejo_signal_handle(int raised_signal, siginfo_t *signal_info,
                            void *target_context);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef _CATALEJO_SIGNAL_H_ */
