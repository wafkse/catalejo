/*
 * Self-contained context structure management.
 */

#ifndef _MIRILLA_CONTEXT_H_
#define _MIRILLA_CONTEXT_H_

#define MIRILLA_CONTEXT_REFERENCE_COUNT_NAME reference_count
#define MIRILLA_CONTEXT_REFERENCE_COUNT_DEFINE refcount_t MIRILLA_CONTEXT_REFERENCE_COUNT_NAME;
#define MIRILLA_CONTEXT_REFERENCE_COUNT_FIELD(context_structure) \
    (&(context_structure)->MIRILLA_CONTEXT_REFERENCE_COUNT_NAME)
#define MIRILLA_CONTEXT_REFERENCE_COUNT_INITIALIZE(context_structure) \
    refcount_set(MIRILLA_CONTEXT_REFERENCE_COUNT_FIELD(context_structure), 1)

#define mirilla_context_reference_get(context_name, context_structure)               \
    ({                                                                               \
        bool ___target_value = false;                                                \
                                                                                     \
        if (!(___target_value = refcount_inc_not_zero(                               \
                  MIRILLA_CONTEXT_REFERENCE_COUNT_FIELD(context_structure))))        \
            /* NOTE(invariant): No bail here. It indicates very bad      \
       * memory corruption going on. */           \
            MIRILLA_ERROR(MIRILLA_LOG_PREFIX_LIFETIME "attempted to take reference " \
                                                      "to logically "                \
                                                      "freed context (%s: 0x%8p)",   \
                          #context_name, &(context_structure));                      \
        ___target_value;                                                             \
    })

#define mirilla_context_reference_set(context_name, context_structure)                       \
    do {                                                                                     \
        if (refcount_dec_and_test(MIRILLA_CONTEXT_REFERENCE_COUNT_FIELD(context_structure))) \
            mirilla_context_##context_name##_destruct(context_structure);                    \
    } while (0)

/*
 * Define a set of helper functions for reference-counting.
 */
#define MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(context_name) \
    bool mirilla_context_##context_name##_reference_get(   \
        struct mirilla_##context_name##_context *context_structure)

#define MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(context_name) \
    void mirilla_context_##context_name##_reference_set(   \
        struct mirilla_##context_name##_context *context_structure)

#define MIRILLA_CONTEXT_DEFINE(context_name, ...)       \
    struct mirilla_##context_name##_context {           \
        __VA_ARGS__                                     \
        MIRILLA_CONTEXT_REFERENCE_COUNT_DEFINE          \
    };                                                  \
    MIRILLA_CONTEXT_CONSTRUCTOR(context_name);          \
    MIRILLA_CONTEXT_DESTRUCTOR(context_name);           \
    MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(context_name); \
    MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(context_name);

#define MIRILLA_CONTEXT_CONSTRUCTOR(context_name)          \
    extern int mirilla_context_##context_name##_construct( \
        struct mirilla_##context_name##_context **context_storage)

#define MIRILLA_CONTEXT_DESTRUCTOR(context_name)           \
    extern void mirilla_context_##context_name##_destruct( \
        struct mirilla_##context_name##_context *target_context)

/*
 * A function-like macro to initialize a context structure defined by a
 * respective `MIRILLA_CONTEXT_DEFINE` macro invocation.
 */
#define mirilla_context_initialize(context_structure)                  \
    do {                                                               \
        MIRILLA_CONTEXT_REFERENCE_COUNT_INITIALIZE(context_structure); \
    } while (0)

#endif
