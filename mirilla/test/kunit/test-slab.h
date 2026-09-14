/*
 * Reusable slab KUnit declarations.
 */

#ifndef _MIRILLA_TEST_SLAB_H_
#define _MIRILLA_TEST_SLAB_H_

#include <kunit/test.h>

/** Verify standalone slab-set geometry and callback validation. */
void mirilla_slab_set_initialization_test(struct kunit *test);

#endif
