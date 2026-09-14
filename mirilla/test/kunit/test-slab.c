#include <kunit/test.h>
#include <linux/mm.h>

#include "mirilla-slab.h"
#include "test-slab.h"

/* Retain the inert KUnit slab owner. */
static bool mirilla_slab_test_owner_get(void *owner_context)
{
    return owner_context != NULL;
}

/* Release the inert KUnit slab owner. */
static void mirilla_slab_test_owner_put(void *owner_context)
{
    (void)owner_context;
}

/* Return the supplied KUnit snapshot as its publication handle. */
static int mirilla_slab_test_publish(void *owner_context, void *snapshot_data,
                                     virtual_size_t snapshot_size, void **publication_handle)
{
    (void)owner_context;
    (void)snapshot_size;
    *publication_handle = snapshot_data;

    return 0;
}

/* Accept revocation of the inert KUnit publication handle. */
static void mirilla_slab_test_revoke(void *owner_context, void *publication_handle)
{
    (void)owner_context;
    (void)publication_handle;
}

/* Supply complete callbacks for standalone slab-set configuration tests. */
static const struct mirilla_slab_operations mirilla_slab_test_operations = {
    .owner_get = mirilla_slab_test_owner_get,
    .owner_put = mirilla_slab_test_owner_put,
    .publish = mirilla_slab_test_publish,
    .revoke = mirilla_slab_test_revoke,
};

/** Verify standalone slab-set geometry and callback validation. */
void mirilla_slab_set_initialization_test(struct kunit *test)
{
    struct mirilla_slab_operations operation_table = mirilla_slab_test_operations;
    struct mirilla_slab_set slab_set = { 0 };
    unsigned long owner_context = 0;

    operation_table.publish = NULL;
    KUNIT_EXPECT_EQ(test,
                    mirilla_slab_set_initialize(&slab_set, &owner_context, &operation_table,
                                                PAGE_SIZE, 1),
                    -EINVAL);
    KUNIT_EXPECT_EQ(test,
                    mirilla_slab_set_initialize(&slab_set, &owner_context,
                                                &mirilla_slab_test_operations, 0, 1),
                    -EINVAL);
    KUNIT_EXPECT_EQ(test,
                    mirilla_slab_set_initialize(&slab_set, &owner_context,
                                                &mirilla_slab_test_operations, PAGE_SIZE - 1, 1),
                    -EINVAL);
    KUNIT_EXPECT_EQ(test,
                    mirilla_slab_set_initialize(&slab_set, &owner_context,
                                                &mirilla_slab_test_operations, PAGE_SIZE, 0),
                    -EINVAL);
    KUNIT_ASSERT_EQ(test,
                    mirilla_slab_set_initialize(&slab_set, &owner_context,
                                                &mirilla_slab_test_operations, PAGE_SIZE, 2),
                    0);
    KUNIT_EXPECT_PTR_EQ(test, slab_set.owner_context, &owner_context);
    KUNIT_EXPECT_PTR_EQ(test, slab_set.operation_table, &mirilla_slab_test_operations);
    KUNIT_EXPECT_EQ(test, slab_set.slab_size, (virtual_size_t)PAGE_SIZE);
    KUNIT_EXPECT_EQ(test, slab_set.slab_limit, 2U);
    KUNIT_EXPECT_TRUE(test, mirilla_slab_set_empty(&slab_set));
}
