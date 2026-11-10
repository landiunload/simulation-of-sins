#ifndef SOS_SIM_BIOLOGY_H
#define SOS_SIM_BIOLOGY_H

#include <stdint.h>

/* Shared biology.
 *
 * The few facts that every being shares and that many otherwise unrelated
 * modules reason about: the common fixed-point scale needs and health are
 * measured on, and the two sexes. Lifting them out of beings.h lets the body,
 * lifecycle, names and disease modules depend on this small vocabulary instead
 * of the whole being storage, while beings.h includes this header so its
 * existing users are unchanged. It is declarations only -- a scale and an enum
 * -- so it owns no state and needs no create, step or hash. */

/* Needs and health share one fixed-point scale: 0 is satisfied (for health,
 * dead) and SOS_NEED_MAX is unbearable (for health, perfect). */
#define SOS_NEED_MAX (UINT32_C(1) << 30)

typedef enum SosSex
{
    SOS_SEX_FEMALE = 0,
    SOS_SEX_MALE = 1,
} SosSex;

#endif
