#ifndef _CATALEJO_SECTION_H_
#define _CATALEJO_SECTION_H_

#include "catalejo-macro.h"

#define CATALEJO_FAULT_SECTION_BASE catalejo_fault

#define CATALEJO_FAULT_SECTION_NAME CATALEJO_STR(CATALEJO_FAULT_SECTION_BASE)

#define CATALEJO_FAULT_SECTION_BOUNDARY_START                                  \
  CATALEJO_CONCAT(__start_, CATALEJO_FAULT_SECTION_BASE)
#define CATALEJO_FAULT_SECTION_BOUNDARY_STOP                                   \
  CATALEJO_CONCAT(__stop_, CATALEJO_FAULT_SECTION_BASE)

#define FAULT_ROUTINE                                                          \
  __attribute__((noinline)) __attribute__((sysv_abi)) __attribute__((naked))   \
  __attribute__((section(CATALEJO_FAULT_SECTION_NAME)))

/*
 * The start of the fault section boundary.
 */
extern char CATALEJO_FAULT_SECTION_BOUNDARY_START[];

/*
 * The stop of the fault section boundary.
 */
extern char CATALEJO_FAULT_SECTION_BOUNDARY_STOP[];

#endif /* ifndef _CATALEJO_SECTION_H_ */
