#include "sim/calendar.h"
#include "sim/world.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "world test failed: %s\n", message);
        exit(1);
    }
}

static void TestCalendar(void)
{
    SosDate date = SosCalendarDate(0u);
    Expect(date.year == 1 && date.day == 1 && date.hour == 6 && date.minute == 0,
           "the world begins at dawn of day one");
    Expect(!SosCalendarIsNight(0u), "dawn is not night");
    const uint64_t evening = (uint64_t)(16 * SOS_TICKS_PER_HOUR + 30 * SOS_TICKS_PER_MINUTE);
    date = SosCalendarDate(evening);
    Expect(date.day == 1 && date.hour == 22 && date.minute == 30, "16.5 hours later it is 22:30");
    Expect(SosCalendarIsNight(evening), "22:30 is night");
    date = SosCalendarDate((uint64_t)SOS_TICKS_PER_YEAR);
    Expect(date.year == 2 && date.day == 1 && date.hour == 6, "a year later it is dawn again");
}

static SosWorld *NewWorld(void)
{
    const SosWorldDesc desc = {.seed = SEED};
    SosWorld *world = SosWorldCreate(&desc);
    Expect(world != NULL, "a world is created");
    return world;
}

static void TestFounders(void)
{
    SosWorld *world = NewWorld();
    const SosBeings *beings = SosWorldBeings(world);
    Expect(SosWorldTick(world) == 0u && SosBeingsLivingCount(beings) == 2u,
           "the world starts with two founders");
    SosBeingView first = {0};
    SosBeingView second = {0};
    Expect(SosBeingsView(beings, 0u, &first) && SosBeingsView(beings, 1u, &second),
           "both founders can be viewed");
    Expect(first.sex != second.sex, "the founders are a man and a woman");
    Expect(first.birthTick == -20 * SOS_TICKS_PER_YEAR && first.mother == 0u,
           "the founders arrive as adults without parents");
    const SosTerrain *terrain = SosWorldTerrain(world);
    Expect(first.position.z == SosTerrainSurfaceZ(terrain, first.position.x, first.position.y),
           "a founder stands on the ground");
    SosWorldDestroy(world);
}

static void TestDeterminism(void)
{
    SosWorld *first = NewWorld();
    SosWorld *second = NewWorld();
    for (int frame = 0; frame < 600; ++frame)
    {
        /* Real time arrives unevenly; equal advances still give equal worlds. */
        const uint64_t ticks = (uint64_t)(frame % 3);
        SosWorldAdvance(first, ticks);
        SosWorldAdvance(second, ticks);
    }
    Expect(SosWorldTick(first) == 600u, "600 frames advance 600 ticks");
    Expect(SosWorldHash(first) == SosWorldHash(second), "equal advances give equal worlds");

    const SosWorldDesc otherDesc = {.seed = SEED + 1u};
    SosWorld *other = SosWorldCreate(&otherDesc);
    Expect(other != NULL, "a second seed creates a world");
    SosWorldAdvance(other, 600u);
    Expect(SosWorldHash(other) != SosWorldHash(first), "another seed gives another world");
    SosWorldDestroy(other);
    SosWorldDestroy(second);
    SosWorldDestroy(first);
}

/* Fast-forwarding generations: a game week per advance, split into steps of
 * about forty minutes, as a fast time scale does. The founders stay together,
 * children are born, and they learn what their parents know. */
static void TestGenerations(void)
{
    SosWorld *world = NewWorld();
    for (int week = 0; week < 20 * 52; ++week)
    {
        SosWorldAdvance(world, (uint64_t)(7 * SOS_TICKS_PER_DAY));
    }
    const SosBeings *beings = SosWorldBeings(world);
    const SosBeingsStats stats = SosBeingsGetStats(beings);
    Expect(stats.births > 0u, "the founders have children within twenty years");
    Expect(SosBeingsLivingCount(beings) == 2u + stats.births - stats.deaths,
           "births and deaths account for the living");

    int learned = 0;
    for (uint32_t slot = 0u; slot < SosBeingsSlotCount(beings); ++slot)
    {
        SosBeingView view;
        if (SosBeingsView(beings, slot, &view) && view.mother != 0u)
        {
            Expect(view.father != 0u, "every child has a father");
            learned |= (view.knowledge & SOS_KNOWLEDGE_SPEECH) != 0u;
        }
    }
    Expect(learned, "children learn speech from their parents");

    const uint64_t expected = UINT64_C(0xD720CC0AFF63558B);
    if (SosWorldHash(world) != expected)
    {
        fprintf(stderr,
                "world hash after twenty years: 0x%016" PRIX64 " (births %" PRIu64
                ", deaths %" PRIu64 ", living %u)\n",
                SosWorldHash(world), stats.births, stats.deaths, SosBeingsLivingCount(beings));
        Expect(0, "twenty years of history match the reference on every platform");
    }
    SosWorldDestroy(world);
}

int main(void)
{
    TestCalendar();
    TestFounders();
    TestDeterminism();
    TestGenerations();
    return 0;
}
