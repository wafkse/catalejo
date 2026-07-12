/**
 * Generic userspace-to-kernelspace multi-element transfers.
 */

#ifndef _MIRILLA_LIST_H_
#define _MIRILLA_LIST_H_

#include "mirilla-miscellaneous.h"

/**
 * Denote the actual type of the element managed by a `struct mirilla_outside_list`.
 */
#define MIRILLA_OUTSIDE_LIST_TYPE(_annotated_type)

/**
 * Do not attempt to populate the provided list.
 */
#define MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE (1U << 0)

/**
 * An attribute set to determine special behavior for `struct mirilla_outside_list` handling.
 */
typedef uint32_t mirilla_outside_list_attribute_t;

/**
 * A generic interface to handle an "outside list", an array for dynamically-sized structures to
 * be transfered from kernelspace to userspace efficiently.
 */
struct mirilla_outside_list {
    /**
     * The out-pointer to the list.
     *
     * This is a `void *`, but should be cast to the underlying type. Annotate with
     * `MIRILLA_OUTSIDE_LIST_TYPE` for cleanliness and to avoid confusion. It will be refered to as
     * a `backing-type` pseudo-type.
     *
     * This may never be `NULL`, unless list population is denegated via the `MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE` attribute.
     */
    virtual_address_t list_address;

    /**
     * The size of the allocation provided for the list address.
     *
     * This is counted in increments of `sizeof(backing-type)`.
     */
    uint32_t list_size;

    /**
     * The size of a singular element in the list.
     */
    uint32_t element_size;

    /**
     * The attribute set of the list.
     */
    mirilla_outside_list_attribute_t list_attribute;
};

/*+
 * An outcome to an operation performed to a `struct mirilla_outside_list`.
 *
 * This is an outcome struct, and dictates
 */
struct mirilla_outside_list_outcome {
    /**
     * The population count of the elements that were kernel-resident.
     *
     * This is counted in increments of `sizeof(backing-type)`.
     */
    uint32_t total_count;
};

#endif /* ifndef _MIRILLA_LIST_H_ */
