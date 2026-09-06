#ifndef _CATALEJO_IMAGE_H_
#define _CATALEJO_IMAGE_H_

#include "catalejo-fault.h"
#include "mirilla-except.h"

/**
 * The callable entry points projected into the sealed accessor image.
 */
struct catalejo_image_entries {
#define X(target_typename, target_type, target_mnemonic, target_register, target_register32)     \
    catalejo_faultable_outcome_t (*CATALEJO_CONCAT(read_, target_typename))(const target_type *, \
                                                                            target_type *);

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    catalejo_faultable_outcome_t (*CATALEJO_CONCAT(write_, target_typename))(target_type *,  \
                                                                             const target_type *);

    CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

    catalejo_faultable_instruction_outcome_t (*monitor_intel_arm)(const uint8_t *);
    catalejo_faultable_instruction_outcome_t (*monitor_intel_wait)(void);
    catalejo_faultable_instruction_outcome_t (*monitor_amd_arm)(const uint8_t *);
    catalejo_faultable_instruction_outcome_t (*monitor_amd_wait)(void);
    catalejo_faultable_copy_outcome_t (*copy)(uint8_t *, const uint8_t *, size_t);
};

/**
 * The initialized process-global runtime image.
 *
 * NOTE(invariant): The image and every entry point become visible only after both VMAs and their
 * common backing object are immutable.
 */
struct catalejo_image_runtime {
    struct mirilla_except_image image;
    struct catalejo_image_entries entries;
};

/*
 * Original accessor entry points retained inside the copyable image section.
 */
#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(              \
        catalejo_image_read_, target_typename)(const target_type *target_source,             \
                                               target_type *target_value);

CATALEJO_FAULT_ROUTINE_SPECIFICATION
#undef X

#define X(target_typename, target_type, target_mnemonic, target_register, target_register32) \
    extern CATALEJO_FAULT_ROUTINE catalejo_faultable_outcome_t CATALEJO_CONCAT(              \
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
 * Construct or retrieve the process-global sealed accessor image.
 *
 * A negative return value is an errno-style failure. The output may be null when a caller only
 * needs to ensure that initialization succeeded.
 */
extern int catalejo_fault_image_initialize(struct mirilla_except_image *target_image);

#endif /* _CATALEJO_IMAGE_H_ */
