/*
 * Basic command definitions
 */

#ifndef _MIRILLA_COMMAND_H_
#define _MIRILLA_COMMAND_H_

#include "mirilla-miscellaneous.h" // IWYU pragma: export

/*
 * Mirilla ioctl magic number - chosen to avoid conflicts with standard ioctls.
 * Using 'O' (0x4F) as the magic number.
 */
#define MIRILLA_IOCTL_MAGIC 'O'

/*
 * Command encoding does not follow kernel conventions due to difficulties
 * interfacing with Rust.
 */
#define MIRILLA_IOCTL_COMMAND_ENUMERATION_MASK 0x0000ffff
#define MIRILLA_IOCTL_COMMAND_CATEGORY_MASK 0x00ff0000
#define MIRILLA_IOCTL_MAGIC_MASK 0xff000000

#define MIRILLA_IOCTL_COMMAND_ENUMERATION_OFFSET (0)
#define MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET (16)
#define MIRILLA_IOCTL_MAGIC_OFFSET (24)

#define MIRILLA_COMMAND_ENCODE(category, enumeration)                                             \
    ((MIRILLA_IOCTL_MAGIC << MIRILLA_IOCTL_MAGIC_OFFSET) |                                        \
     ((category & (MIRILLA_IOCTL_COMMAND_CATEGORY_MASK >> MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET)) \
      << MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET) |                                                 \
     (enumeration &                                                                               \
      (MIRILLA_IOCTL_COMMAND_ENUMERATION_MASK >> MIRILLA_IOCTL_COMMAND_ENUMERATION_OFFSET))       \
         << MIRILLA_IOCTL_COMMAND_ENUMERATION_OFFSET)

#define MIRILLA_COMMAND_CATEGORY(command) \
    ((command & MIRILLA_IOCTL_COMMAND_CATEGORY_MASK) >> MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET)
#define MIRILLA_COMMAND_ENUMERATION(command) (command & MIRILLA_IOCTL_COMMAND_ENUMERATION_MASK)
#define MIRILLA_COMMAND_MAGIC(command) \
    ((command & MIRILLA_IOCTL_MAGIC_MASK) >> MIRILLA_IOCTL_MAGIC_OFFSET)

/*
 * A successful command status.
 *
 * This is returned when the `ioctl` has been completed successfully.
 */
#define MIRILLA_COMMAND_OK 0

/*
 * Determine whether a command result indicated success.
 */
#define MIRILLA_COMMAND_IS_OK(command_code) ((command_code) == MIRILLA_COMMAND_OK)

typedef unsigned int mirilla_command_t;

typedef int mirilla_command_status_t;

typedef unsigned long mirilla_command_argument_t;

#ifdef __KERNEL__

#include <linux/fdtable.h>
#include <linux/file.h>

/*
 * A reserved descriptor and retained file awaiting atomic installation.
 *
 * NOTE(invariant): A reservation owns one file reference and one descriptor number. Installation
 * transfers the file reference to the descriptor table. Lexical cleanup releases both resources
 * when result copying or any earlier operation fails.
 */
struct mirilla_fd_reservation {
    /** File reference held until the command result has been copied. */
    struct file *target_file;
    /** Reserved descriptor number, or a negative value when empty. */
    int file_descriptor;
};

/** Release a pending descriptor reservation during lexical cleanup. */
static inline void mirilla_fd_reservation_cleanup(struct mirilla_fd_reservation *reservation)
{
    if (reservation->file_descriptor >= 0)
        put_unused_fd(reservation->file_descriptor);

    if (reservation->target_file)
        fput(reservation->target_file);

    reservation->target_file = NULL;
    reservation->file_descriptor = -1;
}

/** Attach descriptor-reservation cleanup to a local variable. */
#define MIRILLA_FD_RESERVATION MIRILLA_CLEANUP(mirilla_fd_reservation_cleanup)

/**
 * Reserve a descriptor while retaining file ownership in the reservation.
 *
 * The reservation consumes target_file even when descriptor allocation fails. Lexical cleanup
 * releases that file and any successful descriptor reservation unless installation consumes them.
 */
static inline int mirilla_fd_reservation_prepare(struct mirilla_fd_reservation *reservation,
                                                 struct file *target_file, unsigned int file_flags)
{
    int file_descriptor = get_unused_fd_flags(file_flags);

    reservation->target_file = target_file;
    reservation->file_descriptor = file_descriptor;

    return file_descriptor;
}

/**
 * Install a retained file and consume its descriptor reservation.
 *
 * The caller must invoke this only after successful preparation and result publication to
 * userspace.
 */
static inline void mirilla_fd_reservation_install(struct mirilla_fd_reservation *reservation)
{
    struct file *target_file = reservation->target_file;
    int file_descriptor = reservation->file_descriptor;

    reservation->target_file = NULL;
    reservation->file_descriptor = -1;
    fd_install(file_descriptor, target_file);
}

/*
 * NOTE(invariant): Avoid multi-page `copy_{to,from}_user` for input-output
 * intermediate structures.
 */
/** Assert that a generated command buffer fits within one kernel page. */
#define MIRILLA_ASSERT_IO_SIZE(name) \
    MIRILLA_ASSERT(MIRILLA_SIZEOF(union mirilla_##name##_io) <= PAGE_SIZE, "IO too large: " #name)

#else

/*
 * NOTE(invariant): The command buffer page bound is a kernel implementation constraint.
 */
/** Omit the kernel-only command-buffer assertion from userspace headers. */
#define MIRILLA_ASSERT_IO_SIZE(name)

#endif /* __KERNEL__ */

#endif
