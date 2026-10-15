#include "sim/random.h"
#include "sim/space.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "random test failed: %s\n", message);
        exit(1);
    }
}

static void TestKnownSequence(void)
{
    /* SplitMix64 from state 0: the published reference sequence. Equal values
     * on every platform are what makes worlds replayable. */
    SosRandom random = {.state = 0u};
    Expect(SosRandomNext(&random) == UINT64_C(0xE220A8397B1DCDAF), "first SplitMix64 value");
    Expect(SosRandomNext(&random) == UINT64_C(0x6E789E6AA1B965F4), "second SplitMix64 value");
    Expect(SosRandomNext(&random) == UINT64_C(0x06C45D188009454F), "third SplitMix64 value");
}

static void TestStreams(void)
{
    SosRandom first = SosRandomStream(42u, 1u);
    SosRandom again = SosRandomStream(42u, 1u);
    SosRandom other = SosRandomStream(42u, 2u);
    SosRandom otherSeed = SosRandomStream(43u, 1u);
    const uint64_t value = SosRandomNext(&first);
    Expect(value == SosRandomNext(&again), "a stream replays");
    Expect(value != SosRandomNext(&other), "streams of one seed differ");
    Expect(value != SosRandomNext(&otherSeed), "seeds differ");
    Expect(SosHashCombine(1u, 2u) != SosHashCombine(2u, 1u), "combining is ordered");
}

static void TestBounds(void)
{
    SosRandom random = SosRandomStream(7u, 0u);
    uint32_t counts[6] = {0};
    for (int draw = 0; draw < 60000; ++draw)
    {
        const uint32_t value = SosRandomBelow(&random, 6u);
        Expect(value < 6u, "below stays below the bound");
        ++counts[value];
    }
    for (int face = 0; face < 6; ++face)
    {
        Expect(counts[face] > 9500u && counts[face] < 10500u, "below is roughly uniform");
    }
    Expect(SosRandomBelow(&random, 0u) == 0u, "an empty bound gives zero");

    for (int draw = 0; draw < 10000; ++draw)
    {
        const int64_t value = SosRandomRange(&random, -3, 3);
        Expect(value >= -3 && value <= 3, "range is inclusive");
    }
    Expect(SosRandomRange(&random, 5, 5) == 5 && SosRandomRange(&random, 5, -5) == 5,
           "an empty range gives its minimum");
    (void)SosRandomRange(&random, INT64_MIN, INT64_MAX);

    Expect(!SosRandomChance(&random, 0u, 10u) && SosRandomChance(&random, 10u, 10u) &&
               !SosRandomChance(&random, 1u, 0u),
           "chance handles certainty and impossibility");
}

static void TestSpace(void)
{
    Expect(SosFloorShift(5, 1u) == 2 && SosFloorShift(-5, 1u) == -3 && SosFloorShift(-4, 1u) == -2,
           "floor shift rounds toward negative infinity");
    Expect(SosFloorShift(INT64_MIN, 63u) == -1 && SosFloorShift(INT64_MAX, 62u) == 1,
           "floor shift covers the whole range");
    Expect(SosFloorRemainder(-5, 1u) == 1u && SosFloorRemainder(-4, 2u) == 0u &&
               SosFloorRemainder(7, 2u) == 3u,
           "floor remainder pairs with floor shift");
    Expect(SosIntegerSqrt(0u) == 0u && SosIntegerSqrt(15u) == 3u && SosIntegerSqrt(16u) == 4u &&
               SosIntegerSqrt(UINT64_MAX) == UINT64_C(4294967295),
           "integer square root is exact");
}

int main(void)
{
    TestKnownSequence();
    TestStreams();
    TestBounds();
    TestSpace();
    return 0;
}
