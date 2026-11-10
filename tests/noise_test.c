#include "sim/noise.h"
#include "sim/space.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "noise test failed: %s\n", message);
        exit(1);
    }
}

static int64_t Absolute(int64_t value)
{
    return value < 0 ? -value : value;
}

/* Every field is a Q16 value inside [-1, 1], whatever the position, however
 * extreme, and whatever the lattice. */
static void TestRange(void)
{
    const unsigned shifts[] = {0u, 1u, 8u, 16u, 33u, 63u};
    const int64_t coordinates[] = {0,
                                   1,
                                   -1,
                                   SOS_METRES(1),
                                   -SOS_METRES(1),
                                   INT64_C(1) << 40,
                                   -(INT64_C(1) << 40),
                                   INT64_MAX,
                                   INT64_MIN};
    for (size_t shift = 0u; shift < sizeof(shifts) / sizeof(shifts[0]); ++shift)
    {
        for (size_t i = 0u; i < sizeof(coordinates) / sizeof(coordinates[0]); ++i)
        {
            for (size_t j = 0u; j < sizeof(coordinates) / sizeof(coordinates[0]); ++j)
            {
                const int64_t value2 =
                    SosNoise2(SEED, coordinates[i], coordinates[j], shifts[shift]);
                const int64_t value3 =
                    SosNoise3(SEED, coordinates[i], coordinates[j], coordinates[i], shifts[shift]);
                Expect(Absolute(value2) <= SOS_Q16_ONE, "2D noise is within one");
                Expect(Absolute(value3) <= SOS_Q16_ONE, "3D noise is within one");
            }
        }
    }
}

/* Neighbouring points give neighbouring values: the smoothstep keeps the field
 * continuous, so a step of a cell's 1/64 moves it by little. A discontinuous
 * hash would jump by the whole range instead. */
static void TestContinuity(void)
{
    /* The smoothstep slope peaks at 3/2, so within a cell's 1/64 the value
     * moves by at most 3/2 * 2 * SOS_Q16_ONE / 64 = 3072. */
    const int64_t limit = 4096;
    for (unsigned shift = 12u; shift <= 20u; shift += 4u)
    {
        const int64_t step = (INT64_C(1) << shift) / 64;
        int64_t previous2 = SosNoise2(SEED, 0, 0, shift);
        int64_t previous3 = SosNoise3(SEED, 0, 0, 0, shift);
        int64_t lowest = previous2;
        int64_t highest = previous2;
        for (int64_t index = 1; index <= 1024; ++index)
        {
            const int64_t coordinate = index * step;
            const int64_t value2 = SosNoise2(SEED, coordinate, -coordinate, shift);
            const int64_t value3 = SosNoise3(SEED, 0, 0, coordinate, shift);
            Expect(Absolute(value2 - previous2) < limit, "2D noise changes smoothly");
            Expect(Absolute(value3 - previous3) < limit, "3D noise changes smoothly");
            previous2 = value2;
            previous3 = value3;
            lowest = value2 < lowest ? value2 : lowest;
            highest = value2 > highest ? value2 : highest;
        }
        Expect(highest - lowest > SOS_Q16_ONE / 4, "the field varies over a few cells");
    }
}

/* Two dimensions are the z = 0 slice of the three-dimensional field. */
static void TestSlice(void)
{
    for (int64_t x = -2000; x <= 2000; x += 500)
    {
        Expect(SosNoise2(SEED, x, -x, 16u) == SosNoise3(SEED, x, -x, 0, 16u),
               "2D noise is the z = 0 slice of 3D noise");
    }
}

static void TestDeterminism(void)
{
    for (uint64_t seed = 0u; seed < 4u; ++seed)
    {
        for (int64_t x = -3000; x <= 3000; x += 700)
        {
            const int64_t first2 = SosNoise2(seed, x, -x, 16u);
            const int64_t second2 = SosNoise2(seed, x, -x, 16u);
            const int64_t first3 = SosNoise3(seed, x, 2 * x, -3 * x, 16u);
            const int64_t second3 = SosNoise3(seed, x, 2 * x, -3 * x, 16u);
            Expect(first2 == second2 && first3 == second3, "the same inputs give the same noise");
        }
    }
    bool differs = false;
    for (int64_t x = 0; x < 64 && !differs; ++x)
    {
        differs = SosNoise2(1u, x, x, 8u) != SosNoise2(2u, x, x, 8u);
    }
    Expect(differs, "different seeds give different noise");
}

static void TestEdgeShifts(void)
{
    Expect(SosNoise2(SEED, 5, 7, 64u) == 0, "a shift of 64 is rejected");
    Expect(SosNoise3(SEED, 5, 7, 9, 100u) == 0, "an out-of-range shift is rejected");
    /* The widest and narrowest lattices are valid and stay in range. */
    Expect(Absolute(SosNoise2(SEED, 5, 7, 0u)) <= SOS_Q16_ONE, "a unit lattice is in range");
    Expect(Absolute(SosNoise3(SEED, 5, 7, 9, 63u)) <= SOS_Q16_ONE, "a 2^63 lattice is in range");
}

/* Fixed points of the generator. Every platform and compiler must produce
 * exactly these values, or two machines would see different worlds from the
 * same seed. */
static void TestGoldenValues(void)
{
    static const struct
    {
        int64_t x;
        int64_t y;
        int64_t z;
        unsigned shift;
        int64_t expected2;
        int64_t expected3;
    } points[] = {
        {0, 0, 0, 16u, -43242, -43242},
        {SOS_METRES(1000), SOS_METRES(-250), SOS_METRES(7), 20u, -620, -622},
        {SOS_METRES(-123456), SOS_METRES(98765), SOS_METRES(33), 12u, -11326, -17542},
    };
    for (size_t index = 0u; index < sizeof(points) / sizeof(points[0]); ++index)
    {
        const int64_t value2 =
            SosNoise2(SEED, points[index].x, points[index].y, points[index].shift);
        const int64_t value3 =
            SosNoise3(SEED, points[index].x, points[index].y, points[index].z, points[index].shift);
        if (value2 != points[index].expected2 || value3 != points[index].expected3)
        {
            fprintf(stderr, "golden %zu: 2D %" PRId64 ", 3D %" PRId64 "\n", index, value2, value3);
        }
        Expect(value2 == points[index].expected2 && value3 == points[index].expected3,
               "the noise matches the reference values");
    }
}

int main(void)
{
    TestRange();
    TestContinuity();
    TestSlice();
    TestDeterminism();
    TestEdgeShifts();
    TestGoldenValues();
    return 0;
}
