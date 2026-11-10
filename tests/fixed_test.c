#include "sim/fixed.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "fixed test failed: %s\n", message);
        exit(1);
    }
}

static void TestArithmetic(void)
{
    const SosQ16 half = SOS_Q16_ONE / 2;
    Expect(SosQ16Mul(half, half) == SOS_Q16_ONE / 4, "a half of a half");
    Expect(SosQ16Mul(-half, half) == -SOS_Q16_ONE / 4, "signs multiply");
    Expect(SosQ16Lerp(0, SOS_Q16_ONE, half) == half, "lerp halfway");
    Expect(SosQ16Lerp(SOS_Q16_ONE, -SOS_Q16_ONE, SOS_Q16_ONE) == -SOS_Q16_ONE, "lerp to the end");
    Expect(SosQ16Clamp(5, 0, 3) == 3 && SosQ16Clamp(-5, 0, 3) == 0, "clamp");
    Expect(SosQ16AddClamped(SOS_Q16_ONE, SOS_Q16_ONE, 0, SOS_Q16_ONE) == SOS_Q16_ONE,
           "add clamps up");
    Expect(SosQ16AddClamped(INT32_MIN, -1, INT32_MIN, 0) == INT32_MIN, "add never overflows");
    Expect(SosQ16AddClamped(3, -1, 0, 5) == 2, "add in range");
}

static void TestRatio(void)
{
    Expect(SosQ16Ratio(1, 4) == SOS_Q16_ONE / 4, "a quarter");
    Expect(SosQ16Ratio(-3, 2) == -3 * SOS_Q16_ONE / 2, "negative ratios");
    Expect(SosQ16Ratio(7, 0) == 0, "zero denominator");
    Expect(SosQ16Ratio(INT64_MAX, 1) == INT32_MAX, "too large clamps");
    Expect(SosQ16Ratio(INT64_MIN, -1) == INT32_MAX, "the one overflowing division");
    Expect(SosQ16Ratio(INT64_MIN, 1) == INT32_MIN, "too small clamps");
    const int64_t huge = INT64_C(1) << 60;
    Expect(SosQ16Ratio(huge / 2, huge) == SOS_Q16_ONE / 2, "huge denominators");
}

static void TestDecay(void)
{
    const int64_t value = INT64_C(1) << 40;
    Expect(SosDecay(value, 0u, 100u) == value, "no time, no change");
    Expect(SosDecay(value, 100u, 100u) == value / 2, "one half-life halves");
    Expect(SosDecay(value, 300u, 100u) == value / 8, "three half-lives");
    Expect(SosDecay(-value, 100u, 100u) == -value / 2, "negative values decay toward zero");
    Expect(SosDecay(value, 5u, 0u) == 0, "a zero half-life drops at once");
    Expect(SosDecaySince(value, 100u, 200u, 100u) == value / 2, "decay since a tick");
    Expect(SosDecaySince(value, 200u, 100u, 100u) == value, "a stale tick changes nothing");
    Expect(SosDecay(value, UINT64_MAX, 1u) == 0, "everything fades");
    Expect(SosDecay(INT64_MIN, 1u, UINT64_MAX) == INT64_MIN, "the extreme stays in range");

    /* Within 0.1% of 2^-0.5 halfway through. */
    const int64_t halfway = SosDecay(1000000, 50u, 100u);
    Expect(halfway > 706100 && halfway < 708100, "halfway is 1/sqrt(2)");

    int64_t previous = value;
    for (uint64_t elapsed = 1u; elapsed <= 400u; ++elapsed)
    {
        const int64_t now = SosDecay(value, elapsed, 100u);
        Expect(now <= previous && now >= 0, "decay never grows or crosses zero");
        previous = now;
    }

    /* One long step against many short ones. */
    int64_t stepped = value;
    for (int step = 0; step < 10; ++step)
    {
        stepped = SosDecay(stepped, 37u, 100u);
    }
    const int64_t whole = SosDecay(value, 370u, 100u);
    const int64_t difference = stepped > whole ? stepped - whole : whole - stepped;
    Expect(difference < whole / 200, "steps agree with one long step");

    const uint64_t year = UINT64_C(946080000);
    Expect(SosDecay(value, year * 3u, year) == value / 8, "long half-lives");
    Expect(SosDecay(1000, UINT64_C(1) << 62, UINT64_C(1) << 62) == 500, "huge half-lives");
}

int main(void)
{
    TestArithmetic();
    TestRatio();
    TestDecay();
    return 0;
}
