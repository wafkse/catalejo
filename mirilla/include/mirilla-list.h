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
 * A userspace output buffer for a variable-length kernel result.
 *
 * The caller supplies storage capacity and element size. The kernel fills the available
 * prefix and reports the total entry count separately, allowing a sizing query followed
 * by a larger allocation when needed.
 */
struct mirilla_outside_list {
    /**
     * Userspace address of the output buffer.
     *
     * @par Representation
     * The ABI carries an integer address. `MIRILLA_OUTSIDE_LIST_TYPE` annotates
     * the element type expected by a particular command.
     *
     * @par Constraints
     * The address may be zero only when population is disabled through
     * `MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE`.
     */
    virtual_address_t list_address;

    /**
     * Output buffer capacity in elements, each occupying `element_size` bytes.
     */
    uint32_t list_size;

    /**
     * Byte size of each output element.
     */
    uint32_t element_size;

    /**
     * The attribute set of the list.
     */
    mirilla_outside_list_attribute_t list_attribute;
};

/**
 * Entry count available to an outside-list query.
 *
 * @par Notes
 * The count includes entries that did not fit in the supplied buffer. Only the
 * prefix bounded by both this count and the buffer capacity has been populated.
 */
struct mirilla_outside_list_outcome {
    /**
     * Total available element count, independent of the output buffer capacity.
     */
    uint32_t total_count;
};

#endif /* ifndef _MIRILLA_LIST_H_ */
