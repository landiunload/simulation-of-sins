#include "sim/random.h"

#include <stddef.h>

#define SOS_GOLDEN_GAMMA UINT64_C(0x9E3779B97F4A7C15)

uint64_t SosHash64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31;
    return value;
}

uint64_t SosHashCombine(uint64_t seed, uint64_t value)
{
    return SosHash64(seed ^ SosHash64(value + SOS_GOLDEN_GAMMA));
}

SosRandom SosRandomStream(uint64_t seed, uint64_t stream)
{
    return (SosRandom){.state = SosHashCombine(seed, stream)};
}

uint64_t SosRandomNext(SosRandom *random)
{
    random->state += SOS_GOLDEN_GAMMA;
    return SosHash64(random->state);
}

uint32_t SosRandomBelow(SosRandom *random, uint32_t bound)
{
    if (bound == 0u)
    {
        return 0u;
    }
    /* Lemire's multiply-and-reject: unbiased, usually one draw. */
    uint64_t product = (SosRandomNext(random) >> 32) * (uint64_t)bound;
    uint32_t low = (uint32_t)product;
    if (low < bound)
    {
        const uint32_t threshold = (0u - bound) % bound;
        while (low < threshold)
        {
            product = (SosRandomNext(random) >> 32) * (uint64_t)bound;
            low = (uint32_t)product;
        }
    }
    return (uint32_t)(product >> 32);
}

int64_t SosRandomRange(SosRandom *random, int64_t minimum, int64_t maximum)
{
    if (maximum <= minimum)
    {
        return minimum;
    }
    const uint64_t span = (uint64_t)maximum - (uint64_t)minimum;
    if (span == UINT64_MAX)
    {
        return (int64_t)SosRandomNext(random);
    }
    const uint64_t count = span + 1u;
    /* Rejection keeps the result unbiased for any span. */
    const uint64_t limit = UINT64_MAX - UINT64_MAX % count;
    uint64_t value = SosRandomNext(random);
    while (value >= limit)
    {
        value = SosRandomNext(random);
    }
    return (int64_t)((uint64_t)minimum + value % count);
}

int SosRandomChance(SosRandom *random, uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0u || numerator == 0u)
    {
        return 0;
    }
    if (numerator >= denominator)
    {
        return 1;
    }
    return SosRandomBelow(random, denominator) < numerator;
}
