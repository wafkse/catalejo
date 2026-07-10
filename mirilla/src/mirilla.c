#include <linux/module.h>
#include <linux/kernel.h>

#include "mirilla-device.h"
#include "mirilla-log.h"

static int __init mirilla_init(void)
{
    int mirilla_device_register_error_code;

    MIRILLA_LOG("initializing module");

    if (0 > (mirilla_device_register_error_code = mirilla_device_register())) {
        MIRILLA_ERROR("%04x: primary device failed to register",
                      mirilla_device_register_error_code);

        return mirilla_device_register_error_code;
    }

    MIRILLA_LOG("primary device registered successfully");

    return 0;
}

static void __exit mirilla_exit(void)
{
    int mirilla_device_unregister_error_code;

    MIRILLA_LOG("module exit start");

    if (0 > (mirilla_device_unregister_error_code = mirilla_device_unregister()))
        MIRILLA_ERROR("error(%04x): primary device failed to be unregistered correctly",
                      mirilla_device_unregister_error_code);

    MIRILLA_LOG("module unloaded");
}

module_init(mirilla_init);
module_exit(mirilla_exit);

MODULE_AUTHOR("W. Frakchi");
MODULE_DESCRIPTION("The kernel module for catalejo");
MODULE_LICENSE("GPL");
