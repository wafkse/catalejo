#include "linux/errno.h"
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "mirilla-log.h"
#include "mirilla-command.h"
#include "mirilla-device.h"
#include "mirilla-map.h"

#define CLASS_NAME MIRILLA_DEVICE_NAME

// NOTE: Allow the kernel to assign a major number to us. We do not care about
// the minor number.
#define MIRILLA_CHARACTER_DEVICE_MAJOR 0x00
#define MIRILLA_CHARACTER_DEVICE_MINOR 0x00

#define MIRILLA_CHARACTER_DEVICE_PARENT NULL

MIRILLA_CONTEXT_CONSTRUCTOR(device)
{
	int error_code = 0;

	struct mirilla_device_context *target_context = NULL;

	if (!(*context_storage = target_context =
		      kzalloc(sizeof(struct mirilla_device_context), GFP_KERNEL)))
		MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate per-session device context");

	mirilla_context_initialize(target_context);

	atomic64_set(&target_context->map_target_count, 0);
	xa_init(&target_context->map_target_list);

	return error_code;
}

MIRILLA_CONTEXT_DESTRUCTOR(device)
{
	mirilla_map_target_id_t target_id = MIRILLA_ID_NONE;
	struct mirilla_map_target_context *map_target_context = NULL;

	xa_for_each(&target_context->map_target_list, target_id, map_target_context)
	{
		mirilla_context_map_target_reference_set(map_target_context);

		xa_erase(&target_context->map_target_list, target_id);
	}

	kfree(target_context);
}

static int mirilla_device_major_number = 0;

/*
 * The device class exposed by the Mirilla module.
 */
static struct class *mirilla_device_class = NULL;

/*
 * The singleton device structure.
 */
static struct device *mirilla_device_struct = NULL;

/*
 * The device number assigned to the Mirilla device.
 */
static dev_t mirilla_device_number;

static int mirilla_open(struct inode *inode, struct file *file)
{
	struct mirilla_device_context *device_context = NULL;

	MIRILLA_LOG("device(%8p,  %8p): open", inode, file);

	int error_code = 0;

	if (0 > (error_code = mirilla_context_device_construct(&device_context)))
		MIRILLA_ERROR_AND_RETURN(error_code, "failed to construct device context");

	file->private_data = device_context;

	return error_code;
}

static int mirilla_release(struct inode *inode, struct file *file)
{
	struct mirilla_device_context *device_context = file->private_data;

	mirilla_context_device_destruct(device_context);

	return 0;
}

static long mirilla_ioctl(struct file *file, unsigned int ioctl_command,
			   unsigned long target_argument)
{
	MIRILLA_LOG("device(%8p): ioctl(%04x, %08lu)", file, ioctl_command, target_argument);

	struct mirilla_device_context *device_context = file->private_data;

	if (MIRILLA_COMMAND_MAGIC(ioctl_command) != MIRILLA_IOCTL_MAGIC)
	    return -ENOTSUPP;

	switch (MIRILLA_COMMAND_CATEGORY(ioctl_command)) {
	case MIRILLA_COMMAND_CATEGORY_MAP:
		return mirilla_map_handle_command(device_context,
						   MIRILLA_COMMAND_ENUMERATION(ioctl_command),
						   target_argument);
	default:
		return -ENOTSUPP;
	}
}

/*
 * The device file operations structure for the device exposed.
 */
static struct file_operations mirilla_device_fops = { .open = mirilla_open,
						       .release = mirilla_release,
						       .unlocked_ioctl = mirilla_ioctl,
						       .compat_ioctl = mirilla_ioctl };

int mirilla_device_register(void)
{
	int pointer_error;

	if (mirilla_device_major_number != 0)
		return -EEXIST;

	if (0 > (mirilla_device_major_number = register_chrdev(MIRILLA_CHARACTER_DEVICE_MAJOR,
								MIRILLA_DEVICE_NAME,
								&mirilla_device_fops)))
		return mirilla_device_major_number;

	if (IS_ERR_OR_NULL(mirilla_device_class = class_create(MIRILLA_DEVICE_NAME)))
		goto unregister_device;

	if (IS_ERR_OR_NULL(
		    mirilla_device_struct = device_create(
			    mirilla_device_class, MIRILLA_CHARACTER_DEVICE_PARENT,
			    (mirilla_device_number = MKDEV(mirilla_device_major_number,
							    MIRILLA_CHARACTER_DEVICE_MINOR)),
			    NULL, MIRILLA_DEVICE_NAME)))
		goto unregister_device_and_destroy_class;

	return 0;

unregister_device:
	pointer_error = PTR_ERR(mirilla_device_class);

	MIRILLA_ERROR("error(0x%04x): failed to register device", pointer_error);

	unregister_chrdev(mirilla_device_major_number, MIRILLA_DEVICE_NAME);
	mirilla_device_major_number = 0;
	mirilla_device_class = NULL;

	return pointer_error;

unregister_device_and_destroy_class:
	pointer_error = PTR_ERR(mirilla_device_struct);

	MIRILLA_ERROR("error(0x%04x): failed to register device class", pointer_error);

	class_destroy(mirilla_device_class);

	unregister_chrdev(mirilla_device_major_number, MIRILLA_DEVICE_NAME);
	mirilla_device_major_number = 0;
	mirilla_device_class = NULL;
	mirilla_device_struct = NULL;

	return pointer_error;
}

int mirilla_device_unregister(void)
{
	if (mirilla_device_major_number == 0)
		return -EALREADY;

	if (mirilla_device_struct) {
		device_destroy(mirilla_device_class, mirilla_device_number);

		mirilla_device_struct = NULL;
	}

	if (mirilla_device_class) {
		class_destroy(mirilla_device_class);

		mirilla_device_class = NULL;
	}

	if (mirilla_device_major_number) {
		unregister_chrdev(mirilla_device_major_number, MIRILLA_DEVICE_NAME);

		mirilla_device_major_number = 0;
	}

	return 0;
}
