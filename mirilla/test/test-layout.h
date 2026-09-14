#ifndef _MIRILLA_TEST_LAYOUT_H_
#define _MIRILLA_TEST_LAYOUT_H_

#include <stdint.h>

#include "mirilla-miscellaneous.h"

/**
 * Numeric VMA provenance parsed from a `/proc/self/maps` entry.
 *
 * NOTE(invariant): `start_address` is lower than `end_address`; the remaining fields are copied
 * from the same mapping entry.
 */
struct proc_mapping {
    /** First virtual address in the mapping. */
    virtual_address_t start_address;

    /** First virtual address after the mapping. */
    virtual_address_t end_address;

    /** File offset reported for the mapping. */
    uint64_t file_offset;

    /** Major device number of the backing object. */
    uint32_t device_major;

    /** Minor device number of the backing object. */
    uint32_t device_minor;

    /** Inode number of the backing object. */
    uint64_t inode_number;
};

#endif /* _MIRILLA_TEST_LAYOUT_H_ */
