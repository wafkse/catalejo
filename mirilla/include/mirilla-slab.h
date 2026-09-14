/*
 * Reusable fd-backed fixed-size publication slabs.
 */

#ifndef _MIRILLA_SLAB_H_
#define _MIRILLA_SLAB_H_

#ifdef __KERNEL__

#include <linux/atomic.h>
#include <linux/mm_types.h>

#include "mirilla-miscellaneous.h"

/*
 * Consumer operations for one reusable slab set.
 *
 * NOTE(invariant): owner_get keeps both owner_context and slab_set alive for a complete VMA
 * lifetime. publish consumes snapshot_data only when it succeeds and returns one non-null
 * publication_handle. revoke removes that handle from consumer visibility before releasing it.
 */
struct mirilla_slab_operations {
    /** Acquire the consumer owner before a VMA begins using the slab set. */
    bool (*owner_get)(void *owner_context);
    /** Release the consumer owner after a VMA has destroyed its slab. */
    void (*owner_put)(void *owner_context);
    /** Validate and consume an immutable snapshot, returning its publication handle. */
    int (*publish)(void *owner_context, void *snapshot_data, virtual_size_t snapshot_size,
                   void **publication_handle);
    /** Remove and release one previously returned publication handle. */
    void (*revoke)(void *owner_context, void *publication_handle);
};

/*
 * Configuration and accounting shared by one family of fixed-size slabs.
 *
 * NOTE(invariant): owner_context and operation_table remain immutable while any VMA exists.
 * slab_count includes every admitted or constructing VMA and never exceeds slab_limit.
 */
struct mirilla_slab_set {
    /** Consumer object retained by every admitted VMA. */
    void *owner_context;
    /** Consumer callbacks governing snapshot publication. */
    const struct mirilla_slab_operations *operation_table;
    /** Exact byte size accepted for every mapping. */
    virtual_size_t slab_size;
    /** Maximum number of simultaneously admitted VMAs. */
    unsigned int slab_limit;
    /** Number of admitted or constructing VMAs. */
    atomic_t slab_count;
};

/**
 * Validate and initialize an unused slab set with immutable consumer configuration.
 *
 * slab_size must be nonzero and page aligned. slab_limit must be nonzero. operation_table must
 * provide every callback. The function returns zero on success or a logged negative errno without
 * modifying slab_set on failure.
 */
int mirilla_slab_set_initialize(struct mirilla_slab_set *slab_set, void *owner_context,
                                const struct mirilla_slab_operations *operation_table,
                                virtual_size_t slab_size, unsigned int slab_limit);

/** Determine whether an initialized slab set has no admitted or constructing VMAs. */
bool mirilla_slab_set_empty(const struct mirilla_slab_set *slab_set);

/**
 * Validate and attach one complete writable shared VMA to an initialized slab set.
 *
 * The VMA must be a zero-offset, exact-size, shared, non-executable RW mapping. The caller keeps
 * owner_context alive through this call. Success acquires the VMA-owned reference that retains
 * both the owner and its embedded slab set until VMA close. Failure returns a logged negative errno
 * and leaves VMA ownership with the caller.
 */
int mirilla_slab_map(struct mirilla_slab_set *slab_set, struct vm_area_struct *vma);

#endif

#endif
