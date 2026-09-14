#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/refcount.h>

#include "mirilla-except.h"
#include "test-slab.h"

/* Construct one minimal valid retry record for validator tests. */
static struct mirilla_except_record mirilla_except_test_record(virtual_address_t base_address,
                                                               uint32_t region_size)
{
    return (struct mirilla_except_record){
        .boundary = {
            .base_address = base_address,
            .region_size = region_size,
        },
        .predicate = {
            .except_mask = MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT),
        },
        .action = {
            .tag = MIRILLA_EXCEPT_ACTION_RETRY,
        },
    };
}

/* Verify that an all-zero slab publishes as an empty immutable table. */
static void mirilla_except_empty_snapshot_test(struct kunit *test)
{
    struct mirilla_except_record record_list[4] = { 0 };
    struct mirilla_except_table_context table_context = {
        .size = sizeof(record_list),
        .record_list = record_list,
    };

    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), 0);
    KUNIT_EXPECT_EQ(test, table_context.used_count, (virtual_size_t)0);
}

/* Verify that used records must form one prefix followed only by zero records. */
static void mirilla_except_trailing_zero_occupancy_test(struct kunit *test)
{
    struct mirilla_except_record record_list[4] = {
        mirilla_except_test_record(0x1000, 4),
        mirilla_except_test_record(0x2000, 4),
    };
    struct mirilla_except_table_context table_context = {
        .size = sizeof(record_list),
        .record_list = record_list,
    };

    KUNIT_ASSERT_EQ(test, mirilla_except_table_validate(&table_context), 0);
    KUNIT_EXPECT_EQ(test, table_context.used_count, (virtual_size_t)2);

    record_list[3] = mirilla_except_test_record(0x3000, 4);
    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), -EINVAL);
}

/* Verify ordering, exact-boundary grouping, overlap, and vector validation. */
static void mirilla_except_snapshot_validation_test(struct kunit *test)
{
    struct mirilla_except_record record_list[3] = {
        mirilla_except_test_record(0x1000, 8),
        mirilla_except_test_record(0x1000, 8),
        mirilla_except_test_record(0x2000, 8),
    };
    struct mirilla_except_table_context table_context = {
        .size = sizeof(record_list),
        .record_list = record_list,
    };

    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), 0);

    record_list[1].boundary.region_size = 4;
    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), -EINVAL);
    record_list[1].boundary.region_size = 8;
    record_list[2].boundary.base_address = 0x1004;
    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), -EINVAL);
    record_list[2].boundary.base_address = 0x2000;
    record_list[1].predicate.except_mask = ~0ULL;
    KUNIT_EXPECT_EQ(test, mirilla_except_table_validate(&table_context), -EINVAL);
}

/* Verify that independently valid slabs cannot publish overlapping intervals. */
static void mirilla_except_cross_slab_conflict_test(struct kunit *test)
{
    struct mirilla_except_record left_record_list[] = {
        mirilla_except_test_record(0x1000, 0x100),
    };
    struct mirilla_except_record right_record_list[] = {
        mirilla_except_test_record(0x1080, 0x100),
    };
    struct mirilla_except_table_context left_table = {
        .size = sizeof(left_record_list),
        .record_list = left_record_list,
    };
    struct mirilla_except_table_context right_table = {
        .size = sizeof(right_record_list),
        .record_list = right_record_list,
    };

    KUNIT_ASSERT_EQ(test, mirilla_except_table_validate(&left_table), 0);
    KUNIT_ASSERT_EQ(test, mirilla_except_table_validate(&right_table), 0);
    KUNIT_EXPECT_TRUE(test, mirilla_except_table_conflicts(&left_table, &right_table));

    right_record_list[0].boundary.base_address = 0x2000;
    KUNIT_ASSERT_EQ(test, mirilla_except_table_validate(&right_table), 0);
    KUNIT_EXPECT_FALSE(test, mirilla_except_table_conflicts(&left_table, &right_table));
}

/* Verify that a table reader keeps the immutable object alive after owner release. */
static void mirilla_except_table_reference_handoff_test(struct kunit *test)
{
    struct mirilla_except_table_context *owner_table = NULL;
    struct mirilla_except_table_context *reader_table;

    KUNIT_ASSERT_EQ(test, mirilla_context_except_table_construct(&owner_table), 0);
    reader_table = owner_table;
    KUNIT_ASSERT_TRUE(test, mirilla_context_except_table_reference_get(reader_table));
    KUNIT_EXPECT_EQ(test, refcount_read(&reader_table->reference_count), 2);

    mirilla_context_except_table_reference_set(owner_table);
    KUNIT_EXPECT_EQ(test, refcount_read(&reader_table->reference_count), 1);
    mirilla_context_except_table_reference_set(reader_table);
}

/* Verify that the final ordinary context reference selects destruction. */
static void mirilla_except_context_final_reference_test(struct kunit *test)
{
    struct mirilla_except_context *except_context = NULL;

    KUNIT_ASSERT_EQ(test, mirilla_context_except_construct(&except_context), 0);
    KUNIT_ASSERT_TRUE(test, mirilla_context_except_reference_get(except_context));
    KUNIT_EXPECT_EQ(test, refcount_read(&except_context->reference_count), 2);
    mirilla_context_except_reference_set(except_context);
    KUNIT_EXPECT_EQ(test, refcount_read(&except_context->reference_count), 1);
    mirilla_context_except_reference_set(except_context);
}

/* Verify weak-registry pointer lookup acquires a strong context reference under its lock. */
static void mirilla_except_registry_reference_handoff_test(struct kunit *test)
{
    struct mirilla_except_context *except_context = NULL;
    struct mirilla_except_context *reader_context;

    KUNIT_ASSERT_NOT_NULL(test, current->mm);
    KUNIT_ASSERT_EQ(test, mirilla_context_except_construct(&except_context), 0);
    KUNIT_ASSERT_EQ(test, mirilla_except_test_registry_insert(except_context, current->mm), 0);

    reader_context = mirilla_except_test_registry_get(current->mm);
    KUNIT_ASSERT_PTR_EQ(test, reader_context, except_context);
    KUNIT_EXPECT_EQ(test, refcount_read(&reader_context->reference_count), 2);

    mirilla_context_except_reference_set(except_context);
    KUNIT_EXPECT_EQ(test, refcount_read(&reader_context->reference_count), 1);
    mirilla_context_except_reference_set(reader_context);
}

/* Verify slab detachment cannot invalidate a table held by an active reader. */
static void mirilla_except_detach_active_reader_test(struct kunit *test)
{
    struct mirilla_except_table_context *table_list[MIRILLA_EXCEPT_SLAB_LIMIT] = { 0 };
    struct mirilla_except_table_context *table_context = NULL;
    struct mirilla_except_context *except_context = NULL;
    unsigned int table_count;

    KUNIT_ASSERT_EQ(test, mirilla_context_except_construct(&except_context), 0);
    KUNIT_ASSERT_EQ(test, mirilla_context_except_table_construct(&table_context), 0);

    raw_spin_lock(&except_context->table_lock);
    list_add_tail(&table_context->context_node, &except_context->table_list);
    raw_spin_unlock(&except_context->table_lock);

    table_count = mirilla_except_test_active_table_get(except_context, table_list);
    KUNIT_ASSERT_EQ(test, table_count, 1U);
    KUNIT_EXPECT_EQ(test, refcount_read(&table_context->reference_count), 2);

    raw_spin_lock(&except_context->table_lock);
    list_del_init(&table_context->context_node);
    raw_spin_unlock(&except_context->table_lock);

    mirilla_context_except_table_reference_set(table_context);
    KUNIT_EXPECT_EQ(test, refcount_read(&table_list[0]->reference_count), 1);
    mirilla_except_test_active_table_set(table_list, table_count);
    mirilla_context_except_reference_set(except_context);
}

/* Enumerate the reusable-slab and exception-table KUnit cases. */
static struct kunit_case mirilla_except_test_case_list[] = {
    KUNIT_CASE(mirilla_slab_set_initialization_test),
    KUNIT_CASE(mirilla_except_empty_snapshot_test),
    KUNIT_CASE(mirilla_except_trailing_zero_occupancy_test),
    KUNIT_CASE(mirilla_except_snapshot_validation_test),
    KUNIT_CASE(mirilla_except_cross_slab_conflict_test),
    KUNIT_CASE(mirilla_except_table_reference_handoff_test),
    KUNIT_CASE(mirilla_except_context_final_reference_test),
    KUNIT_CASE(mirilla_except_registry_reference_handoff_test),
    KUNIT_CASE(mirilla_except_detach_active_reader_test),
    {}
};

/* Describe the slab and exception-table KUnit suite. */
static struct kunit_suite mirilla_except_test_suite = {
    .name = "mirilla-except-table",
    .test_cases = mirilla_except_test_case_list,
};

kunit_test_suite(mirilla_except_test_suite);
