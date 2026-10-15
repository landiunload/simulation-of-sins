#include "sim/calendar.h"
#include "sim/census.h"
#include "sim/terrain.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "census test failed: %s\n", message);
        exit(1);
    }
}

static uint64_t Spawn(SosBeings *beings, const SosTerrain *terrain, uint64_t tick, SosSex sex,
                      int64_t birthTick)
{
    const SosBeingSpawn spawn = {
        .sex = sex,
        .birthTick = birthTick,
        .position = {0, 0, 0},
        .knowledge = 0u,
        .mother = 0u,
        .father = 0u,
    };
    const uint64_t serial = SosBeingsSpawn(beings, terrain, tick, &spawn);
    Expect(serial != 0u, "a being is spawned");
    return serial;
}

/* A birth `years` before the census tick. */
static int64_t Born(uint64_t tick, int64_t years)
{
    return (int64_t)tick - years * SOS_TICKS_PER_YEAR;
}

static void TestEmpty(void)
{
    SosBeings *beings = SosBeingsCreate(SEED);
    Expect(beings != NULL, "beings are created");
    SosCensus census;
    Expect(SosCensusTake(beings, 0u, &census), "an empty population gives a census");
    Expect(census.total == 0u && census.medianAgeYears == 0u && census.dependencyRatio == 0 &&
               census.eldestSerial == 0u && census.health.mean == 0 && census.health.minimum == 0u,
           "an empty census is all zeroes");
    Expect(!SosCensusTake(NULL, 0u, &census) && !SosCensusTake(beings, 0u, NULL),
           "a missing census input fails");
    SosBeingsDestroy(beings);
}

/* Eight beings of known ages, one over a hundred: the pyramid, the median, the
 * dependency ratio and the eldest all have one right answer. */
static void TestPopulation(SosBeings *beings, const SosTerrain *terrain, uint64_t tick,
                           SosCensus *out)
{
    const uint64_t eldest = Spawn(beings, terrain, tick, SOS_SEX_FEMALE, Born(tick, 10));
    Spawn(beings, terrain, tick, SOS_SEX_MALE, Born(tick, 3));
    Spawn(beings, terrain, tick, SOS_SEX_FEMALE, Born(tick, 17));
    Spawn(beings, terrain, tick, SOS_SEX_MALE, Born(tick, 20));
    Spawn(beings, terrain, tick, SOS_SEX_FEMALE, Born(tick, 30));
    Spawn(beings, terrain, tick, SOS_SEX_MALE, Born(tick, 66));
    Spawn(beings, terrain, tick, SOS_SEX_FEMALE, Born(tick, 105));
    Spawn(beings, terrain, tick, SOS_SEX_MALE, Born(tick, 10));
    Expect(eldest == 1u, "serials are handed out in order");

    Expect(SosCensusTake(beings, tick, out), "a census is taken");
    Expect(out->tick == tick, "the census names its tick");
    Expect(out->total == 8u, "eight live");
    Expect(out->bySex[SOS_SEX_FEMALE] == 4u && out->bySex[SOS_SEX_MALE] == 4u, "four of each sex");
    /* Ages: female 10, 17, 30, 105 and male 3, 20, 66, 10. */
    Expect(out->age[SOS_SEX_FEMALE][2] == 1u && out->age[SOS_SEX_FEMALE][3] == 1u &&
               out->age[SOS_SEX_FEMALE][6] == 1u && out->age[SOS_SEX_FEMALE][20] == 1u,
           "women fall in their five-year buckets");
    Expect(out->age[SOS_SEX_MALE][0] == 1u && out->age[SOS_SEX_MALE][2] == 1u &&
               out->age[SOS_SEX_MALE][4] == 1u && out->age[SOS_SEX_MALE][13] == 1u,
           "men fall in their five-year buckets");
    for (uint32_t sex = 0u; sex < SOS_CENSUS_SEX_COUNT; ++sex)
    {
        uint32_t sum = 0u;
        for (uint32_t bucket = 0u; bucket < SOS_CENSUS_AGE_BUCKETS; ++bucket)
        {
            sum += out->age[sex][bucket];
        }
        Expect(sum == out->bySex[sex], "the pyramid holds every being of a sex");
    }
    Expect(out->byActivity[SOS_ACTIVITY_IDLE] == 8u, "everyone idles before a step");
    Expect(out->pregnant == 0u, "nobody is pregnant");
    Expect(out->health.mean == SOS_Q16_ONE && out->health.minimum == SOS_NEED_MAX,
           "everyone is in full health");
    Expect(out->hunger.mean == 0 && out->thirst.mean == 0 && out->fatigue.mean == 0,
           "nobody is hungry, thirsty or tired");
    Expect(out->hunger.minimum == 0u && out->thirst.minimum == 0u && out->fatigue.minimum == 0u,
           "the needs start satisfied");
    Expect(out->medianAgeYears == 17u, "the median is the lower middle of 3..105");
    Expect(out->dependencyRatio == SosQ16Ratio(5, 3), "five depend on three");
    Expect(out->eldestSerial == 7u, "the eldest serial is the oldest being");
}

static void TestGrowth(void)
{
    const SosCensus before = {.total = 100u};
    const SosCensus after = {.total = 110u};
    const SosQ16 tenPercent = SosQ16Ratio(10, 100);
    Expect(SosCensusGrowthPerYear(&before, &after, (uint64_t)SOS_TICKS_PER_YEAR) == tenPercent,
           "ten more in a year is ten percent a year");
    Expect(SosCensusGrowthPerYear(&before, &after, (uint64_t)(SOS_TICKS_PER_YEAR / 2)) ==
               (SosQ16)(tenPercent * 2),
           "the same change in half a year doubles the rate");
    const SosCensus smaller = {.total = 80u};
    Expect(SosCensusGrowthPerYear(&before, &smaller, (uint64_t)SOS_TICKS_PER_YEAR) ==
               -SosQ16Ratio(20, 100),
           "a shrinking population has a negative rate");
    Expect(SosCensusGrowthPerYear(&before, &after, 0u) == 0 &&
               SosCensusGrowthPerYear(NULL, &after, 1u) == 0 &&
               SosCensusGrowthPerYear(&before, NULL, 1u) == 0 &&
               SosCensusGrowthPerYear(&smaller, &before, 0u) == 0,
           "a missing or timeless comparison gives no growth");
}

static void TestDeterminism(const SosBeings *beings, uint64_t tick)
{
    SosCensus first;
    SosCensus second;
    Expect(SosCensusTake(beings, tick, &first) && SosCensusTake(beings, tick, &second),
           "two censuses are taken");
    char a[2048];
    char b[2048];
    Expect(SosCensusFormat(&first, a, sizeof(a)) && SosCensusFormat(&second, b, sizeof(b)),
           "both censuses format");
    Expect(strcmp(a, b) == 0, "the same world gives the same census");
}

static void TestFormat(const SosCensus *c)
{
    char buffer[2048];
    Expect(SosCensusFormat(c, buffer, sizeof(buffer)), "a census formats whole");
    Expect(strlen(buffer) > 0u && buffer[strlen(buffer) - 1u] == '\n',
           "the report ends in a newline");

    char small[16];
    memset(small, 'x', sizeof(small));
    Expect(!SosCensusFormat(c, small, sizeof(small)), "a small buffer reports truncation");
    Expect(strlen(small) < sizeof(small), "a truncated report is still NUL-terminated");
    Expect(strncmp(small, "census at tick ", 15u) == 0, "the report starts with the tick line");

    char one[1] = {'x'};
    Expect(!SosCensusFormat(c, one, 1u) && one[0] == '\0', "a one-byte buffer holds only the NUL");

    char guarded[64u + 8u];
    memset(guarded, 0x7f, sizeof(guarded));
    (void)SosCensusFormat(c, guarded, 64u);
    for (size_t index = 64u; index < sizeof(guarded); ++index)
    {
        Expect(guarded[index] == 0x7f, "formatting stays inside the buffer");
    }

    Expect(!SosCensusFormat(c, buffer, 0u) && !SosCensusFormat(NULL, buffer, sizeof(buffer)) &&
               !SosCensusFormat(c, NULL, sizeof(buffer)),
           "a missing formatting input fails");
}

/* The exact report of the eight known beings; every platform must produce it. */
static void TestGolden(const SosCensus *c)
{
    char text[2048];
    Expect(SosCensusFormat(c, text, sizeof(text)), "the golden census formats");
    static const char expected[] =
        "census at tick 94608000000\n"
        "population 8: female 4, male 4, pregnant 0\n"
        "age female: 0 0 1 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 1\n"
        "age male: 1 0 1 0 1 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0\n"
        "activity idle 8, wander 0, eat 0, drink 0, sleep 0, forage 0\n"
        "health 100.00% (min 1073741824), hunger 0.00% (min 0), thirst 0.00% (min 0), fatigue "
        "0.00% (min 0)\n"
        "median age 17 years, dependency ratio 1.66, eldest 7\n";
    if (strcmp(text, expected) != 0)
    {
        fprintf(stderr, "census text:\n%s", text);
        Expect(0, "the census report matches the reference");
    }
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    TestEmpty();
    TestGrowth();
    SosBeings *beings = SosBeingsCreate(SEED);
    Expect(beings != NULL, "beings are created");
    const uint64_t tick = (uint64_t)(100 * SOS_TICKS_PER_YEAR);
    SosCensus population;
    TestPopulation(beings, &terrain, tick, &population);
    TestDeterminism(beings, tick);
    TestFormat(&population);
    TestGolden(&population);
    SosBeingsDestroy(beings);
    return 0;
}
