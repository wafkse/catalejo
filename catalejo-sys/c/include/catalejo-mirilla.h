#ifndef _CATALEJO_MIRILLA_H_
#define _CATALEJO_MIRILLA_H_

#include "mirilla/mirilla-command.h" // IWYU pragma: export
#include "mirilla/mirilla-context.h" // IWYU pragma: export
#include "mirilla/mirilla-device.h"  // IWYU pragma: export
#include "mirilla/mirilla-id.h"      // IWYU pragma: export
#include "mirilla/mirilla-map.h"     // IWYU pragma: export

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Engage a target process.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t
catalejo_mirilla_engage(const int fd, const pid_t process_id,
                         mirilla_map_target_id_t *engage_id);

/**
 * Disengage from a target process.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t
catalejo_mirilla_disengage(const int fd, mirilla_map_target_id_t target_id);

/**
 * For an engaged target process, create a peephole at the specified virtual
 * memory range.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t
catalejo_mirilla_peephole(int fd, mirilla_map_target_id_t target_id,
                           uint64_t start_address, uint64_t end_address,
                           mirilla_map_peephole_id_t *peephole_id,
                           int *peephole_fd);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _CATALEJO_MIRILLA_H_ */
