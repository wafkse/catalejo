#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#if defined(MIRILLA_KUNIT)
#include <kunit/test.h>
#endif

#include "mirilla-device.h"
#include "mirilla-except.h"
#include "mirilla-log.h"

/* Select the anonymous exception inode label exposed by this build mode. */
#if defined(MIRILLA_STEALTH_MODE)
#define MIRILLA_EXCEPT_INODE_NAME MIRILLA_STEALTH_EXCEPT_INODE_NAME
#define MIRILLA_EXCEPT_WARN_ON_ONCE(condition) unlikely(condition)
#else
#define MIRILLA_EXCEPT_INODE_NAME "[mirilla-except]"
#define MIRILLA_EXCEPT_WARN_ON_ONCE(condition) WARN_ON_ONCE(condition)
#endif

struct mirilla_except_global_context mirilla_except_context;

/* Define the exception consumer callbacks used by the reusable slab layer. */
static const struct mirilla_slab_operations mirilla_except_slab_operations;

/* The file owns one reference. Contexts keep the family alive but its list is weak. The
 * creator_context anchor is released in file->release, breaking the initial reference cycle. */
struct mirilla_except_family {
    refcount_t reference_count;
    struct mutex lock;
    struct list_head context_list;
    struct mirilla_except_context *creator_context;
    virtual_size_t slab_size;
    mirilla_except_id_t id;
};

static void mirilla_except_family_put(struct mirilla_except_family *family)
{
    if (refcount_dec_and_test(&family->reference_count)) {
        MIRILLA_EXCEPT_WARN_ON_ONCE(!list_empty(&family->context_list));
        kfree(family);
    }
}

/* Define the context reference acquisition operation used by slab readers. */
MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(except)
{
    return mirilla_context_reference_get(except, context_structure);
}

/* Define the context reference release operation used by slab owners. */
MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(except)
{
    mirilla_context_reference_set(except, context_structure);
}

/* Define the immutable table reference acquisition operation. */
MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(except_table)
{
    return mirilla_context_reference_get(except_table, context_structure);
}

/* Define the immutable table reference release operation. */
MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(except_table)
{
    mirilla_context_reference_set(except_table, context_structure);
}

/* Return the registry bucket selected by an address-space pointer. */
static struct mirilla_except_registry_bucket *
mirilla_except_registry_bucket(struct mm_struct *address_space)
{
    unsigned long bucket_index = hash_ptr(address_space, 6);

    return &mirilla_except_context.registry.bucket_list[bucket_index];
}

/* Find a context while the selected registry bucket is locked. */
static struct mirilla_except_context *
mirilla_except_registry_find_locked(struct mirilla_except_registry_bucket *bucket,
                                    struct mm_struct *address_space)
{
    struct mirilla_except_context *except_context;

    hlist_for_each_entry(except_context, &bucket->context_list, registry_node)
    {
        if (except_context->address_space == address_space)
            return except_context;
    }

    return NULL;
}

/* Acquire a context reference from the weak registry. */
static struct mirilla_except_context *mirilla_except_registry_get(struct mm_struct *address_space)
{
    struct mirilla_except_registry_bucket *registry_bucket;
    struct mirilla_except_context *except_context;

    if (!address_space)
        return NULL;

    registry_bucket = mirilla_except_registry_bucket(address_space);
    raw_spin_lock(&registry_bucket->lock);

    /* NOTE(lifetime): The bucket lock keeps the weak registry link live while the context
     * reference is acquired. */
    except_context = mirilla_except_registry_find_locked(registry_bucket, address_space);
    /* A final put may have reached zero while its destructor waits for this bucket lock. */
    if (except_context && !refcount_inc_not_zero(&except_context->reference_count))
        except_context = NULL;

    raw_spin_unlock(&registry_bucket->lock);

    return except_context;
}

/* Allocate and initialize one fd-owned exception context. */
MIRILLA_CONTEXT_CONSTRUCTOR(except)
{
    struct mirilla_except_context *except_context;

    except_context = kzalloc(sizeof(*except_context), GFP_KERNEL);
    if (!except_context)
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate exception context");

    mirilla_context_initialize(except_context);
    except_context->id = MIRILLA_ID_NONE;
    except_context->address_space = NULL;
    raw_spin_lock_init(&except_context->table_lock);
    INIT_LIST_HEAD(&except_context->table_list);
    INIT_HLIST_NODE(&except_context->registry_node);
    INIT_LIST_HEAD(&except_context->family_node);

    *context_storage = except_context;

    return 0;
}

/* Remove and destroy an exception context after its final reference. */
MIRILLA_CONTEXT_DESTRUCTOR(except)
{
    struct mirilla_except_registry_bucket *registry_bucket;

    MIRILLA_EXCEPT_WARN_ON_ONCE(!mirilla_slab_set_empty(&target_context->slab_set));
    MIRILLA_EXCEPT_WARN_ON_ONCE(!list_empty(&target_context->table_list));

    if (target_context->family) {
        struct mirilla_except_family *family = target_context->family;

        /* Use family -> registry lock order, matching insertion. A lookup cannot see a
         * zero-ref context, and a new context cannot race ahead of registry removal. */
        mutex_lock(&family->lock);
        if (target_context->address_space) {
            registry_bucket = mirilla_except_registry_bucket(target_context->address_space);
            raw_spin_lock(&registry_bucket->lock);
            if (!hlist_unhashed(&target_context->registry_node))
                hlist_del_init(&target_context->registry_node);
            raw_spin_unlock(&registry_bucket->lock);
        }
        if (!list_empty(&target_context->family_node))
            list_del_init(&target_context->family_node);
        mutex_unlock(&family->lock);
    } else if (target_context->address_space) {
        registry_bucket = mirilla_except_registry_bucket(target_context->address_space);
        raw_spin_lock(&registry_bucket->lock);
        if (!hlist_unhashed(&target_context->registry_node))
            hlist_del_init(&target_context->registry_node);
        raw_spin_unlock(&registry_bucket->lock);
    }
    if (target_context->address_space)
        mmdrop(target_context->address_space);

    if (target_context->family)
        mirilla_except_family_put(target_context->family);

    kfree(target_context);
}

/* Return a strong context reference. A family has at most one context per mm; the global
 * registry additionally rejects a competing family for the same mm. */
static struct mirilla_except_context *
mirilla_except_family_get_context(struct mirilla_except_family *family, struct mm_struct *mm)
{
    struct mirilla_except_context *context;
    struct mirilla_except_registry_bucket *bucket;
    int error_code;

retry:
    mutex_lock(&family->lock);
    list_for_each_entry(context, &family->context_list, family_node)
    {
        if (context->address_space == mm) {
            if (!refcount_inc_not_zero(&context->reference_count)) {
                mutex_unlock(&family->lock);
                cond_resched();
                goto retry;
            }
            mutex_unlock(&family->lock);
            return context;
        }
    }

    error_code = mirilla_context_except_construct(&context);
    if (error_code) {
        mutex_unlock(&family->lock);
        return ERR_PTR(error_code);
    }

    context->family = family;
    refcount_inc(&family->reference_count);
    context->address_space = mm;
    mmgrab(mm);
    context->id = family->id;
    error_code = mirilla_slab_set_initialize(&context->slab_set, context,
                                             &mirilla_except_slab_operations, family->slab_size,
                                             MIRILLA_EXCEPT_SLAB_LIMIT);
    if (error_code)
        goto fail;

    bucket = mirilla_except_registry_bucket(mm);
    raw_spin_lock(&bucket->lock);
    if (mirilla_except_registry_find_locked(bucket, mm)) {
        raw_spin_unlock(&bucket->lock);
        error_code = -EEXIST;
        goto fail;
    }
    hlist_add_head(&context->registry_node, &bucket->context_list);
    raw_spin_unlock(&bucket->lock);
    list_add_tail(&context->family_node, &family->context_list);
    mutex_unlock(&family->lock);
    return context;

fail:
    mutex_unlock(&family->lock);
    mirilla_context_except_reference_set(context);
    return ERR_PTR(error_code);
}

/* Allocate one immutable table reference object. */
MIRILLA_CONTEXT_CONSTRUCTOR(except_table)
{
    struct mirilla_except_table_context *table_context;

    table_context = kzalloc(sizeof(*table_context), GFP_KERNEL);
    if (!table_context)
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate exception table context");

    mirilla_context_initialize(table_context);
    INIT_LIST_HEAD(&table_context->context_node);
    *context_storage = table_context;

    return 0;
}

/* Free one immutable table snapshot after its final reference. */
MIRILLA_CONTEXT_DESTRUCTOR(except_table)
{
    kvfree(target_context->record_list);
    kfree(target_context);
}

/* Validate one instruction boundary and calculate its exclusive end. */
bool mirilla_except_boundary_valid(const struct mirilla_except_boundary *boundary,
                                   virtual_address_t *boundary_end)
{
    bool base_present = boundary->base_address != 0;
    bool size_present = boundary->region_size != 0;
    bool reserved_clear = boundary->reserved == 0;
    bool fields_valid = base_present && size_present && reserved_clear;
    bool range_accessible;

    if (!fields_valid)
        return false;

    if (check_add_overflow(boundary->base_address, (virtual_address_t)boundary->region_size,
                           boundary_end))
        return false;

    range_accessible =
        access_ok((void __user *)(unsigned long)boundary->base_address, boundary->region_size);
    if (!range_accessible)
        return false;

    return *boundary_end > boundary->base_address;
}

/* Validate an exception vector and optional page-fault predicate. */
bool mirilla_except_predicate_valid(const struct mirilla_except_predicate *predicate)
{
    bool vector_present = predicate->except_mask != 0;
    bool vector_supported = !(predicate->except_mask & ~MIRILLA_EXCEPT_X86_SUPPORTED_MASK);
    bool value_masked = !(predicate->error_code_value & ~predicate->error_code_mask);
    bool mask_supported = !(predicate->error_code_mask & ~MIRILLA_EXCEPT_PF_ERROR_MASK);
    bool has_error_mask = predicate->error_code_mask != 0;
    bool page_fault_only = predicate->except_mask ==
                           MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT);
    bool vector_valid = vector_present && vector_supported;
    bool error_valid = value_masked && mask_supported;
    bool predicate_compatible = !has_error_mask || page_fault_only;

    if (!vector_valid)
        return false;

    if (!error_valid)
        return false;

    if (!predicate_compatible)
        return false;

    return true;
}

/* Match an exception and error code against one validated predicate. */
bool mirilla_except_predicate_match(const struct mirilla_except_predicate *predicate,
                                    mirilla_except_mask_t except_mask, unsigned long error_code)
{
    bool vector_match;
    bool error_match;

    if (!mirilla_except_predicate_valid(predicate))
        return false;

    vector_match = predicate->except_mask & except_mask;
    error_match = (error_code & predicate->error_code_mask) == predicate->error_code_value;

    return vector_match && error_match;
}

/* Validate the tag and zero padding of one action. */
bool mirilla_except_action_valid(const struct mirilla_except_action *action)
{
    const unsigned char *context = (const unsigned char *)&action->context;

    switch (action->tag) {
    case MIRILLA_EXCEPT_ACTION_NONE:
    case MIRILLA_EXCEPT_ACTION_RETRY:
        return !memchr_inv(context, 0, sizeof(action->context));
    case MIRILLA_EXCEPT_ACTION_IP: {
        bool address_present = action->context.ip.address != 0;
        bool address_accessible =
            access_ok((void __user *)(unsigned long)action->context.ip.address, 1);
        bool padding_clear = !memchr_inv(context + sizeof(action->context.ip), 0,
                                         sizeof(action->context) - sizeof(action->context.ip));

        return address_present && address_accessible && padding_clear;
    }
    default:
        return false;
    }
}

/* Validate a complete copied slab and count its used record prefix. */
int mirilla_except_table_validate(struct mirilla_except_table_context *table)
{
    virtual_size_t record_capacity = table->size / sizeof(*table->record_list);
    virtual_address_t previous_base = 0, previous_end = 0;
    unsigned int group_count = 0;
    bool zero_suffix = false;
    virtual_size_t record_index;

    table->used_count = 0;

    for (record_index = 0; record_index < record_capacity; record_index++) {
        const struct mirilla_except_record *record = &table->record_list[record_index];
        virtual_address_t boundary_end;
        bool zero = !memchr_inv(record, 0, sizeof(*record));

        if (zero) {
            zero_suffix = true;
            continue;
        }

        if (zero_suffix)
            MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table has a used record after its zero "
                                              "suffix");

        if (!mirilla_except_boundary_valid(&record->boundary, &boundary_end))
            MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table has an invalid boundary");

        if (!mirilla_except_predicate_valid(&record->predicate))
            MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table has an invalid predicate");

        if (!mirilla_except_action_valid(&record->action))
            MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table has an invalid action");

        if (record_index && record->boundary.base_address < previous_base)
            MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table records are not sorted");

        if (record_index && record->boundary.base_address == previous_base) {
            bool same_boundary = boundary_end == previous_end;
            bool within_group_limit = ++group_count <= MIRILLA_EXCEPT_BOUNDARY_RECORD_LIMIT;

            if (!same_boundary)
                MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table has mismatched exact "
                                                  "boundaries");

            if (!within_group_limit)
                MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table boundary group is too large");
        } else {
            if (record_index && record->boundary.base_address < previous_end)
                MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception table intervals overlap");

            group_count = 1;
        }

        previous_base = record->boundary.base_address;
        previous_end = boundary_end;
        table->used_count++;
    }

    return 0;
}

/* Determine whether two validated tables contain overlapping intervals. */
bool mirilla_except_table_conflicts(const struct mirilla_except_table_context *left,
                                    const struct mirilla_except_table_context *right)
{
    virtual_size_t left_index = 0, right_index = 0;

    while (left_index < left->used_count && right_index < right->used_count) {
        const struct mirilla_except_boundary *left_boundary =
            &left->record_list[left_index].boundary;
        const struct mirilla_except_boundary *right_boundary =
            &right->record_list[right_index].boundary;
        virtual_address_t left_end = left_boundary->base_address + left_boundary->region_size;
        virtual_address_t right_end = right_boundary->base_address + right_boundary->region_size;

        if (left_boundary->base_address < right_end && right_boundary->base_address < left_end)
            return true;

        if (left_end <= right_boundary->base_address) {
            do {
                left_index++;
            } while (left_index < left->used_count &&
                     left->record_list[left_index].boundary.base_address ==
                         left_boundary->base_address);
        } else {
            do {
                right_index++;
            } while (right_index < right->used_count &&
                     right->record_list[right_index].boundary.base_address ==
                         right_boundary->base_address);
        }
    }

    return false;
}

/* Search one immutable table for a matching interval and predicate. */
static bool mirilla_except_table_search(const struct mirilla_except_table_context *table,
                                        unsigned long instruction_pointer,
                                        mirilla_except_mask_t except_mask, unsigned long error_code,
                                        struct mirilla_except_action *action)
{
    virtual_size_t low = 0, high = table->used_count;
    virtual_size_t index;

    while (low < high) {
        virtual_size_t middle = low + (high - low) / 2;

        if (instruction_pointer < table->record_list[middle].boundary.base_address)
            high = middle;
        else
            low = middle + 1;
    }

    if (!low)
        return false;

    index = low - 1;
    if (instruction_pointer >= table->record_list[index].boundary.base_address +
                                   table->record_list[index].boundary.region_size)
        return false;

    do {
        const struct mirilla_except_record *record = &table->record_list[index];
        bool same_base = record->boundary.base_address ==
                         table->record_list[low - 1].boundary.base_address;
        bool same_size = record->boundary.region_size ==
                         table->record_list[low - 1].boundary.region_size;
        bool same_boundary = same_base && same_size;

        if (!same_boundary)
            break;

        if (mirilla_except_predicate_match(&record->predicate, except_mask, error_code)) {
            *action = record->action;

            return true;
        }
    } while (index-- != 0);

    return false;
}

/* Acquire bounded references to every active immutable exception table. */
static unsigned int
mirilla_except_active_tables_get(struct mirilla_except_context *context,
                                 struct mirilla_except_table_context **table_list)
{
    struct mirilla_except_table_context *table;
    unsigned int count = 0;

    raw_spin_lock(&context->table_lock);
    list_for_each_entry(table, &context->table_list, context_node)
    {
        /* NOTE(lifetime): Each reference is acquired while active-table membership is protected.
         * Readers may then search the immutable table outside the lock. */

        if (MIRILLA_EXCEPT_WARN_ON_ONCE(count >= MIRILLA_EXCEPT_SLAB_LIMIT))
            break;

        if (mirilla_context_except_table_reference_get(table))
            table_list[count++] = table;
    }
    raw_spin_unlock(&context->table_lock);

    return count;
}

/* Release the bounded table-reference array in reverse order. */
static void mirilla_except_active_tables_set(struct mirilla_except_table_context **table_list,
                                             unsigned int count)
{
    while (count)
        mirilla_context_except_table_reference_set(table_list[--count]);
}

#if defined(MIRILLA_KUNIT)
/* Link one constructed context into the production registry for a KUnit lifetime test. */
int mirilla_except_test_registry_insert(struct mirilla_except_context *except_context,
                                        struct mm_struct *address_space)
{
    struct mirilla_except_registry_bucket *registry_bucket;
    bool context_present = except_context != NULL;
    bool address_space_present = address_space != NULL;
    bool address_space_bound;
    bool registry_linked;

    if (!context_present || !address_space_present)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "KUnit registry insertion has an invalid context");

    address_space_bound = except_context->address_space != NULL;
    registry_linked = !hlist_unhashed(&except_context->registry_node);
    if (address_space_bound || registry_linked)
        MIRILLA_ERROR_AND_RETURN(-EEXIST, "KUnit context is already registered");

    except_context->address_space = address_space;
    mmgrab(address_space);
    registry_bucket = mirilla_except_registry_bucket(address_space);

    raw_spin_lock(&registry_bucket->lock);
    if (mirilla_except_registry_find_locked(registry_bucket, address_space)) {
        raw_spin_unlock(&registry_bucket->lock);
        except_context->address_space = NULL;
        mmdrop(address_space);

        MIRILLA_ERROR_AND_RETURN(-EEXIST, "KUnit address space already has a context");
    }

    hlist_add_head(&except_context->registry_node, &registry_bucket->context_list);
    raw_spin_unlock(&registry_bucket->lock);

    return 0;
}

/* Acquire a context reference through the production registry lookup path. */
struct mirilla_except_context *mirilla_except_test_registry_get(struct mm_struct *address_space)
{
    return mirilla_except_registry_get(address_space);
}

/* Acquire bounded active-table references through the production traversal path. */
unsigned int mirilla_except_test_active_table_get(struct mirilla_except_context *except_context,
                                                  struct mirilla_except_table_context **table_list)
{
    return mirilla_except_active_tables_get(except_context, table_list);
}

/* Release table references acquired through the KUnit traversal entry point. */
void mirilla_except_test_active_table_set(struct mirilla_except_table_context **table_list,
                                          unsigned int table_count)
{
    mirilla_except_active_tables_set(table_list, table_count);
}
#endif

/* Search the active tables associated with one address space. */
bool mirilla_except_lookup(struct mm_struct *mm, unsigned long instruction_pointer,
                           mirilla_except_mask_t except_mask, unsigned long error_code,
                           struct mirilla_except_action *action)
{
    struct mirilla_except_table_context *table_list[MIRILLA_EXCEPT_SLAB_LIMIT];
    struct mirilla_except_context *context;
    unsigned int count, index;
    bool matched = false;

    context = mirilla_except_registry_get(mm);
    if (!context)
        return false;

    count = mirilla_except_active_tables_get(context, table_list);
    for (index = 0; index < count; index++)
        if (mirilla_except_table_search(table_list[index], instruction_pointer, except_mask,
                                        error_code, action)) {
            matched = true;
            break;
        }

    mirilla_except_active_tables_set(table_list, count);
    mirilla_context_except_reference_set(context);

    return matched;
}

/* Acquire one exception context reference for an admitted generic slab VMA. */
static bool mirilla_except_slab_owner_get(void *owner_context)
{
    return mirilla_context_except_reference_get(owner_context);
}

/* Release one exception context reference after a generic slab VMA closes. */
static void mirilla_except_slab_owner_put(void *owner_context)
{
    mirilla_context_except_reference_set(owner_context);
}

/* Resolve a new-mm VMA to the family context; the returned set carries one temporary ref. */
static struct mirilla_slab_set *mirilla_except_slab_fork_set(void *owner_context,
                                                             struct mm_struct *child_mm)
{
    struct mirilla_except_context *parent = owner_context;
    struct mirilla_except_context *child;

    child = mirilla_except_family_get_context(parent->family, child_mm);
    if (IS_ERR(child))
        return ERR_CAST(child);
    return &child->slab_set;
}

/* Validate and publish one immutable exception-table snapshot. */
static int mirilla_except_table_publish(void *owner_context, void *snapshot_data,
                                        virtual_size_t snapshot_size, void **publication_handle)
{
    struct mirilla_except_table_context *other_table_list[MIRILLA_EXCEPT_SLAB_LIMIT];
    struct mirilla_except_table_context *table MIRILLA_RESOURCE(except_table) = NULL;
    struct mirilla_except_context *context = owner_context;
    unsigned int table_count, table_index;
    int error_code = 0;

    error_code = mirilla_context_except_table_construct(&table);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to construct exception table");

    table->size = snapshot_size;
    table->record_list = snapshot_data;

    error_code = mirilla_except_table_validate(table);
    if (error_code) {
        table->record_list = NULL;

        MIRILLA_ERROR_AND_RETURN(error_code, "exception table snapshot validation failed");
    }

    /* NOTE(invariant): mprotect holds this mm's mmap write lock across conflict detection and
     * table insertion. No sibling slab can publish or edit between these phases. */
    table_count = mirilla_except_active_tables_get(context, other_table_list);
    for (table_index = 0; table_index < table_count; table_index++)
        if (mirilla_except_table_conflicts(table, other_table_list[table_index])) {
            error_code = -EEXIST;
            break;
        }
    mirilla_except_active_tables_set(other_table_list, table_count);
    if (error_code) {
        table->record_list = NULL;

        MIRILLA_ERROR_AND_RETURN(error_code, "exception table overlaps an active table");
    }

    raw_spin_lock(&context->table_lock);
    list_add_tail(&table->context_node, &context->table_list);
    raw_spin_unlock(&context->table_lock);

    *publication_handle = mirilla_resource_take(except_table, table);

    return 0;
}

/* Remove one immutable exception table from lookup and release its owner reference. */
static void mirilla_except_table_revoke(void *owner_context, void *publication_handle)
{
    struct mirilla_except_table_context *table = publication_handle;
    struct mirilla_except_context *context = owner_context;

    raw_spin_lock(&context->table_lock);
    list_del_init(&table->context_node);
    raw_spin_unlock(&context->table_lock);

    /* NOTE(lifetime): Removal precedes the owner-reference release. Readers that already acquired
     * a table reference remain valid while new lookups can no longer discover it. */
    mirilla_context_except_table_reference_set(table);
}

#if defined(MIRILLA_KUNIT)
/* Exercise the production family, weak registry, publication, and teardown paths with two
 * synthetic mm identities. Each mm keeps a base mm_count reference owned by the test. */
void mirilla_except_test_family_lifecycle(struct kunit *test)
{
    struct mirilla_except_family *family;
    struct mirilla_except_context *parent = NULL, *child = NULL, *again;
    struct mm_struct *parent_mm, *child_mm;
    struct mirilla_except_record *parent_records = NULL, *child_records = NULL;
    struct mirilla_except_action action;
    void *parent_publication = NULL, *child_publication = NULL;
    const virtual_size_t slab_size = MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE;
    bool parent_active, child_active;

    parent_mm = kunit_kzalloc(test, sizeof(*parent_mm), GFP_KERNEL);
    child_mm = kunit_kzalloc(test, sizeof(*child_mm), GFP_KERNEL);
    family = kzalloc(sizeof(*family), GFP_KERNEL);
    if (!parent_mm || !child_mm || !family) {
        kfree(family);
        KUNIT_FAIL(test, "failed to allocate family test state");
        return;
    }
    atomic_set(&parent_mm->mm_count, 1);
    atomic_set(&child_mm->mm_count, 1);
    refcount_set(&family->reference_count, 1);
    mutex_init(&family->lock);
    INIT_LIST_HEAD(&family->context_list);
    family->slab_size = slab_size;
    family->id = 1;

    parent = mirilla_except_family_get_context(family, parent_mm);
    if (IS_ERR(parent)) {
        KUNIT_FAIL(test, "parent context allocation failed");
        parent = NULL;
        goto out;
    }
    family->creator_context = parent;
    child = mirilla_except_family_get_context(family, child_mm);
    if (IS_ERR(child)) {
        KUNIT_FAIL(test, "child context allocation failed");
        child = NULL;
        goto out;
    }
    again = mirilla_except_family_get_context(family, child_mm);
    if (IS_ERR(again)) {
        KUNIT_FAIL(test, "child context lookup failed");
        goto out;
    }
    KUNIT_EXPECT_PTR_EQ(test, again, child);
    mirilla_context_except_reference_set(again);
    KUNIT_EXPECT_PTR_NE(test, parent, child);
    KUNIT_EXPECT_EQ(test, parent->id, child->id);

    parent_records = kvzalloc(slab_size, GFP_KERNEL);
    child_records = kvzalloc(slab_size, GFP_KERNEL);
    if (!parent_records || !child_records) {
        KUNIT_FAIL(test, "table snapshot allocation failed");
        goto out;
    }
    parent_records[0] = (struct mirilla_except_record){
        .boundary = { .base_address = 0x1000, .region_size = 4 },
        .predicate = { .except_mask = MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT) },
        .action = { .tag = MIRILLA_EXCEPT_ACTION_RETRY },
    };
    memcpy(child_records, parent_records, slab_size);
    if (mirilla_except_table_publish(parent, parent_records, slab_size, &parent_publication)) {
        KUNIT_FAIL(test, "family table publication failed");
        goto out;
    }
    parent_records = NULL;
    if (mirilla_except_table_publish(child, child_records, slab_size, &child_publication)) {
        KUNIT_FAIL(test, "child table publication failed");
        goto out;
    }
    child_records = NULL;

    parent_active = mirilla_except_lookup(
        parent_mm, 0x1000, MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action);
    child_active = mirilla_except_lookup(
        child_mm, 0x1000, MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action);
    KUNIT_EXPECT_TRUE(test, parent_active);
    KUNIT_EXPECT_TRUE(test, child_active);

    mirilla_except_table_revoke(child, child_publication);
    child_publication = NULL;
    KUNIT_EXPECT_TRUE(test, mirilla_except_lookup(
                                parent_mm, 0x1000,
                                MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action));
    KUNIT_EXPECT_FALSE(test, mirilla_except_lookup(
                                 child_mm, 0x1000,
                                 MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action));

    child_records = kvzalloc(slab_size, GFP_KERNEL);
    if (!child_records) {
        KUNIT_FAIL(test, "child replacement snapshot allocation failed");
        goto out;
    }
    child_records[0] = (struct mirilla_except_record){
        .boundary = { .base_address = 0x2000, .region_size = 4 },
        .predicate = { .except_mask = MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT) },
        .action = { .tag = MIRILLA_EXCEPT_ACTION_RETRY },
    };
    if (mirilla_except_table_publish(child, child_records, slab_size, &child_publication)) {
        KUNIT_FAIL(test, "child replacement publication failed");
        goto out;
    }
    child_records = NULL;
    mirilla_except_table_revoke(parent, parent_publication);
    parent_publication = NULL;
    KUNIT_EXPECT_FALSE(test, mirilla_except_lookup(
                                 parent_mm, 0x1000,
                                 MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action));
    KUNIT_EXPECT_TRUE(test, mirilla_except_lookup(
                                child_mm, 0x2000,
                                MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT), 0, &action));
    mirilla_except_table_revoke(child, child_publication);
    child_publication = NULL;
    mirilla_context_except_reference_set(child);
    child = NULL;
    again = mirilla_except_registry_get(child_mm);
    KUNIT_EXPECT_PTR_EQ(test, again, NULL);
    if (again)
        mirilla_context_except_reference_set(again);
    again = mirilla_except_family_get_context(family, parent_mm);
    KUNIT_EXPECT_PTR_EQ(test, again, parent);
    if (!IS_ERR(again))
        mirilla_context_except_reference_set(again);

out:
    if (child_publication)
        mirilla_except_table_revoke(child, child_publication);
    if (parent_publication)
        mirilla_except_table_revoke(parent, parent_publication);
    kvfree(child_records);
    kvfree(parent_records);
    if (child)
        mirilla_context_except_reference_set(child);
    if (parent)
        mirilla_context_except_reference_set(parent);
    mirilla_except_family_put(family);
    KUNIT_EXPECT_EQ(test, atomic_read(&parent_mm->mm_count), 1);
    KUNIT_EXPECT_EQ(test, atomic_read(&child_mm->mm_count), 1);
}
#endif

/* Bind exception table ownership and validation to the generic slab lifecycle. */
static const struct mirilla_slab_operations mirilla_except_slab_operations = {
    .owner_get = mirilla_except_slab_owner_get,
    .owner_put = mirilla_except_slab_owner_put,
    .fork_set = mirilla_except_slab_fork_set,
    .publish = mirilla_except_table_publish,
    .revoke = mirilla_except_table_revoke,
};

/* Resolve the calling mm in the inherited family and map a slab owned by that mm. */
static int mirilla_except_file_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct mirilla_except_family *family = file->private_data;
    struct mirilla_except_context *context;
    int error_code;

    context = mirilla_except_family_get_context(family, vma->vm_mm);
    if (IS_ERR(context))
        return PTR_ERR(context);

    error_code = mirilla_slab_map(&context->slab_set, vma);
    mirilla_context_except_reference_set(context);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to map exception slab");

    return 0;
}

/* Release the creator anchor and file reference after all descriptors and VMAs close. */
static int mirilla_except_file_release(struct inode *inode, struct file *file)
{
    struct mirilla_except_family *family = file->private_data;
    struct mirilla_except_context *creator;

    file->private_data = NULL;
    if (family) {
        creator = family->creator_context;
        family->creator_context = NULL;
        if (creator)
            mirilla_context_except_reference_set(creator);
        mirilla_except_family_put(family);
    }

    return 0;
}

/* Define the fd operations for exception contexts. */
static const struct file_operations mirilla_except_file_operations = {
    .release = mirilla_except_file_release,
    .mmap = mirilla_except_file_mmap,
    .owner = THIS_MODULE,
};

/* Validate CREATE and prepare its descriptor reservation. */
static int mirilla_except_create(struct mirilla_device_context *device_context,
                                 union mirilla_except_create_io *io,
                                 struct mirilla_fd_reservation *fd_reservation)
{
    struct mirilla_except_family *family;
    struct mirilla_except_context *context;
    struct file *context_file;
    virtual_size_t slab_size = io->argument.slab_size;
    int file_descriptor, error_code;

    if (!current->mm)
        MIRILLA_ERROR_AND_RETURN(-ESTALE, "exception context requires a current address space");

    if (slab_size > MIRILLA_EXCEPT_SLAB_SIZE_LIMIT)
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception slab size exceeds the kernel limit");

    if (slab_size % sizeof(struct mirilla_except_record))
        MIRILLA_ERROR_AND_RETURN(-EINVAL, "exception slab size is not record aligned");

    family = kzalloc(sizeof(*family), GFP_KERNEL);
    if (!family)
        MIRILLA_ERROR_AND_RETURN(-ENOMEM, "failed to allocate exception family");
    refcount_set(&family->reference_count, 1);
    mutex_init(&family->lock);
    INIT_LIST_HEAD(&family->context_list);
    family->slab_size = slab_size;
    family->id = atomic_inc_return(&device_context->except_count);

    context = mirilla_except_family_get_context(family, current->mm);
    if (IS_ERR(context)) {
        error_code = PTR_ERR(context);
        mirilla_except_family_put(family);
        return error_code;
    }
    family->creator_context = context;

    context_file = anon_inode_create_getfile(MIRILLA_EXCEPT_INODE_NAME,
                                             &mirilla_except_file_operations, family,
                                             MIRILLA_EXCEPT_FILE_FLAGS, NULL);
    if (IS_ERR(context_file)) {
        error_code = PTR_ERR(context_file);
        family->creator_context = NULL;
        mirilla_context_except_reference_set(context);
        mirilla_except_family_put(family);
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to create exception context file");
    }

    file_descriptor =
        mirilla_fd_reservation_prepare(fd_reservation, context_file, MIRILLA_EXCEPT_FILE_FLAGS);
    if (file_descriptor < 0)
        MIRILLA_ERROR_AND_RETURN(file_descriptor, "failed to reserve exception context descriptor");

    io->result.id = family->id;
    io->result.fd = file_descriptor;

    return 0;
}

/* Handle the exception command category and commit successful CREATE results. */
mirilla_command_status_t
mirilla_except_handle_command(struct mirilla_device_context *device_context,
                              mirilla_command_t command, mirilla_command_argument_t argument)
{
    union mirilla_except_create_io io;
    void __user *user_structure = (void __user *)argument;
    struct mirilla_fd_reservation fd_reservation MIRILLA_FD_RESERVATION = {
        .target_file = NULL,
        .file_descriptor = -1,
    };
    int error_code;

    if (command != MIRILLA_COMMAND_EXCEPT_CREATE)
        MIRILLA_ERROR_AND_RETURN(-ENOTTY, "unknown exception command");

    if (copy_from_user(&io, user_structure, sizeof(io)))
        MIRILLA_ERROR_AND_RETURN(-EFAULT, "failed to copy exception command from userspace");

    error_code = mirilla_except_create(device_context, &io, &fd_reservation);
    if (error_code)
        MIRILLA_ERROR_AND_RETURN(error_code, "failed to create exception context");

    if (copy_to_user(user_structure, &io, sizeof(io)))
        MIRILLA_ERROR_AND_RETURN(-EFAULT, "failed to copy exception result to userspace");

    mirilla_fd_reservation_install(&fd_reservation);

    return 0;
}

/* Initialize the weak per-mm registry buckets. */
int mirilla_except_registry_initialize(void)
{
    unsigned int index;

    for (index = 0; index < MIRILLA_EXCEPT_REGISTRY_BUCKET_COUNT; index++) {
        raw_spin_lock_init(&mirilla_except_context.registry.bucket_list[index].lock);
        INIT_HLIST_HEAD(&mirilla_except_context.registry.bucket_list[index].context_list);
    }

    return 0;
}

/* Verify that module teardown has no remaining weak registry entries. */
void mirilla_except_registry_deinitialize(void)
{
    unsigned int index;

    for (index = 0; index < MIRILLA_EXCEPT_REGISTRY_BUCKET_COUNT; index++)
        MIRILLA_EXCEPT_WARN_ON_ONCE(
            !hlist_empty(&mirilla_except_context.registry.bucket_list[index].context_list));
}
