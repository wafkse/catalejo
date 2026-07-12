/*
 * Monotonic identifiers for structure identification.
 */

#ifndef _MIRILLA_ID_H_
#define _MIRILLA_ID_H_

#include "mirilla-miscellaneous.h" // IWYU pragma: export

#define MIRILLA_ID_TYPE_FORMAT_STRING "id(%lu)"

#define MIRILLA_ID_TYPE_NAME uint32_t

#define MIRILLA_ID_NONE ((mirilla_id_t)0)

/*
 * A generic monotonic identifier type.
 *
 * NOTE(invariant): These must never be zero.
 */
typedef MIRILLA_ID_TYPE_NAME mirilla_id_t;

#ifdef __KERNEL__

#include <linux/atomic.h>

/*
 * Atomic type with the same bits as an identifier.
 *
 * NOTE(invariant): These must never be zero.
 */
typedef atomic_t mirilla_atomic_id_t;

static_assert(sizeof(mirilla_id_t) == sizeof(mirilla_atomic_id_t), "size missmatch between "
                                                                   "atomic and non-atomic "
                                                                   "integer ID primitive");

#endif /* __KERNEL__ */

#endif
