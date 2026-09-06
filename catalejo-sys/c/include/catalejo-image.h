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
 * The immutable runtime image registered with Mirilla by this layer.
 *
 * NOTE(invariant): The image and every entry point are populated only after both VMAs and their
 * common backing object are immutable. A pointer is published only while a retained Mirilla
 * session owns the registration for the calling address space.
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
 * Construct, register and publish the process-global exception image.
 *
 * The descriptor must belong to Mirilla. This layer opens and retains a dedicated registration
 * session through the supplied descriptor so the returned image remains registered without
 * extending the lifetime of unrelated map commands. A child created through fork must call this
 * again before retrieving or using the inherited image.
 *
 * Returns zero on success and a negative errno value on failure.
 */
extern int catalejo_fault_image_initialize(int target_device,
                                           const struct catalejo_image_runtime **target_runtime);

/**
 * Retrieve the image registered for the calling address space.
 *
 * Returns zero on success and a negative errno value when initialization has not completed or the
 * calling process inherited an image that has not been registered after fork.
 */
extern int catalejo_fault_image_retrieve(const struct catalejo_image_runtime **target_runtime);

#endif /* _CATALEJO_IMAGE_H_ */
