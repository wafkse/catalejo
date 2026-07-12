#define _GNU_SOURCE

#include <errno.h>
#include <sys/ioctl.h>

#include "catalejo-mirilla.h"

/**
 * Engage a target process.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t catalejo_mirilla_engage(
    const int fd, const pid_t process_id, mirilla_map_target_id_t *engage_id
) {
    mirilla_command_status_t command_code = MIRILLA_COMMAND_OK;

    union mirilla_map_engage_io io;

    io.argument = (struct mirilla_map_engage_argument){ process_id };

    command_code = ioctl(fd, MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_ENGAGE), &io);

    // NOTE: glibc collapsed the kernel's negative `-errno` status into -1 and
    // left the real code in the thread-local `errno`, we recover it so the caller
    // receives the full status without having to read `errno` itself.
    if (command_code < 0)
        return -errno;

    if (!MIRILLA_COMMAND_IS_OK(command_code))
        return command_code;

    *engage_id = io.result.target_id;

    return command_code;
}


/**
 * Disengage from a target process.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t catalejo_mirilla_disengage(
    const int fd, mirilla_map_target_id_t target_id
) {
    mirilla_command_status_t command_code = MIRILLA_COMMAND_OK;

    union mirilla_map_disengage_io io;

    io.argument = (struct mirilla_map_disengage_argument){ target_id };

    command_code = ioctl(fd, MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_DISENGAGE), &io);

    if (command_code < 0)
        return -errno;

    return command_code;
}

/**
 * For an engaged target process, create a peephole at the specified virtual memory range.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t catalejo_mirilla_peephole(
    int fd, mirilla_map_target_id_t target_id, virtual_address_t start_address, virtual_address_t end_address, mirilla_map_peephole_initialize_word_t initialize_word, mirilla_map_peephole_id_t *peephole_id, int *peephole_fd
) {
    mirilla_command_status_t command_code = MIRILLA_COMMAND_OK;

    union mirilla_map_peephole_io io;

    io.argument = (struct mirilla_map_peephole_argument){
        .target_id = target_id,
        .start_address = start_address,
        .end_address = end_address,
        .initialize_word = initialize_word,
    };

    command_code = ioctl(fd, MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_PEEPHOLE), &io);

    if (command_code < 0)
        return -errno;

    if (!MIRILLA_COMMAND_IS_OK(command_code))
        return command_code;

    *peephole_id = io.result.id;
    *peephole_fd = io.result.fd;

    return command_code;
}

/**
 * For an engaged target process, retrieve the address space layout, the
 * kernel-resident auxiliary vector, and the argument/environment metadata.
 *
 * The provided file descriptor must be of the `mirilla` kernel module.
 */
mirilla_command_status_t catalejo_mirilla_address_space_layout(
    int fd, mirilla_map_target_id_t target_id,
    struct mirilla_outside_list *layout_list,
    struct mirilla_outside_list *auxiliary_vector_list,
    struct mirilla_map_address_space_metadata *metadata,
    struct mirilla_outside_list_outcome *layout_outcome,
    struct mirilla_outside_list_outcome *auxiliary_vector_outcome
) {
    mirilla_command_status_t command_code = MIRILLA_COMMAND_OK;

    union mirilla_map_address_space_layout_io io;

    io.argument = (struct mirilla_map_address_space_layout_argument){
        .target_id = target_id,
        .layout_list = *layout_list,
        .auxiliary_vector_list = *auxiliary_vector_list,
    };

    command_code = ioctl(fd, MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP, MIRILLA_COMMAND_MAP_ADDRESS_SPACE_LAYOUT), &io);

    if (command_code < 0)
        return -errno;

    if (!MIRILLA_COMMAND_IS_OK(command_code))
        return command_code;

    *metadata = io.result.metadata;
    *layout_outcome = io.result.layout_outcome;
    *auxiliary_vector_outcome = io.result.auxiliary_vector_outcome;

    return command_code;
}
