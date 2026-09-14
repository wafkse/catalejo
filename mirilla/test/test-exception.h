#ifndef _MIRILLA_TEST_EXCEPTION_H_
#define _MIRILLA_TEST_EXCEPTION_H_

#include "catalejo-except.h"

/** Owned resources for one userspace exception-context test. */
struct exception_fixture {
    /** Mirilla device session used to create the exception context. */
    int device_fd;
    /** Anonymous exception-context descriptor. */
    int exception_fd;
    /** Identifier returned with the exception-context descriptor. */
    mirilla_except_id_t exception_id;
    /** Complete userspace slab mapping, or NULL before mapping. */
    struct mirilla_except_record *record_list;
};

/** Run the userspace exception-slab integration suite. */
int mirilla_test_exception(void);
/** Run the dedicated exception-slab PTE-freeze stress case. */
int mirilla_test_exception_atomic(void);

#endif
