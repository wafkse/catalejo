/**
 * Miscellaneous macros for the mirilla kernel module.
 */

#ifndef _MIRILLA_MISCELLANEOUS_H
#define _MIRILLA_MISCELLANEOUS_H

#ifdef __KERNEL__

#include <linux/types.h>

#else

#include <stdint.h>

#endif

/**
 * An integer primitive capable of representing a virtual address.
 */
typedef uint64_t virtual_address_t;

/**
 * An integer primitive capable of representing the size of a virtual address region.
 */
typedef virtual_address_t virtual_size_t;

/**
 * An integer primitive capable of representing the alignment of a virtual address.
 */
typedef virtual_address_t virtual_align_t;

/**
 * An integer primitive capable of representing an offset applied to a virtual address.
 */
typedef virtual_address_t virtual_offset_t;

#endif
