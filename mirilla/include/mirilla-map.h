/*
 * NOTE(docs): Module documentation is pending.
 */

#ifndef _MIRILLA_MAP_H_
#define _MIRILLA_MAP_H_

#include "mirilla-command.h" // IWYU pragma: export
#include "mirilla-context.h" // IWYU pragma: export
#include "mirilla-id.h"

/* These are required for userspace bindings. */
#ifndef __KERNEL__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#endif /* __KERNEL__ */

#ifdef __KERNEL__

#include "mirilla-device.h"

#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

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
    X(PEEPHOLE, 0x02, peephole)

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

#ifdef __KERNEL__

/*
 * NOTE(invariant): Avoid multi-page `copy_{to,from}_user` for input-output
 * intermediate structures.
 */
#define MIRILLA_ASSERT_IO_SIZE(name) \
    static_assert(sizeof(union mirilla_map_##name##_io) <= PAGE_SIZE, "IO too large: " #name)

#else

/*
 * NOTE(workaround): Bindgen dislikes `static_assert`.
 */
#define MIRILLA_ASSERT_IO_SIZE(name)

#endif /* __KERNEL__ */

#define MIRILLA_MAP_DEFINE_COMMAND_IO(name)            \
    union mirilla_map_##name##_io {                    \
        struct mirilla_map_##name##_argument argument; \
        struct mirilla_map_##name##_result result;     \
    };                                                 \
    MIRILLA_ASSERT_IO_SIZE(name)

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
 * An integer primitive capable of representing a virtual address.
 */
typedef size_t virtual_address_t;

/**
 * An initialization word for peephole creation.
 *
 * This is used to inform of specific preferences to the kernel when it creates the peephole.
 */
typedef unsigned short mirilla_map_peephole_initialize_word_t;

/**
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
 * NOTE(security): The minimum capability an engagement author must hold.
 * `CAP_SYS_PTRACE` is appropriate because it already grants the same access
 * scope, namely unfettered `process_vm_{read,write}v` on arbitrary processes.
 */
#define MIRILLA_MAP_ENGAGE_CAPABILITIES (CAP_SYS_PTRACE)

#define MIRILLA_MAP_FILE_FLAGS (O_RDWR | O_CLOEXEC)

#endif /* __KERNEL__ */

#endif /* _MIRILLA_MAP_H_ */
