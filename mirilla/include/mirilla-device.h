/*
 * Header file for the Mirilla Device.
 */

#ifndef _MIRILLA_DEVICE_H
#define _MIRILLA_DEVICE_H

#include "mirilla-context.h" // IWYU pragma: export
#include "mirilla-id.h" // IWYU pragma: export

/* NOTE(invariant): The class name and device name are the same. */
#define MIRILLA_DEVICE_NAME "mirilla"

#ifdef __KERNEL__

#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/xarray.h>

#include <linux/kref.h>

MIRILLA_CONTEXT_DEFINE(
	device, struct {
		/*
       * The map target context list for this same device
       * session.
       */
		mirilla_atomic_id_t map_target_count;

		/*
       * The list of map targets.
       */
		struct xarray map_target_list;
	};);

MIRILLA_CONTEXT_CONSTRUCTOR(device);
MIRILLA_CONTEXT_DESTRUCTOR(device);

/*
 * Initialize the device for the mirilla module.
 */
int mirilla_device_register(void);

/*
 * Unregister the device for the mirilla module.
 */
int mirilla_device_unregister(void);

#endif

#endif
