#ifndef _CATALEJO_IMAGE_H_
#define _CATALEJO_IMAGE_H_

#include "catalejo-fault.h"
#include "mirilla-except.h"

#define CATALEJO_IMAGE_MEMFD_NAME "launchpad"

/**
 * The callable entry points projected into the sealed accessor image.
 */
struct catalejo_image_entries {
#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    /** Relocated protected read accessor for this primitive width. */                       \
    catalejo_outcome_t (*CATALEJO_CONCAT(read_, target_typename))(const target_type *,       \
                                                                  target_type *);

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    /** Relocated protected write accessor for this primitive width. */                      \
    catalejo_outcome_t (*CATALEJO_CONCAT(write_, target_typename))(target_type *,            \
                                                                   const target_type *);

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

    /** Relocated Intel UMONITOR accessor. */
    catalejo_faultable_instruction_outcome_t (*monitor_intel_arm)(const uint8_t *);

    /** Relocated Intel UMWAIT accessor. */
    catalejo_faultable_instruction_outcome_t (*monitor_intel_wait)(void);

    /** Relocated AMD MONITORX accessor. */
    catalejo_faultable_instruction_outcome_t (*monitor_amd_arm)(const uint8_t *);

    /** Relocated AMD MWAITX accessor. */
    catalejo_faultable_instruction_outcome_t (*monitor_amd_wait)(void);

    /** Relocated protected byte-copy accessor. */
    catalejo_faultable_copy_outcome_t (*copy)(uint8_t *, const uint8_t *, size_t);
};

/**
 * The initialized runtime image retained by the Rust image proof.
 *
 * NOTE(invariant): The image and every entry point are populated only after both VMAs and their
 * common backing object are immutable.
 */
struct catalejo_image_runtime {
    /** Immutable accessor and exception-table VMAs registered with Mirilla. */
    struct mirilla_except_image image;

    /** Callable entry points rebased into the executable accessor VMA. */
    struct catalejo_image_entries entries;
};

/*
 * Original accessor entry points retained inside the copyable image section.
 */
#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_outcome_t CATALEJO_CONCAT(                        \
        catalejo_image_read_, target_typename)(const target_type *target_source,             \
                                               target_type *target_value);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_outcome_t CATALEJO_CONCAT(                        \
        catalejo_image_write_, target_typename)(target_type * target_value,                  \
                                                const target_type *target_source);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_intel_arm(const uint8_t *target_address);

extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_intel_wait(void);

extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_amd_arm(const uint8_t *target_address);

extern CATALEJO_FAULT_ROUTINE catalejo_faultable_instruction_outcome_t
catalejo_image_monitor_amd_wait(void);

extern CATALEJO_FAULT_ROUTINE catalejo_faultable_copy_outcome_t
catalejo_image_copy(uint8_t *target_address, const uint8_t *target_source, size_t target_count);

/**
 * Construct one sealed accessor runtime into caller-owned storage.
 *
 * No process-global initialization state is maintained by this layer. The caller owns both the
 * one-time construction policy and the returned runtime storage.
 */
extern catalejo_outcome_t
catalejo_fault_image_initialize(struct catalejo_image_runtime *target_runtime);

#endif /* _CATALEJO_IMAGE_H_ */
