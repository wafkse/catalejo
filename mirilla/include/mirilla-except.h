/*
 * Fixed-slab userspace architectural exception tables.
 */

#ifndef _MIRILLA_EXCEPT_H_
#define _MIRILLA_EXCEPT_H_

#include "mirilla-command.h" // IWYU pragma: export
#include "mirilla-id.h" // IWYU pragma: export
#include "mirilla-miscellaneous.h" // IWYU pragma: export

#ifdef __KERNEL__
#include <asm/trapnr.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/** The ioctl category reserved for exception-context commands. */
#define MIRILLA_COMMAND_CATEGORY_EXCEPT 0x01

/** The complete exception command list. */
#define MIRILLA_EXCEPT_COMMANDS X(CREATE, 0x00, create)

#define X(name, val, io) MIRILLA_COMMAND_EXCEPT_##name = val,
enum { MIRILLA_EXCEPT_COMMANDS MIRILLA_EXCEPT_NR_COMMANDS };
#undef X

#define X(name, val, io) "MIRILLA_COMMAND_EXCEPT_" #name,
__attribute((unused)) static const char *MIRILLA_COMMAND_NAME_EXCEPT_TABLE[] = {
    MIRILLA_EXCEPT_COMMANDS NULL
};
#undef X

/** Return the generated name for an exception command number. */
#define MIRILLA_COMMAND_NAME_EXCEPT(except_command) \
    (MIRILLA_COMMAND_NAME_EXCEPT_TABLE[except_command])

/** Define the input-output union and size assertion for one exception command. */
#define MIRILLA_EXCEPT_DEFINE_COMMAND_IO(name)            \
    union mirilla_except_##name##_io {                    \
        struct mirilla_except_##name##_argument argument; \
        struct mirilla_except_##name##_result result;     \
    };                                                    \
    MIRILLA_ASSERT_IO_SIZE(except_##name)

/** An identifier for an fd-owned exception context. */
typedef mirilla_id_t mirilla_except_id_t;

/** A bit mask of architectural exception vectors. */
typedef uint64_t mirilla_except_mask_t;

/** The number of vector bits represented by an exception mask. */
#define MIRILLA_EXCEPT_VECTOR_LIMIT (MIRILLA_SIZEOF(mirilla_except_mask_t) * 8U)

/** Convert an exception vector number to its mask bit. */
#define MIRILLA_EXCEPT_MASK(vector) (1ULL << (vector))

/** x86 invalid-opcode vector number. */
#define MIRILLA_EXCEPT_X86_INVALID_OPCODE (6U)
/** x86 general-protection vector number. */
#define MIRILLA_EXCEPT_X86_GENERAL_PROTECTION (13U)
/** x86 page-fault vector number. */
#define MIRILLA_EXCEPT_X86_PAGE_FAULT (14U)
/** x86 alignment-check vector number. */
#define MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK (17U)

#ifdef __KERNEL__
MIRILLA_ASSERT(MIRILLA_EXCEPT_X86_INVALID_OPCODE == X86_TRAP_UD, "x86 #UD trap number mismatch");
MIRILLA_ASSERT(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION == X86_TRAP_GP, "x86 #GP trap number "
                                                                     "mismatch");
MIRILLA_ASSERT(MIRILLA_EXCEPT_X86_PAGE_FAULT == X86_TRAP_PF, "x86 #PF trap number mismatch");
MIRILLA_ASSERT(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK == X86_TRAP_AC, "x86 #AC trap number mismatch");
#endif

/** The x86 vectors supported by the exception table ABI. */
#define MIRILLA_EXCEPT_X86_SUPPORTED_MASK                         \
    (MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_INVALID_OPCODE) |     \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_GENERAL_PROTECTION) | \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_PAGE_FAULT) |         \
     MIRILLA_EXCEPT_MASK(MIRILLA_EXCEPT_X86_ALIGNMENT_CHECK))

/** The maximum number of records sharing one instruction interval. */
#define MIRILLA_EXCEPT_BOUNDARY_RECORD_LIMIT (16U)
/** The hard maximum number of slabs in one address space. */
#define MIRILLA_EXCEPT_SLAB_LIMIT (16U)
/** The maximum permitted slab size in bytes. */
#define MIRILLA_EXCEPT_SLAB_SIZE_LIMIT (2U * 1024U * 1024U)
/** The default slab size used by built-in userspace recovery. */
#define MIRILLA_EXCEPT_DEFAULT_SLAB_SIZE (12U * 1024U)
/** The number of buckets in the weak per-mm context registry. */
#define MIRILLA_EXCEPT_REGISTRY_BUCKET_COUNT (64U)

/** Page-fault present error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_PRESENT (0x01U)
/** Page-fault write error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_WRITE (0x02U)
/** Page-fault user error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_USER (0x04U)
/** Page-fault reserved-bit error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_RESERVED (0x08U)
/** Page-fault instruction-fetch error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_INSTRUCTION (0x10U)
/** Page-fault protection-key error-code bit. */
#define MIRILLA_EXCEPT_X86_PF_PROTECTION_KEY (0x20U)
/** The page-fault error-code bits accepted by predicates. */
#define MIRILLA_EXCEPT_PF_ERROR_MASK (0x3fU)

/*
 * An instruction interval in an immutable exception record.
 *
 * NOTE(invariant): A used boundary has a nonzero base and size, has a representable exclusive
 * end, and keeps reserved zero. The all-zero boundary participates in the all-zero unused suffix.
 */
struct mirilla_except_boundary {
    /** Inclusive start address of the protected instruction interval. */
    virtual_address_t base_address;
    /** Size of the protected instruction interval in bytes. */
    uint32_t region_size;
    /** Reserved ABI padding, which must remain zero. */
    uint32_t reserved;
};

/*
 * An architectural exception predicate.
 *
 * NOTE(invariant): The vector mask contains only supported vectors. Error-code predicates are
 * valid only for a page-fault-only vector and select only stable page-fault error bits.
 */
struct mirilla_except_predicate {
    /** Exception vectors that activate this record. */
    mirilla_except_mask_t except_mask;
    /** Page-fault error-code bits participating in the comparison. */
    uint32_t error_code_mask;
    /** Required values for the selected page-fault error-code bits. */
    uint32_t error_code_value;
};

/** The encoded action discriminator. */
typedef uint16_t mirilla_except_action_tag_t;

enum {
    /** Do not alter the saved register frame. */
    MIRILLA_EXCEPT_ACTION_NONE = 0,
    /** Replace the saved instruction pointer. */
    MIRILLA_EXCEPT_ACTION_IP = 1,
    /** Retry the faulting instruction. */
    MIRILLA_EXCEPT_ACTION_RETRY = 2,
};

/** Empty payload for an action with no context. */
struct mirilla_except_action_none_context {
} MIRILLA_PACKED;

/** Payload containing a userspace instruction address. */
struct mirilla_except_action_ip_context {
    /** Userspace instruction address to resume at. */
    virtual_address_t address;
} MIRILLA_PACKED;

/** Empty payload for a retry action. */
struct mirilla_except_action_retry_context {
} MIRILLA_PACKED;

/** Alignment extent used to make the packed action union exactly 14 bytes. */
struct mirilla_except_action_context_extent {
    /** Eight-byte alignment extent. */
    uint64_t aligned_64;
    /** Four-byte alignment extent. */
    uint32_t aligned_32;
    /** Two-byte alignment extent. */
    uint16_t aligned_16;
} MIRILLA_PACKED;

/** The tag-selected payload of an exception action. */
union mirilla_except_action_context {
    /** Empty payload for the none action. */
    struct mirilla_except_action_none_context none;
    /** Instruction-pointer payload for the IP action. */
    struct mirilla_except_action_ip_context ip;
    /** Empty payload for the retry action. */
    struct mirilla_except_action_retry_context retry;
    /** ABI-only extent member. */
    struct mirilla_except_action_context_extent extent MIRILLA_UNSTABLE_FIELD;
} MIRILLA_PACKED;

/*
 * An immutable architectural exception action.
 *
 * NOTE(invariant): The 14-byte context is interpreted only by tag. Unused bytes are zero, and an
 * IP action names a nonzero userspace instruction address.
 */
struct mirilla_except_action {
    /** Tag-selected action payload. */
    union mirilla_except_action_context context;
    /** Action discriminator. */
    mirilla_except_action_tag_t tag;
} MIRILLA_ALIGNED(16);

/*
 * One fixed-stride exception record.
 *
 * NOTE(invariant): Used records form a sorted prefix. Exact-boundary records are adjacent and
 * bounded. Distinct instruction intervals do not overlap. The remaining slab is all-zero records.
 */
struct mirilla_except_record {
    /** Instruction interval matched before the predicate. */
    struct mirilla_except_boundary boundary;
    /** Architectural exception predicate. */
    struct mirilla_except_predicate predicate;
    /** Recovery action applied after a match. */
    struct mirilla_except_action action;
} MIRILLA_ALIGNED(16);

MIRILLA_ASSERT(MIRILLA_SIZEOF(struct mirilla_except_boundary) == 16, "exception boundary ABI size");
MIRILLA_ASSERT(MIRILLA_SIZEOF(struct mirilla_except_predicate) == 16, "exception predicate ABI "
                                                                      "size");
MIRILLA_ASSERT(MIRILLA_SIZEOF(struct mirilla_except_action_context_extent) == 14, "exception "
                                                                                  "action context "
                                                                                  "extent ABI "
                                                                                  "size");
MIRILLA_ASSERT(MIRILLA_SIZEOF(union mirilla_except_action_context) == 14, "exception action "
                                                                          "context ABI size");
MIRILLA_ASSERT(MIRILLA_SIZEOF(struct mirilla_except_action) == 16, "exception action ABI size");
MIRILLA_ASSERT(MIRILLA_OFFSETOF(struct mirilla_except_action, tag) == 14, "exception action tag "
                                                                          "ABI offset");
MIRILLA_ASSERT(MIRILLA_SIZEOF(struct mirilla_except_record) == 48, "exception record ABI size");
MIRILLA_ASSERT(MIRILLA_ALIGNOF(struct mirilla_except_record) == 16, "exception record ABI "
                                                                    "alignment");

/** Input for creating one fd-owned exception context. */
struct mirilla_except_create_argument {
    /** Requested fixed slab size in bytes. */
    virtual_size_t slab_size;
};

/** Successful result of creating one fd-owned exception context. */
struct mirilla_except_create_result {
    /** Identifier assigned to the new context. */
    mirilla_except_id_t id;
    /** Owned descriptor for the new context. */
    int fd;
};

MIRILLA_EXCEPT_DEFINE_COMMAND_IO(create);

#ifdef __KERNEL__

#include <linux/ftrace.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

#include "mirilla-context.h"
#include "mirilla-slab.h"

struct mirilla_device_context;
struct pt_regs;

/*
 * An immutable kernel snapshot of one published slab.
 *
 * NOTE(invariant): record_list is a complete slab-sized kernel allocation and is immutable after
 * validation. used_count identifies its sorted nonzero prefix. Every reader owns a context
 * reference before accessing either field.
 */
MIRILLA_CONTEXT_DEFINE(
    except_table, struct {
        /** Exact byte size of the immutable snapshot allocation. */
        virtual_size_t size;
        /** Number of records in the validated sorted prefix. */
        virtual_size_t used_count;
        /** Kernel-owned immutable record copy. */
        struct mirilla_except_record *record_list;
        /** Link in the owning exception context's active table list. */
        struct list_head context_node;
    };);

/*
 * The fd-owned exception context permanently bound to one mm.
 *
 * NOTE(invariant): address_space owns one mm_count reference. The registry entry is weak. Every
 * live slab owns one context reference through slab_set. table_lock protects active immutable
 * table membership. The generic slab layer owns VMA storage and publication transitions.
 */
MIRILLA_CONTEXT_DEFINE(
    except, struct {
        /** Identifier exposed to the creating userspace process. */
        mirilla_except_id_t id;
        /** Permanently bound address space. */
        struct mm_struct *address_space;
        /** Generic fixed-slab mapping and publication state. */
        struct mirilla_slab_set slab_set;
        /** Protects active immutable table membership and reference acquisition. */
        raw_spinlock_t table_lock;
        /** Immutable tables currently visible to exception lookup. */
        struct list_head table_list;
        /** Weak registry link for this address space. */
        struct hlist_node registry_node;
    };);

/*
 * One bucket in the weak per-address-space exception registry.
 *
 * NOTE(invariant): Every linked context remains weakly owned by its ordinary references. The lock
 * serializes weak-link lookup, insertion, and removal with reference acquisition.
 */
struct mirilla_except_registry_bucket {
    /** Serializes lookup and weak-link insertion or removal. */
    raw_spinlock_t lock;
    /** Contexts hashed to this bucket. */
    struct hlist_head context_list;
};

/** The fixed-size weak registry used to locate one exception context per address space. */
struct mirilla_except_registry {
    /** Weak context buckets indexed by address-space pointer. */
    struct mirilla_except_registry_bucket bucket_list[MIRILLA_EXCEPT_REGISTRY_BUCKET_COUNT];
};

/** Module-lifetime exception state shared by lookup and the architectural fault hook. */
struct mirilla_except_global_context {
    /** Per-mm weak context registry. */
    struct mirilla_except_registry registry;
    /** Module-lifetime ftrace exception hook. */
    struct ftrace_ops ftrace_operations;
};

extern struct mirilla_except_global_context mirilla_except_context;

/** Validate a boundary and return its exclusive end address. */
bool mirilla_except_boundary_valid(const struct mirilla_except_boundary *boundary,
                                   virtual_address_t *boundary_end);
/** Validate the vector and page-fault predicate fields. */
bool mirilla_except_predicate_valid(const struct mirilla_except_predicate *predicate);
/** Test one predicate against an exception and error code. */
bool mirilla_except_predicate_match(const struct mirilla_except_predicate *predicate,
                                    mirilla_except_mask_t except_mask, unsigned long error_code);
/** Validate an action payload and discriminator. */
bool mirilla_except_action_valid(const struct mirilla_except_action *action);
/** Validate one complete immutable slab snapshot. */
int mirilla_except_table_validate(struct mirilla_except_table_context *table);
/** Detect overlap between two validated immutable snapshots. */
bool mirilla_except_table_conflicts(const struct mirilla_except_table_context *left,
                                    const struct mirilla_except_table_context *right);
/** Apply a validated action to a userspace register frame. */
bool mirilla_except_action_apply(struct pt_regs *user_registers,
                                 const struct mirilla_except_action *action);
/** Search all active tables for one matching recovery action. */
bool mirilla_except_lookup(struct mm_struct *mm, unsigned long instruction_pointer,
                           mirilla_except_mask_t except_mask, unsigned long error_code,
                           struct mirilla_except_action *action);

/** Install the module-lifetime exception hook and context registry. */
int mirilla_except_initialize(void);
/** Remove the module-lifetime exception hook and context registry. */
void mirilla_except_deinitialize(void);
/** Initialize all weak registry buckets. */
int mirilla_except_registry_initialize(void);
/** Check that all weak registry buckets are empty. */
void mirilla_except_registry_deinitialize(void);

/** Handle one exception-context command from the device dispatcher. */
mirilla_command_status_t
mirilla_except_handle_command(struct mirilla_device_context *device_context,
                              mirilla_command_t command, mirilla_command_argument_t argument);

#if defined(MIRILLA_KUNIT)
/** Link a constructed context into the registry for a KUnit lifetime test. */
int mirilla_except_test_registry_insert(struct mirilla_except_context *except_context,
                                        struct mm_struct *address_space);
/** Acquire a context reference through the production registry lookup path. */
struct mirilla_except_context *mirilla_except_test_registry_get(struct mm_struct *address_space);
/** Acquire active table references through the production table traversal path. */
unsigned int mirilla_except_test_active_table_get(struct mirilla_except_context *except_context,
                                                  struct mirilla_except_table_context **table_list);
/** Release table references acquired by mirilla_except_test_active_table_get. */
void mirilla_except_test_active_table_set(struct mirilla_except_table_context **table_list,
                                          unsigned int table_count);
#endif

/** Flags used when creating an exception context descriptor. */
#define MIRILLA_EXCEPT_FILE_FLAGS (O_RDWR | O_CLOEXEC)

#endif

#endif
