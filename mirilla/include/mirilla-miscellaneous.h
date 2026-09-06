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
 * An integer primitive capable of representing a forward offset applied to a virtual address.
 */
typedef virtual_address_t virtual_offset_t;

/**
 * An integer primitive capable of representing an address-relative displacement applied to a
 * virtual address.
 */
typedef int64_t virtual_relative_t;

/**
 * Apply a signed relative displacement to a virtual address.
 *
 * Converting the displacement to the unsigned address representation makes backward displacement
 * well-defined without risking signed overflow. As with ordinary virtual-address arithmetic, the
 * result wraps at the boundary of the virtual-address representation.
 */
static inline virtual_address_t virtual_relative_apply(virtual_address_t target_base,
                                                       virtual_relative_t target_displacement)
{
    return target_base + (virtual_address_t)target_displacement;
}

/**
 * Resolve a field-relative displacement to its virtual address.
 *
 * The displacement is interpreted relative to the address of its own field. This representation
 * permits position-independent records without dynamic pointer relocations.
 */
static inline virtual_address_t
virtual_relative_resolve(const virtual_relative_t *target_displacement)
{
    return virtual_relative_apply((virtual_address_t)(unsigned long)target_displacement,
                                  *target_displacement);
}

#endif
