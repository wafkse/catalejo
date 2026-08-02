#ifndef _CATALEJO_SECTION_H_
#define _CATALEJO_SECTION_H_

#include "catalejo-macro.h"

#define CATALEJO_FAULT_SECTION_BASE catalejo_fault

#define CATALEJO_FAULT_FIXUP_SECTION_BASE CATALEJO_CONCAT(CATALEJO_FAULT_SECTION_BASE, _fixup)

#define CATALEJO_FAULT_SECTION_NAME CATALEJO_STR(CATALEJO_FAULT_SECTION_BASE)

#define CATALEJO_FAULT_FIXUP_SECTION_NAME CATALEJO_STR(CATALEJO_FAULT_FIXUP_SECTION_BASE)

#define CATALEJO_FAULT_SECTION_BOUNDARY_START CATALEJO_CONCAT(__start_, CATALEJO_FAULT_SECTION_BASE)
#define CATALEJO_FAULT_SECTION_BOUNDARY_STOP CATALEJO_CONCAT(__stop_, CATALEJO_FAULT_SECTION_BASE)

#define CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_START \
    CATALEJO_CONCAT(__start_, CATALEJO_FAULT_FIXUP_SECTION_BASE)
#define CATALEJO_FAULT_FIXUP_SECTION_BOUNDARY_STOP \
    CATALEJO_CONCAT(__stop_, CATALEJO_FAULT_FIXUP_SECTION_BASE)

#if __has_attribute(retain)
#define CATALEJO_RETAIN __attribute__((retain))
#elif defined(__GNUC__) && __GNUC__ >= 11
#define CATALEJO_RETAIN __attribute__((retain))
#else
#error "Catalejo requires compiler support for __attribute__((retain))"
#endif

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

#endif /* ifndef _CATALEJO_SECTION_H_ */
