/**
 * Miscellaneous macros for the mirilla kernel module.
 */

#ifndef _MIRILLA_MISCELLANEOUS_H
#define _MIRILLA_MISCELLANEOUS_H

#ifdef __KERNEL__

#include <linux/stddef.h>
#include <linux/types.h>

#else

#include <stddef.h>
#include <stdint.h>

#endif

/*
 * Language-independent compile-time helpers used by Mirilla ABI headers.
 *
 * NOTE(invariant): Kernel and C++ consumers use `static_assert`, C consumers use
 * `_Static_assert`. Bindgen sees no assertion declaration. Size, alignment, and offset queries
 * retain their native language semantics behind the same Mirilla spelling.
 */
#if defined(__BINDGEN__)
/** Omit a native compile-time assertion while generating Rust bindings. */
#define MIRILLA_ASSERT(target_condition, target_message)
#elif defined(__KERNEL__) || defined(__cplusplus)
/** Assert one ABI property with the native kernel or C++ spelling. */
#define MIRILLA_ASSERT(target_condition, target_message) \
    static_assert(target_condition, target_message)
#else
/** Assert one ABI property with the native C spelling. */
#define MIRILLA_ASSERT(target_condition, target_message) \
    _Static_assert(target_condition, target_message)
#endif

/*
 * Source-level warning for ABI fields that callers should not consume.
 *
 * Unstable fields may change meaning or representation without becoming stable ABI. GCC and
 * Clang warn when C or C++ source names these fields. Bindgen accepts the attribute but does not
 * currently propagate field deprecation to generated Rust bindings.
 */
/** Mark a source-visible ABI field whose representation is not stable. */
#define MIRILLA_UNSTABLE_FIELD \
    __attribute__((deprecated("unstable ABI field, do not depend on it")))

/** Apply packed C layout to an ABI declaration. */
#define MIRILLA_PACKED __attribute__((packed))
/** Apply an explicit byte alignment to an ABI declaration. */
#define MIRILLA_ALIGNED(target_alignment) __attribute__((aligned(target_alignment)))

/*
 * Lexical ownership helpers for native C resource variables.
 *
 * NOTE(invariant): A resource cleanup observes either its declared empty value or exactly one
 * owned resource. Taking a resource writes the empty value before returning ownership to the
 * caller. Dropping a resource invokes the same cleanup operation immediately.
 */
/** Attach a lexical cleanup function to a local C variable. */
#define MIRILLA_CLEANUP(function) __attribute__((cleanup(function)))

/** Select the generated cleanup function for one resource kind. */
#define MIRILLA_RESOURCE(name) MIRILLA_CLEANUP(mirilla_resource_cleanup_##name)

/** Define held, cleanup, take, and drop helpers for one resource kind. */
#define MIRILLA_RESOURCE_DEFINE(name, type, empty, held_expression, release_expression) \
    static inline int mirilla_resource_held_##name(type resource)                       \
    {                                                                                   \
        return (held_expression);                                                       \
    }                                                                                   \
                                                                                        \
    static inline void mirilla_resource_cleanup_##name(type *resource_storage)          \
    {                                                                                   \
        type resource = *resource_storage;                                              \
                                                                                        \
        if (mirilla_resource_held_##name(resource)) {                                   \
            release_expression;                                                         \
            *resource_storage = (empty);                                                \
        }                                                                               \
    }                                                                                   \
                                                                                        \
    static inline type mirilla_resource_take_##name(type *resource_storage)             \
    {                                                                                   \
        type resource = *resource_storage;                                              \
                                                                                        \
        *resource_storage = (empty);                                                    \
                                                                                        \
        return resource;                                                                \
    }

/** Test whether a resource variable owns a resource. */
#define mirilla_resource_held(name, resource) mirilla_resource_held_##name(resource)
/** Take ownership from a resource variable without releasing it. */
#define mirilla_resource_take(name, resource) mirilla_resource_take_##name(&(resource))
/** Release and empty a resource variable immediately. */
#define mirilla_resource_drop(name, resource) mirilla_resource_cleanup_##name(&(resource))

/** Return the native byte size of a type for an ABI assertion. */
#define MIRILLA_SIZEOF(target_type) sizeof(target_type)
/** Return the native byte offset of a structure member for an ABI assertion. */
#define MIRILLA_OFFSETOF(target_type, target_member) offsetof(target_type, target_member)

#ifdef __KERNEL__
/** Return the native kernel alignment of a type for an ABI assertion. */
#define MIRILLA_ALIGNOF(target_type) __alignof__(target_type)
#elif defined(__cplusplus)
/** Return the native C++ alignment of a type for an ABI assertion. */
#define MIRILLA_ALIGNOF(target_type) alignof(target_type)
#else
/** Return the native C alignment of a type for an ABI assertion. */
#define MIRILLA_ALIGNOF(target_type) _Alignof(target_type)
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
