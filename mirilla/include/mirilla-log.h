/*
 * Logging facilities for the Mirilla infrastructure.
 */

#ifndef _MIRILLA_LOG_H
#define _MIRILLA_LOG_H

#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/printk.h>

#define MIRILLA_LOG_LOG_LEVEL KERN_INFO
#define MIRILLA_LOG_ERROR_LEVEL KERN_ERR

#if defined(MIRILLA_DEBUG)
/*
 * Log a debug log for further inspection.
 */
#define MIRILLA_DEBUG(fmt, ...) \
    printk(MIRILLA_LOG_LOG_LEVEL "mirilla(debug): " fmt "\n", ##__VA_ARGS__)
#else
#define MIRILLA_DEBUG(fmt, ...) ((void)0)
#endif

/*
 * Log a general-purpose informative formatted string.
 */
#define MIRILLA_LOG(fmt, ...) printk(MIRILLA_LOG_LOG_LEVEL "mirilla: " fmt "\n", ##__VA_ARGS__)

/*
 * Log a formatted error string.
 */
#define MIRILLA_ERROR(fmt, ...)                        \
    printk(MIRILLA_LOG_ERROR_LEVEL "mirilla: "         \
                                   "error: " fmt "\n", \
           ##__VA_ARGS__)

/*
 * Log a formatted error string and return the appropriate `ERRNO`.
 */
#define MIRILLA_ERROR_AND_RETURN(retval, fmt, ...) \
    do {                                           \
        MIRILLA_ERROR(fmt, ##__VA_ARGS__);         \
                                                   \
        return retval;                             \
    } while (0);

/*
 * Lifetime-related log messages.
 */
#define MIRILLA_LOG_PREFIX_LIFETIME "lifetime: "

#endif

#endif
