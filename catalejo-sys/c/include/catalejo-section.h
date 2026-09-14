#ifndef _CATALEJO_SECTION_H_
#define _CATALEJO_SECTION_H_

#include "catalejo-macro.h"

/** Base token used to name the linked protected-routine section. */
#define CATALEJO_FAULT_SECTION_BASE catalejo_fault

/** Base token used to name the linked rollback-record section. */
#define CATALEJO_FAULT_FIXUP_SECTION_BASE CATALEJO_CONCAT(CATALEJO_FAULT_SECTION_BASE, _fixup)

/** String name of the linked protected-routine section. */
#define CATALEJO_FAULT_SECTION_NAME CATALEJO_STR(CATALEJO_FAULT_SECTION_BASE)

/** String name of the linked rollback-record section. */
#define CATALEJO_FAULT_FIXUP_SECTION_NAME CATALEJO_STR(CATALEJO_FAULT_FIXUP_SECTION_BASE)

/** Linker-generated start symbol for the protected-routine section. */
#define CATALEJO_FAULT_SECTION_BOUNDARY_START CATALEJO_CONCAT(__start_, CATALEJO_FAULT_SECTION_BASE)
/** Linker-generated stop symbol for the protected-routine section. */
#define CATALEJO_FAULT_SECTION_BOUNDARY_STOP CATALEJO_CONCAT(__stop_, CATALEJO_FAULT_SECTION_BASE)

/** Linker-generated start symbol for the rollback-record section. */
#define CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START \
    CATALEJO_CONCAT(__start_, CATALEJO_FAULT_FIXUP_SECTION_BASE)
/** Linker-generated stop symbol for the rollback-record section. */
#define CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP \
    CATALEJO_CONCAT(__stop_, CATALEJO_FAULT_FIXUP_SECTION_BASE)

#if __has_attribute(retain)
/** Retain a linked symbol despite section garbage collection. */
#define CATALEJO_RETAIN __attribute__((retain))
#elif defined(__GNUC__) && __GNUC__ >= 11
/** Retain a linked symbol despite section garbage collection. */
#define CATALEJO_RETAIN __attribute__((retain))
#else
#error "Catalejo requires compiler support for __attribute__((retain))"
#endif

/** Define one retained naked protected routine in the dedicated text section. */
#define CATALEJO_FAULT_ROUTINE                                                                 \
    __attribute__((noinline)) __attribute__((sysv_abi)) __attribute__((naked)) CATALEJO_RETAIN \
        __attribute__((section(CATALEJO_FAULT_SECTION_NAME)))

/*
 * The start of the fault section boundary.
 */
extern char CATALEJO_FAULT_SECTION_BOUNDARY_START[];

/*
 * The stop of the fault section boundary.
 */
extern char CATALEJO_FAULT_SECTION_BOUNDARY_STOP[];

/*
 * The start of the fault fixup section boundary.
 */
extern char CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START[];

/*
 * The stop of the fault fixup section boundary.
 */
extern char CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP[];

/* Force extraction of the linked routine object from static archives. */
extern void catalejo_fault_routines_retain(void);

#endif /* ifndef _CATALEJO_SECTION_H_ */
