/*
 * Basic command definitions
 */

#ifndef _MIRILLA_COMMAND_H_
#define _MIRILLA_COMMAND_H_

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

#define MIRILLA_COMMAND_ENCODE(category, enumeration)                                            \
	((MIRILLA_IOCTL_MAGIC << MIRILLA_IOCTL_MAGIC_OFFSET) |                                  \
	 ((category &                                                                             \
	   (MIRILLA_IOCTL_COMMAND_CATEGORY_MASK >> MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET))      \
	  << MIRILLA_IOCTL_COMMAND_CATEGORY_OFFSET) |                                            \
	 (enumeration &                                                                           \
	  (MIRILLA_IOCTL_COMMAND_ENUMERATION_MASK >> MIRILLA_IOCTL_COMMAND_ENUMERATION_OFFSET)) \
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

#endif
