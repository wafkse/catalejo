#ifndef _CATALEJO_MIRILLA_H_
#define _CATALEJO_MIRILLA_H_

#include "mirilla/mirilla-command.h" // IWYU pragma: export
#include "mirilla/mirilla-context.h" // IWYU pragma: export
#include "mirilla/mirilla-device.h" // IWYU pragma: export
#include "mirilla/mirilla-except.h" // IWYU pragma: export
#include "mirilla/mirilla-id.h" // IWYU pragma: export
#include "mirilla/mirilla-map.h" // IWYU pragma: export

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Engage a target process.
 *
 * @par Requirements
 * The provided descriptor must identify a Mirilla device session.
 */
mirilla_command_status_t catalejo_mirilla_engage(const int fd, const pid_t process_id,
                                                 mirilla_map_target_id_t *engage_id);

/**
 * Disengage from a target process.
 *
 * @par Requirements
 * The provided descriptor must identify a Mirilla device session.
 */
mirilla_command_status_t catalejo_mirilla_disengage(const int fd,
                                                    mirilla_map_target_id_t target_id);

/**
 * For an engaged target process, create a peephole at the specified virtual
 * memory range.
 *
 * @par Requirements
 * The provided descriptor must identify a Mirilla device session.
 */
mirilla_command_status_t
catalejo_mirilla_peephole(int fd, mirilla_map_target_id_t target_id,
                          virtual_address_t start_address, virtual_address_t end_address,
                          mirilla_map_peephole_initialize_word_t initialize_word,
                          mirilla_map_peephole_id_t *peephole_id, int *peephole_fd);

/**
 * For an engaged target process, retrieve the address space layout, the
 * kernel-resident auxiliary vector, and the argument/environment metadata.
 *
 * The caller describes each output buffer through a `mirilla_outside_list`. The kernel
 * fills up to its capacity and reports the total available entry count through the
 * matching `mirilla_outside_list_outcome`, allowing the caller to resize for another query.
 *
 * @par Requirements
 * The provided descriptor must identify a Mirilla device session.
 */
mirilla_command_status_t catalejo_mirilla_address_space_layout(
    int fd, mirilla_map_target_id_t target_id, struct mirilla_outside_list *layout_list,
    struct mirilla_outside_list *auxiliary_vector_list,
    struct mirilla_map_address_space_metadata *metadata,
    struct mirilla_outside_list_outcome *layout_outcome,
    struct mirilla_outside_list_outcome *auxiliary_vector_outcome);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _CATALEJO_MIRILLA_H_ */
