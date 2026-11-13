#ifndef _CATALEJO_EXCEPT_H_
#define _CATALEJO_EXCEPT_H_

#include "mirilla/mirilla-except.h"

#ifndef __BINDGEN__
#include <sys/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque process-local backend for the published built-in exception table. */
struct catalejo_fault_backend;

/**
 * Create one fd-owned exception context for the calling address space.
 *
 * Success writes a nonzero identifier and transfers one owned descriptor to the caller. Returns
 * zero or a negative errno value.
 */
mirilla_command_status_t catalejo_mirilla_except_create(int device_fd, virtual_size_t slab_size,
                                                        mirilla_except_id_t *except_id,
                                                        int *except_fd);

/**
 * Map one exact-size writable shared slab from an exception fd.
 *
 * Success writes the first record address. Returns zero or a negative errno value.
 */
int catalejo_except_slab_map(int except_fd, virtual_size_t slab_size,
                             struct mirilla_except_record **record_list);

/**
 * Publish a complete writable slab as an immutable kernel snapshot.
 *
 * Publication makes the mapping read-only before installing its records for fault handling.
 * If validation fails, the mapping stays writable so the caller can correct the records and
 * retry. The return value is zero on success or a negative errno value on failure.
 */
int catalejo_except_slab_publish(struct mirilla_except_record *record_list,
                                 virtual_size_t slab_size);

/**
 * Remove a slab snapshot and restore writable editing access.
 *
 * The snapshot is revoked before writable access resumes, preventing edits from changing
 * records still in use by the kernel. If the transition fails, the mapping stays published.
 * The return value is zero on success or a negative errno value on failure.
 */
int catalejo_except_slab_edit(struct mirilla_except_record *record_list, virtual_size_t slab_size);

/**
 * Unmap one complete slab and release its VMA.
 *
 * Returns zero or a negative errno value.
 */
int catalejo_except_slab_unmap(struct mirilla_except_record *record_list, virtual_size_t slab_size);

/**
 * Initialize or reuse the process singleton for Catalejo's linked fault routines.
 *
 * The supplied descriptor must identify a Mirilla device so initialization can allocate and
 * publish the linked recovery records. Fork children inherit that publication and can reuse
 * the backend without initializing it again. Returns zero on success or a negative errno value.
 */
int catalejo_fault_backend_initialize(int device_fd, const struct catalejo_fault_backend **backend);

/**
 * Retrieve the initialized backend for the calling process.
 *
 * A fork child may retrieve the inherited backend immediately.
 */
int catalejo_fault_backend_retrieve(const struct catalejo_fault_backend **backend);

#ifdef __cplusplus
}
#endif

#endif
