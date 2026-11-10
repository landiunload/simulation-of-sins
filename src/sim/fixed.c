#include "sim/fixed.h"

#include <stdbool.h>

/* |numerator| and |denominator| below this keep numerator * 2^16 in 64 bits. */
#define EXACT_LIMIT (INT64_C(1) << 47)
/* Quotients beyond 2^15 do not fit a Q16 value. */
#define WHOLE_LIMIT (INT64_C(1) << 15)
#define SEGMENT_BITS 4u
#define SEGMENT_FRACTION_BITS (SOS_Q16_BITS - SEGMENT_BITS)

/* 2^(-k/16) in Q16 for k = 0..16, rounded to nearest. Interpolating between
 * them stays within 0.1% of the exact curve. */
static const uint32_t halving[(1u << SEGMENT_BITS) + 1u] = {
    65536u, 62757u, 60097u, 57549u, 55109u, 52773u, 50535u, 48393u, 46341u,
    44376u, 42495u, 40693u, 38968u, 37316u, 35734u, 34219u, 32768u,
};

static SosQ16 ClampToQ16(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : (SosQ16)value);
}

SosQ16 SosQ16Ratio(int64_t numerator, int64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }
    if (numerator == INT64_MIN && denominator == -1)
    {
        return INT32_MAX;
    }
    const int64_t whole = numerator / denominator;
    if (whole >= WHOLE_LIMIT || whole <= -WHOLE_LIMIT)
    {
        return whole > 0 ? INT32_MAX : INT32_MIN;
    }
    const int64_t remainder = numerator % denominator;
    const bool exact = denominator < EXACT_LIMIT && denominator > -EXACT_LIMIT;
    const int64_t fraction =
        exact ? remainder * SOS_Q16_ONE / denominator : remainder / (denominator / SOS_Q16_ONE);
    return ClampToQ16(whole * SOS_Q16_ONE + fraction);
}

int64_t SosDecay(int64_t value, uint64_t elapsed, uint64_t halfLife)
{
    if (elapsed == 0u || value == 0)
    {
        return value;
    }
    if (halfLife == 0u)
    {
        return 0;
    }
    const uint64_t halves = elapsed / halfLife;
    if (halves >= 64u)
    {
        return 0;
    }
    const uint64_t remainder = elapsed % halfLife;
    uint64_t fraction = halfLife < (UINT64_C(1) << 47) ? (remainder << SOS_Q16_BITS) / halfLife
                                                       : remainder / (halfLife >> SOS_Q16_BITS);
    if (fraction >= (uint64_t)SOS_Q16_ONE)
    {
        fraction = (uint64_t)SOS_Q16_ONE - 1u;
    }
    const uint32_t segment = (uint32_t)(fraction >> SEGMENT_FRACTION_BITS);
    const uint64_t within = fraction & ((UINT64_C(1) << SEGMENT_FRACTION_BITS) - 1u);
    const uint64_t factor =
        halving[segment] -
        ((halving[segment] - halving[segment + 1u]) * within >> SEGMENT_FRACTION_BITS);

    /* The magnitude in unsigned arithmetic, so INT64_MIN decays too. */
    const uint64_t magnitude = value < 0 ? 0u - (uint64_t)value : (uint64_t)value;
    const uint64_t halved = magnitude >> halves;
    /* halved * factor / 2^16 without overflowing 64 bits. */
    const uint64_t scaled =
        (halved >> SOS_Q16_BITS) * factor +
        (((halved & ((UINT64_C(1) << SOS_Q16_BITS) - 1u)) * factor) >> SOS_Q16_BITS);
    if (scaled > (uint64_t)INT64_MAX)
    {
        /* Only INT64_MIN a moment later: too little time to change it. */
        return value;
    }
    return value < 0 ? -(int64_t)scaled : (int64_t)scaled;
}
