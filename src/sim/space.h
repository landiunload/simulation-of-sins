#ifndef SOS_SIM_SPACE_H
#define SOS_SIM_SPACE_H

#include <stdint.h>

/* World space. A position is three signed 64-bit integers in units of 1/1024
 * metre, Z pointing up. The range is about nine quadrillion metres from the
 * origin on every axis, so the world is unbounded for every practical purpose,
 * and integer arithmetic gives the same result on every machine. Presentation
 * converts to floats only relative to a nearby origin. */

#define SOS_UNITS_PER_METRE 1024
#define SOS_METRES(metres) ((int64_t)(metres) * SOS_UNITS_PER_METRE)

typedef struct SosPosition
{
    int64_t x;
    int64_t y;
    int64_t z;
} SosPosition;

/* floor(value / 2^shift) for any value, without relying on how the compiler
 * shifts negative numbers. */
static inline int64_t SosFloorShift(int64_t value, unsigned shift)
{
    if (value >= 0)
    {
        return (int64_t)((uint64_t)value >> shift);
    }
    /* -1 - floor((-value - 1) / 2^shift), computed in unsigned arithmetic so
     * INT64_MIN is also exact. */
    const uint64_t magnitude = (uint64_t)(-(value + 1));
    return -1 - (int64_t)(magnitude >> shift);
}

/* value - floor(value / 2^shift) * 2^shift, in [0, 2^shift). */
static inline uint64_t SosFloorRemainder(int64_t value, unsigned shift)
{
    return (uint64_t)value & ((UINT64_C(1) << shift) - 1u);
}

/* floor(sqrt(value)), exact for every 64-bit input. */
uint64_t SosIntegerSqrt(uint64_t value);

#endif
