/*
 * NOTE(docs): Module documentation is pending.
 */

#ifndef _MIRILLA_MAP_H_
#define _MIRILLA_MAP_H_

#include "mirilla-command.h" // IWYU pragma: export
#include "mirilla-context.h" // IWYU pragma: export
#include "mirilla-id.h"
#include "mirilla-list.h"

/* These are required for userspace bindings. */
#ifndef __KERNEL__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#endif /* __KERNEL__ */

#ifdef __KERNEL__

#include <linux/mm_types.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "mirilla-device.h"

/*
 * Required forward-declarations for MMU notifier operations.
 */
extern bool
mirilla_map_target_notificate_invalidate_range(struct mmu_interval_notifier *target_subscribe,
                                               const struct mmu_notifier_range *range,
                                               unsigned long sequence_count);

/*
 * The MMU notifier operation table and operation declaration.
 */
static const struct mmu_interval_notifier_ops mirilla_peephole_mmu_interval_notifier_operations = {
    .invalidate = mirilla_map_target_notificate_invalidate_range,
};

/*
 * Required forward-declarations for VMA operations.
 */
extern vm_fault_t mirilla_map_peephole_vm_fault(struct vm_fault *vmf);
extern void mirilla_map_peephole_vm_open(struct vm_area_struct *vma);
extern void mirilla_map_peephole_vm_close(struct vm_area_struct *vma);
extern int mirilla_map_peephole_vm_mremap(struct vm_area_struct *vma);
extern int mirilla_map_peephole_vm_mprotect(struct vm_area_struct *vma, unsigned long start,
                                            unsigned long end, unsigned long newflags);

/*
 * The VMA operation table and operation declaration.
 */
static const struct vm_operations_struct mirilla_map_peephole_vm_operations = {
    .fault = mirilla_map_peephole_vm_fault,
    .open = mirilla_map_peephole_vm_open,
    .close = mirilla_map_peephole_vm_close,
    .mremap = mirilla_map_peephole_vm_mremap,
    .mprotect = mirilla_map_peephole_vm_mprotect
};

/*
 * Required forward-declarations for the file operations.
 */
extern int mirilla_map_peephole_file_release(struct inode *ino, struct file *file);
extern int mirilla_map_peephole_file_mmap(struct file *file, struct vm_area_struct *vma);

static const struct file_operations mirilla_map_peephole_file_operations = {
    .release = mirilla_map_peephole_file_release,
    .mmap = mirilla_map_peephole_file_mmap,
};

#endif /* __KERNEL__ */

/*
 * The top-level command category for map-related functionality.
 */
#define MIRILLA_COMMAND_CATEGORY_MAP 0x00

#define MIRILLA_MAP_COMMANDS      \
    X(ENGAGE, 0x00, engage)       \
    X(DISENGAGE, 0x01, disengage) \
    X(PEEPHOLE, 0x02, peephole)   \
    X(ADDRESS_SPACE_LAYOUT, 0x03, address_space_layout)

#define X(name, val, io) MIRILLA_COMMAND_MAP_##name = val,
enum { MIRILLA_MAP_COMMANDS MIRILLA_MAP_NR_COMMANDS };
#undef X

#define X(name, val, io) "MIRILLA_COMMAND_MAP_" #name,
__attribute((
    unused)) static const char *MIRILLA_COMMAND_NAME_MAP_TABLE[] = { MIRILLA_MAP_COMMANDS NULL };
#undef X

/*
 * Determine the human-readable name of a map command.
 *
 * This is the command itself, not the full command integer.
 */
#define MIRILLA_COMMAND_NAME_MAP(map_command) (MIRILLA_COMMAND_NAME_MAP_TABLE[map_command])

#define MIRILLA_MAP_CONTEXT_LIST \
    X(target)                    \
    X(peephole)

#define MIRILLA_MAP_ID_TYPE_DECLARE(type_name) typedef mirilla_id_t mirilla_map_##type_name##_id_t

/*
 * Identifier type definitions for context record structures.
 */
#define X(context_name) MIRILLA_MAP_ID_TYPE_DECLARE(context_name);
MIRILLA_MAP_CONTEXT_LIST
#undef X

#define MIRILLA_MAP_DEFINE_COMMAND_IO(name)            \
    union mirilla_map_##name##_io {                    \
        struct mirilla_map_##name##_argument argument; \
        struct mirilla_map_##name##_result result;     \
    };                                                 \
    MIRILLA_ASSERT_IO_SIZE(map_##name)

struct mirilla_map_engage_argument {
    /*
   * The process identifier to engage with.
   */
    pid_t process_id;
};

struct mirilla_map_engage_result {
    /*
   * The internal observed process identifier associated with the observed
   * process.
   *
   * This is to be used for further references to the observed process.
   */
    mirilla_map_target_id_t target_id;
};

MIRILLA_MAP_DEFINE_COMMAND_IO(engage);

struct mirilla_map_disengage_argument {
    mirilla_map_target_id_t target_id;
};

struct mirilla_map_disengage_result {};

MIRILLA_MAP_DEFINE_COMMAND_IO(disengage);

/**
 * An initialization word for peephole creation.
 *
 * This is used to inform of specific preferences to the kernel when it creates the peephole.
 */
typedef uint32_t mirilla_map_peephole_initialize_word_t;

/*
 * Populate the `struct vm_area_struct` on a `mmap`.
 *
 * This may increase the latency of a `mmap` system call for a `peephole` file descriptor.
 */
#define MIRILLA_MAP_PEEPHOLE_INITIALIZE_POPULATE (0x0001)

/*
 * The number of observed frames a single populate iteration pins in one remote
 * pin. Batching amortizes the `get_user_pages_remote` page-table walk and the
 * observed `mmap_lock` round trip over many granules rather than paying both per
 * granule the way the demand-fault path does.
 *
 * WARNING: This is used to hold an array on the stack, keep the size small.
 */
#define MIRILLA_MAP_PEEPHOLE_POPULATE_ITERATION_SIZE (64)

struct mirilla_map_peephole_argument {
    /*
   * The monotonic identifier to the target observed virtual address space.
   *
   * This must have been acquired for the same
   */
    mirilla_map_target_id_t target_id;

    /*
   * The start and end addresses of the peephole.
   */
    virtual_address_t start_address, end_address;

    /**
     * The initialization word for the peephole about to be created.
     *
     * This is a bitset, populated by the `MIRILLA_MAP_PEEPHOLE_INITIALIZE_*`
     * family of flags so that a caller can request one-shot creation-time
     * behavior without a separate command.
     */
    mirilla_map_peephole_initialize_word_t initialize_word;
};

struct mirilla_map_peephole_result {
    /*
   * The monotonic identifier associated with this just-created peephole.
   */
    mirilla_map_peephole_id_t id;

    /*
   * The file descriptor that has been exposed to userspace.
   *
   * This will require to be memory-mapped via `mmap` to actually access the
   * underlying memory.
   */
    int fd;
};

MIRILLA_MAP_DEFINE_COMMAND_IO(peephole);

#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_READ (1U << 0)
#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_WRITE (1U << 1)
#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_EXEC (1U << 2)

#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_ANONYMOUS (1U << 3)
#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_SHARED (1U << 4)
#define MIRILLA_MAP_LAYOUT_ATTRIBUTE_STACK (1U << 5)

/*
 * The attributes to a layout component.
 *
 * Described by the `MIRILLA_MAP_LAYOUT_ATTRIBUTE_*` macro family.
 *
 * Each bit corresponds to a kernel VMA flag or predicate as follows:
 *
 * * `READ`:      `vm_flags & VM_READ`.
 * * `WRITE`:     `vm_flags & VM_WRITE`.
 * * `EXEC`:      `vm_flags & VM_EXEC`.
 * * `ANONYMOUS`: `vma_is_anonymous(area)`, as the VMA has no `vm_ops`. This
 *   covers private anonymous mappings (`MAP_PRIVATE | MAP_ANONYMOUS`).
 *   Shared anonymous mappings (`MAP_SHARED | MAP_ANONYMOUS`) are backed by
 *   anonymous shmem and carry `vm_ops`, so they are reported as `SHARED`
 *   but not `ANONYMOUS`.
 * * `SHARED`:    `vm_flags & VM_SHARED`.
 * * `STACK`:     `vm_flags & VM_GROWSDOWN`. The VMA grows downward, which
 *   is the kernel's marker for stack (and guard) mappings.
 */
typedef uint32_t mirilla_map_layout_attributes_t;

/**
 * A structure that describes a portion of the address space.
 */
struct mirilla_map_address_space_layout {
    /**
     * The start and end virtual address pair.
     */
    virtual_address_t start_address, end_address;

    /**
     * The attribute list for this portion of the address space.
     */
    mirilla_map_layout_attributes_t attribute_list;
};

/*
 * The type of an auxiliary vector entry.
 */
typedef uint64_t mirilla_auxiliary_vector_type_t;

/*
 * The value of an auxiliary vector entry.
 */
typedef uint64_t mirilla_auxiliary_vector_value_t;

/**
 * A mirilla-provided auxiliary vector entry.
 *
 * NOTE(bitness): On an observed `32-bit` target, the respective type-entry auxiliary entry pair
 * are widened to 64-bit integers.
 */
struct mirilla_auxiliary_vector_entry {
    /**
     * The auxiliary vector type.
     */
    mirilla_auxiliary_vector_type_t entry_type;

    /**
     * The auxiliary vector value.
     */
    mirilla_auxiliary_vector_value_t entry_value;
};

#if defined(__KERNEL__) && (defined(CONFIG_X86_64) || defined(CONFIG_ARM64))

/**
 * NOTE: Must match the size of two `saved auxv` entries.
 *
 * Only asserted on `x86_64` for the time being.
 */
static_assert(sizeof(struct mirilla_auxiliary_vector_entry) ==
              2 * sizeof(typeof(((struct mm_struct *)NULL)->saved_auxv[AT_VECTOR_SIZE])));

#endif

/**
 * Useful metadata used for an initial explore of the foreign address space.
 */
struct mirilla_map_address_space_metadata {
    /**
     * The virtual address range of the process environment.
     */
    virtual_address_t environment_start, environment_end;

    /**
     * The virtual address range of the process argument list.
     */
    virtual_address_t argument_start, argument_end;
};

struct mirilla_map_address_space_layout_argument {
    /**
     * The monotonic identifier of the engaged target whose address space is to
     * be described.
     *
     * This must have been acquired via a `MAP` `ENGAGE` command on the same
     * device session.
     */
    mirilla_map_target_id_t target_id;

    /**
     * The outside-pointer to populate with the address space layout information.
     */
    MIRILLA_OUTSIDE_LIST_TYPE(struct mirilla_map_address_space_layout)
    struct mirilla_outside_list layout_list;

    /**
     * The outside-pointer to populate with the kernel-resident auxiliary vector.
     */
    MIRILLA_OUTSIDE_LIST_TYPE(struct mirilla_map_auxiliary_vector_entry)
    struct mirilla_outside_list auxiliary_vector_list;
};

struct mirilla_map_address_space_layout_result {
    /**
     * Kernel-resident metadata of the address space whose layout was requested.
     */
    struct mirilla_map_address_space_metadata metadata;

    /*
     * The outcome of the provided `layout-list`.
     */
    struct mirilla_outside_list_outcome layout_outcome;

    /*
     * The outcome of the provided `auxiliary-vector-list`.
     */
    struct mirilla_outside_list_outcome auxiliary_vector_outcome;
};

MIRILLA_MAP_DEFINE_COMMAND_IO(address_space_layout);

#ifdef __KERNEL__

/*
 * Declare all command handler functions.
 */
#define X(name, val, io)                                             \
    extern mirilla_command_status_t mirilla_map_handle_command_##io( \
        struct mirilla_device_context *device_context, union mirilla_map_##io##_io *io);
MIRILLA_MAP_COMMANDS
#undef X

MIRILLA_CONTEXT_DEFINE(
    map_target, struct {
        /*
       * The identifier assigned to this map target.
       */
        mirilla_map_target_id_t id;

        /*
       * The process pid that is deemed the target.
       */
        struct pid *process_id;

        /*
       * The peephole atomic counter associated to this map target.
       */
        mirilla_atomic_id_t peephole_count;

        /*
       *  The Read-Copy-Update callback for this map target.
       */
        struct rcu_head teardown_callback;
    };)

/*
 * The states of a peephole status word.
 */
typedef enum {
    MIRILLA_PEEPHOLE_STATE_ALIVE,
    MIRILLA_PEEPHOLE_STATE_DEAD,
    NR_MIRILLA_PEEPHOLE_STATES
} mirilla_peephole_state_variant_t;

/*
 * The atomic status word of a peephole.
 */
typedef atomic_t mirilla_peephole_state_t;

/*
 * Check against missmatching enum and state sizes.
 */
static_assert(sizeof(mirilla_peephole_state_variant_t) == sizeof(mirilla_peephole_state_t));

MIRILLA_CONTEXT_DEFINE(
    map_peephole, struct {
        /*
       * The identifier assigned to this peephole.
       *
       * This is unique inside the same map target.
       */
        mirilla_map_peephole_id_t id;

        /*
       * The state of the peephole.
       */
        mirilla_peephole_state_t peephole_state;

        /**
         * The peephole word that dictates one-shot behavior of the peephole in specific circumstances.
         */
        mirilla_map_peephole_initialize_word_t peephole_word;

        /*
       * The anonymous-inode-backed file used for the peephole.
       *
       * The private data of this file is this same peephole context.
       *
       * This does hold a strong reference to the context.
       */
        struct file *file;

        /*
       * The start and end addresses of the peephole.
       */
        virtual_address_t start_address, end_address;

        /**
       * The `mmu_interval_notifier` registered against the `mm_struct` the
       * peephole is mapping against.
       */
        struct mmu_interval_notifier interval_subscribe;

        /*
       * The observed foreign address space.
       */
        struct mm_struct *address_space;

        /*
       * NOTE(lock): Serializes peephole-VMA PTE installs against the
       * interval-notifier invalidate callback. The fault path holds it
       * across `mmu_interval_read_retry()` and the `vmf_insert_mixed()`
       * install, and the invalidate callback holds it across
       * `mmu_interval_set_seq()` and the PTE and pin teardown. This is the
       * user-provided lock the `mmu_interval_read_*` API mandates on both
       * sides so a racing install cannot slip past a collision. A plain
       * mutex suffices because `pin_user_pages_remote()` runs outside it
       * and contention is low.
       */
        struct mutex install_lock;

        /*
       *  The Read-Copy-Update callback for this peephole.
       */
        struct rcu_head teardown_callback;
    };);

/*
 * Declare context-specific reference-counting helper functions.
 */
#define X(context_name) extern MIRILLA_CONTEXT_REFERENCE_GET_DEFINE(map_##context_name);
MIRILLA_MAP_CONTEXT_LIST
#undef X
#define X(context_name) extern MIRILLA_CONTEXT_REFERENCE_SET_DEFINE(map_##context_name);
MIRILLA_MAP_CONTEXT_LIST
#undef X

/*
 * Map command handler dispatcher.
 */
mirilla_command_status_t mirilla_map_handle_command(struct mirilla_device_context *device_context,
                                                    mirilla_command_t command,
                                                    mirilla_command_argument_t argument);

/*
 * NOTE(security): The minimum capability set an engagement author must hold.
 * `CAP_SYS_PTRACE` is appropriate because it already grants the same access
 * scope, namely unfettered `process_vm_{read,write}v` on arbitrary processes.
 */
#define MIRILLA_MAP_ENGAGE_CAPABILITIES (CAP_SYS_PTRACE)

/*
 * NOTE(security): Disable engagement capability checking wholesale.
 *
 * This is discouraged for use, and is intended for debugging purposes only.
 */
#define MIRILLA_MAP_ENGAGE_IGNORE_CAPABILITIES (1)

#define MIRILLA_MAP_FILE_FLAGS (O_RDWR | O_CLOEXEC)

#endif /* __KERNEL__ */

#endif /* _MIRILLA_MAP_H_ */
