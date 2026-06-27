#ifndef _CATALEJO_MACRO_H_
#define _CATALEJO_MACRO_H_

#define _CATALEJO_STR(x) #x
#define CATALEJO_STR(x) _CATALEJO_STR(x)

#define _CATALEJO_CONCAT(a, b) a##b
#define CATALEJO_CONCAT(a, b) _CATALEJO_CONCAT(a, b)

#define CATALEJO_USED __attribute__((used))
#define CATALEJO_UNUSED __attribute__((unused))

/**
 * The outcome type for the `catalejo-fault` API.
 */
typedef enum catalejo_faultable_outcome {
  CATALEJO_OUTCOME_SUCCESS,
  CATALEJO_OUTCOME_ERROR = -1,
  CATALEJO_OUTCOME_INVALID_VALUE = -2,
} catalejo_faultable_outcome_t;

#endif /* ifndef _CATALEJO_MACRO_H_ */
