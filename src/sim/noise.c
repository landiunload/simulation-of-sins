#include "sim/noise.h"

#include "sim/random.h"
#include "sim/space.h"

/* The largest shift whose lattice cell and remainder still fit a 64-bit word:
 * a shift of 64 would make the 1 << shift masks undefined. */
#define SOS_NOISE_MAX_SHIFT 63u

/* A lattice corner in [-SOS_Q16_ONE, SOS_Q16_ONE): the top 17 bits of the
 * corner's hash spread over the interval. The corner coordinates have already
 * been reduced modulo 2^shift, so they are the same corner for every position
 * in the cell. */
static int64_t Lattice(uint64_t seed, uint64_t x, uint64_t y, uint64_t z)
{
    const uint64_t hash = SosHashCombine(SosHashCombine(SosHashCombine(seed, x), y), z);
    return (int64_t)(hash >> 47u) - (int64_t)SOS_Q16_ONE;
}

/* The position inside its lattice cell, scaled from the cell's 2^shift units
 * to Q16. */
static int64_t Fraction(int64_t coordinate, unsigned shift)
{
    const uint64_t remainder = SosFloorRemainder(coordinate, shift);
    return shift >= SOS_Q16_BITS ? (int64_t)(remainder >> (shift - SOS_Q16_BITS))
                                 : (int64_t)(remainder << (SOS_Q16_BITS - shift));
}

/* 3t^2 - 2t^3 in Q16: the smoothstep that blends the corners. It is flat at
 * each lattice point, so the field is continuous and has no visible seams.
 * Truncating division is exact and portable. */
static int64_t Smooth(int64_t t)
{
    const int64_t squared = t * t / SOS_Q16_ONE;
    return squared * (3 * (int64_t)SOS_Q16_ONE - 2 * t) / SOS_Q16_ONE;
}

/* from + (to - from) * t: `from` at t = 0, `to` at SOS_Q16_ONE. */
static int64_t Lerp(int64_t from, int64_t to, int64_t t)
{
    return from + (to - from) * t / SOS_Q16_ONE;
}

int64_t SosNoise2(uint64_t seed, int64_t x, int64_t y, unsigned shift)
{
    if (shift > SOS_NOISE_MAX_SHIFT)
    {
        return 0;
    }
    const uint64_t cellX = (uint64_t)SosFloorShift(x, shift);
    const uint64_t cellY = (uint64_t)SosFloorShift(y, shift);
    const int64_t sx = Smooth(Fraction(x, shift));
    const int64_t sy = Smooth(Fraction(y, shift));
    const int64_t bottom =
        Lerp(Lattice(seed, cellX, cellY, 0u), Lattice(seed, cellX + 1u, cellY, 0u), sx);
    const int64_t top =
        Lerp(Lattice(seed, cellX, cellY + 1u, 0u), Lattice(seed, cellX + 1u, cellY + 1u, 0u), sx);
    return Lerp(bottom, top, sy);
}

int64_t SosNoise3(uint64_t seed, int64_t x, int64_t y, int64_t z, unsigned shift)
{
    if (shift > SOS_NOISE_MAX_SHIFT)
    {
        return 0;
    }
    const uint64_t cellX = (uint64_t)SosFloorShift(x, shift);
    const uint64_t cellY = (uint64_t)SosFloorShift(y, shift);
    const uint64_t cellZ = (uint64_t)SosFloorShift(z, shift);
    const int64_t sx = Smooth(Fraction(x, shift));
    const int64_t sy = Smooth(Fraction(y, shift));
    const int64_t sz = Smooth(Fraction(z, shift));
    int64_t layers[2];
    for (uint64_t dz = 0u; dz < 2u; ++dz)
    {
        const int64_t bottom = Lerp(Lattice(seed, cellX, cellY, cellZ + dz),
                                    Lattice(seed, cellX + 1u, cellY, cellZ + dz), sx);
        const int64_t top = Lerp(Lattice(seed, cellX, cellY + 1u, cellZ + dz),
                                 Lattice(seed, cellX + 1u, cellY + 1u, cellZ + dz), sx);
        layers[dz] = Lerp(bottom, top, sy);
    }
    return Lerp(layers[0], layers[1], sz);
}
