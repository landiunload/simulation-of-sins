#include "sim/space.h"

uint64_t SosIntegerSqrt(uint64_t value)
{
    /* Bit-by-bit square root: no floating point, identical everywhere. */
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value)
    {
        bit >>= 2;
    }
    while (bit != 0u)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}
