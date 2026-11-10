#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/memories.h"
#include "sim/space.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "memories test failed: %s\n", message);
        exit(1);
    }
}

/* A memory whose place is derived from the subject and kind, so a test can see
 * at a glance which record came back. */
static SosMemory MakeMemory(uint32_t kind, uint64_t subject, uint64_t tick, SosQ16 salience,
                            int16_t valence)
{
    return (SosMemory){
        .kind = kind,
        .subject = subject,
        .place = {(int64_t)subject * SOS_METRES(2), (int64_t)kind * SOS_METRES(3), SOS_METRES(1)},
        .tick = tick,
        .salience = salience,
        .valence = valence,
    };
}

/* A memory is remembered once and given back by recall. */
static void TestAddAndRecall(void)
{
    SosMemories memories;
    SosMemories other;
    SosMemoriesInit(&memories);
    SosMemoriesInit(&other);
    Expect(SosMemoriesHash(&memories) == SosMemoriesHash(&other), "an empty memory hashes equally");

    const SosMemory food = MakeMemory(4u, 12u, 100u, SOS_Q16_ONE / 2, 40);
    Expect(SosMemoriesRemember(&memories, &food, 100u), "a memory is remembered");

    SosMemory recalled;
    Expect(SosMemoriesRecall(&memories, 4u, 12u, 100u, &recalled),
           "it is recalled by kind and subject");
    Expect(recalled.kind == 4u && recalled.subject == 12u && recalled.place.x == food.place.x &&
               recalled.place.y == food.place.y && recalled.place.z == food.place.z &&
               recalled.tick == 100u && recalled.salience == SOS_Q16_ONE / 2 &&
               recalled.valence == 40,
           "it comes back as it went in");

    Expect(!SosMemoriesRecall(&memories, 5u, 12u, 100u, &recalled),
           "the wrong kind does not match");
    Expect(!SosMemoriesRecall(&memories, 4u, 13u, 100u, &recalled),
           "the wrong subject does not match");
    Expect(SosMemoriesRecall(&memories, SOS_EVENT_NONE, 0u, 100u, &recalled),
           "both wildcards find the only memory");
}

/* A repeat strengthens the one memory; it never takes a second slot. */
static void TestReinforcement(void)
{
    SosMemories memories;
    SosMemoriesInit(&memories);
    const SosMemory visit = MakeMemory(1u, 5u, 0u, SOS_Q16_ONE / 4, -10);
    for (int repeat = 0; repeat < 10; ++repeat)
    {
        Expect(SosMemoriesRemember(&memories, &visit, 0u), "a repeat is kept");
    }

    SosMemory recalled;
    Expect(SosMemoriesRecall(&memories, 1u, 5u, 0u, &recalled),
           "the reinforced memory is recalled");
    Expect(recalled.salience == (SosQ16)(10 * (SOS_Q16_ONE / 4)), "repeats add up the salience");

    /* The ten repeats used one record: the rest still fit. */
    for (uint32_t index = 0u; index < SOS_MEMORIES_CAPACITY - 1u; ++index)
    {
        const SosMemory other = MakeMemory(2u, 100u + index, 0u, SOS_Q16_ONE / 4, 0);
        Expect(SosMemoriesRemember(&memories, &other, 0u), "distinct memories fill the rest");
    }
    const SosMemory extra = MakeMemory(2u, 999u, 0u, SOS_Q16_ONE / 4, 0);
    Expect(!SosMemoriesRemember(&memories, &extra, 0u), "one past capacity is refused");

    /* Salience saturates instead of overflowing. */
    for (int repeat = 0; repeat < 100; ++repeat)
    {
        (void)SosMemoriesRemember(&memories, &visit, 0u);
    }
    Expect(SosMemoriesRecall(&memories, 1u, 5u, 0u, &recalled) &&
               recalled.salience == SOS_MEMORIES_MAX_SALIENCE,
           "reinforcement saturates");
}

/* A repeat after a long silence decays the old record first: a memory that had
 * faded to nothing cannot come back at full strength. */
static void TestReinforcementAfterGap(void)
{
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    SosMemories memories;
    SosMemoriesInit(&memories);

    /* A calm memory fades to nothing within days; two hundred days is far past
     * its half-life of two. */
    const SosMemory first = MakeMemory(6u, 20u, 0u, SOS_Q16_ONE, 0);
    Expect(SosMemoriesRemember(&memories, &first, 0u), "the first meeting is remembered");

    const uint64_t late = 200u * day;
    const SosMemory again = MakeMemory(6u, 20u, late, SOS_Q16_ONE, 0);
    Expect(SosMemoriesRemember(&memories, &again, late), "the meeting is renewed");

    SosMemory recalled;
    Expect(SosMemoriesRecall(&memories, 6u, 20u, late, &recalled),
           "the renewed memory is recalled");
    Expect(recalled.salience == SOS_Q16_ONE && recalled.tick == late,
           "a faded memory is renewed at the strength of the new event alone");
    Expect(SosMemoriesRecall(&memories, 6u, 20u, late + 2u * day, &recalled) &&
               recalled.salience == SOS_Q16_ONE / 2,
           "the renewed memory then fades from its own strength");
}

/* Remembering an older event after a newer one must never move the record's
 * tick backwards or let the older event outweigh the newer. */
static void TestOutOfOrderReinforcement(void)
{
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    SosMemories memories;
    SosMemoriesInit(&memories);

    SosMemory later = MakeMemory(7u, 21u, 100u * day, SOS_Q16_ONE, 40);
    later.place = (SosPosition){SOS_METRES(4), SOS_METRES(5), SOS_METRES(6)};
    Expect(SosMemoriesRemember(&memories, &later, 100u * day), "the later meeting is remembered");

    const SosMemory earlier = MakeMemory(7u, 21u, 0u, SOS_Q16_ONE, -40);
    Expect(SosMemoriesRemember(&memories, &earlier, 200u * day),
           "an earlier meeting arrives afterwards");

    SosMemory recalled;
    Expect(SosMemoriesRecall(&memories, 7u, 21u, 100u * day, &recalled),
           "the memory is still recalled");
    Expect(recalled.tick == 100u * day, "the tick never moves backwards");
    Expect(recalled.salience == SOS_Q16_ONE, "the faded older event adds nothing");
    Expect(recalled.valence == 40 && recalled.place.x == SOS_METRES(4),
           "the newer event keeps its place and feeling");
}

/* The record is embedded in every being, so its size and field offsets are
 * pinned to exact values: adding, removing, moving or re-padding a field fails
 * here instead of silently growing every being. */
static void TestFootprint(void)
{
    Expect(sizeof(SosMemory) == 56u, "a memory record is exactly 56 bytes");
    Expect(sizeof(SosMemories) == 456u, "a being's memory is exactly 456 bytes");
    Expect(offsetof(SosMemory, subject) == 0u, "subject is the first field");
    Expect(offsetof(SosMemory, place) == 8u, "place follows subject");
    Expect(offsetof(SosMemory, tick) == 32u, "tick follows place");
    Expect(offsetof(SosMemory, kind) == 40u, "kind follows tick");
    Expect(offsetof(SosMemory, salience) == 44u, "salience follows kind");
    Expect(offsetof(SosMemory, valence) == 48u, "valence is the last field");
}

/* When memory is full the weakest record makes room for a stronger newcomer. */
static void TestReplacement(void)
{
    SosMemories memories;
    SosMemoriesInit(&memories);
    for (uint32_t index = 0u; index < SOS_MEMORIES_CAPACITY; ++index)
    {
        const SosMemory equal = MakeMemory(2u, index + 1u, 0u, SOS_Q16_ONE, 0);
        Expect(SosMemoriesRemember(&memories, &equal, 0u), "the memory fills up");
    }

    SosMemory recalled;
    const SosMemory weaker = MakeMemory(2u, 500u, 0u, SOS_Q16_ONE / 2, 0);
    Expect(!SosMemoriesRemember(&memories, &weaker, 0u), "a weaker memory is refused");
    const SosMemory tied = MakeMemory(2u, 500u, 0u, SOS_Q16_ONE, 0);
    Expect(!SosMemoriesRemember(&memories, &tied, 0u), "a tie keeps the older memory");
    const SosMemory stronger = MakeMemory(2u, 500u, 0u, 2 * SOS_Q16_ONE, 80);
    Expect(SosMemoriesRemember(&memories, &stronger, 0u),
           "a stronger memory displaces the weakest");
    Expect(!SosMemoriesRecall(&memories, 2u, 1u, 0u, &recalled), "the weakest record is gone");
    Expect(SosMemoriesRecall(&memories, 2u, 500u, 0u, &recalled) && recalled.valence == 80,
           "the newcomer is recalled");
}

/* Salience follows the elapsed time, and emotion slows the fading. */
static void TestFading(void)
{
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    SosMemories memories;
    SosMemoriesInit(&memories);
    const SosMemory dull = MakeMemory(3u, 7u, 0u, SOS_Q16_ONE, 0);
    const SosMemory vivid = MakeMemory(3u, 8u, 0u, SOS_Q16_ONE, 32767);
    Expect(SosMemoriesRemember(&memories, &dull, 0u) && SosMemoriesRemember(&memories, &vivid, 0u),
           "two vividnesses are remembered");

    SosMemory recalled;
    Expect(SosMemoriesRecall(&memories, 3u, 7u, 0u, &recalled) && recalled.salience == SOS_Q16_ONE,
           "salience is whole before time passes");
    Expect(SosMemoriesRecall(&memories, 3u, 7u, 2 * day, &recalled) &&
               recalled.salience == SOS_Q16_ONE / 2,
           "a dull memory halves in one half-life");

    SosMemory dullLater;
    SosMemory vividLater;
    Expect(SosMemoriesRecall(&memories, 3u, 7u, 4 * day, &dullLater), "the dull memory recalls");
    Expect(SosMemoriesRecall(&memories, 3u, 8u, 4 * day, &vividLater), "the vivid memory recalls");
    Expect(dullLater.salience < SOS_Q16_ONE / 2 && vividLater.salience > 3 * SOS_Q16_ONE / 4,
           "a memory that mattered fades more slowly");

    Expect(!SosMemoriesRecall(&memories, 3u, 7u, 40 * day, &recalled),
           "a dull memory is beyond recall after many half-lives");
    Expect(SosMemoriesRecall(&memories, 3u, 8u, 40 * day, &recalled),
           "the vivid memory is still there");
}

/* Feelings are the salience-weighted valence toward a subject. */
static void TestFeeling(void)
{
    const uint64_t tick = 1000u;
    SosMemories memories;
    SosMemoriesInit(&memories);
    const SosMemory good = MakeMemory(1u, 42u, tick, SOS_Q16_ONE, 300);
    const SosMemory bad = MakeMemory(2u, 42u, tick, SOS_Q16_ONE, -100);
    Expect(SosMemoriesRemember(&memories, &good, tick) &&
               SosMemoriesRemember(&memories, &bad, tick),
           "two feelings about one person");
    Expect(SosMemoriesFeelingAbout(&memories, 42u, tick) == 100,
           "equally salient feelings average their valence");

    const SosMemory smallGood = MakeMemory(1u, 43u, tick, SOS_Q16_ONE, 100);
    const SosMemory bigBad = MakeMemory(2u, 43u, tick, 3 * SOS_Q16_ONE, -100);
    Expect(SosMemoriesRemember(&memories, &smallGood, tick) &&
               SosMemoriesRemember(&memories, &bigBad, tick),
           "two feelings of different strength");
    Expect(SosMemoriesFeelingAbout(&memories, 43u, tick) == -50, "the stronger memory weighs more");
    Expect(SosMemoriesFeelingAbout(&memories, 99u, tick) == 0, "a stranger feels like nothing");
    Expect(SosMemoriesFeelingAbout(&memories, 0u, tick) == 0, "nobody feels like nothing");
    Expect(SosMemoriesFeelingAbout(NULL, 42u, tick) == 0, "no memory feels like nothing");
}

/* Places come back with the memory, favouring the most salient one. */
static void TestPlace(void)
{
    const uint64_t tick = 500u;
    SosMemories memories;
    SosMemoriesInit(&memories);
    SosPosition place = {0, 0, 0};
    Expect(!SosMemoriesPlaceOf(&memories, 4u, tick, &place), "an empty memory has no place");

    const SosMemory quiet = MakeMemory(4u, 1u, tick, SOS_Q16_ONE, 0);
    const SosMemory loud = MakeMemory(4u, 2u, tick, 3 * SOS_Q16_ONE, 0);
    Expect(SosMemoriesRemember(&memories, &quiet, tick) &&
               SosMemoriesRemember(&memories, &loud, tick),
           "two places are remembered");

    Expect(SosMemoriesPlaceOf(&memories, 4u, tick, &place), "a place is found");
    Expect(place.x == loud.place.x && place.y == loud.place.y && place.z == loud.place.z,
           "the most salient place wins");
    Expect(SosMemoriesPlaceOf(&memories, SOS_EVENT_NONE, tick, &place) && place.x == loud.place.x,
           "a wildcard kind takes the most salient place");
    Expect(!SosMemoriesPlaceOf(&memories, 9u, tick, &place), "an unremembered kind has no place");
}

/* Forget drops what faded to nothing and keeps the rest in order. */
static void TestForget(void)
{
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    SosMemories memories;
    SosMemoriesInit(&memories);
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        const SosMemory dull = MakeMemory(5u, 10u + index, 0u, SOS_Q16_ONE, 0);
        Expect(SosMemoriesRemember(&memories, &dull, 0u), "a dull memory is remembered");
    }
    const SosMemory vivid = MakeMemory(5u, 50u, 0u, SOS_Q16_ONE, 32767);
    Expect(SosMemoriesRemember(&memories, &vivid, 0u), "a vivid memory is remembered");

    Expect(SosMemoriesForget(&memories, 0u) == 0u, "nothing fades before time passes");
    Expect(SosMemoriesForget(&memories, 60 * day) == 5u, "five dull memories fade away");

    SosMemory recalled;
    Expect(!SosMemoriesRecall(&memories, 5u, 10u, 60 * day, &recalled),
           "a forgotten memory is not recalled");
    Expect(SosMemoriesRecall(&memories, 5u, 50u, 60 * day, &recalled), "the vivid memory survives");
    Expect(SosMemoriesForget(&memories, 60 * day) == 0u, "forgetting twice drops nothing more");
}

/* Every argument is checked; an invalid call changes nothing. */
static void TestEdges(void)
{
    SosMemories memories;
    SosMemoriesInit(&memories);
    SosMemoriesInit(NULL);
    const uint64_t empty = SosMemoriesHash(&memories);
    Expect(SosMemoriesHash(NULL) == 0u, "no memory hashes to zero");
    Expect(SosMemoriesForget(NULL, 0u) == 0u, "no memory forgets nothing");
    Expect(SosMemoriesFeelingAbout(NULL, 5u, 0u) == 0, "no memory has no feeling");
    const SosMemory probe = MakeMemory(1u, 1u, 0u, SOS_Q16_ONE, 0);
    Expect(!SosMemoriesRemember(NULL, &probe, 0u), "no memory remembers nothing");
    Expect(!SosMemoriesRemember(&memories, NULL, 0u), "nothing is not a memory");
    Expect(!SosMemoriesRecall(NULL, 1u, 1u, 0u, &(SosMemory){0}), "no memory recalls nothing");
    Expect(!SosMemoriesRecall(&memories, 1u, 1u, 0u, NULL), "recall needs somewhere to write");
    Expect(!SosMemoriesPlaceOf(NULL, 1u, 0u, &(SosPosition){0}), "no memory has no place");
    Expect(!SosMemoriesPlaceOf(&memories, 1u, 0u, NULL), "a place needs somewhere to write");

    const SosMemory noKind = MakeMemory(SOS_EVENT_NONE, 1u, 0u, SOS_Q16_ONE, 0);
    Expect(!SosMemoriesRemember(&memories, &noKind, 0u), "no event kind is refused");
    const SosMemory tooStrong = MakeMemory(1u, 1u, 0u, SOS_MEMORIES_MAX_SALIENCE + 1, 0);
    Expect(!SosMemoriesRemember(&memories, &tooStrong, 0u), "too strong a salience is refused");
    const SosMemory negative = MakeMemory(1u, 1u, 0u, -1, 0);
    Expect(!SosMemoriesRemember(&memories, &negative, 0u), "negative salience is refused");
    const SosMemory future = MakeMemory(1u, 1u, 10u, SOS_Q16_ONE, 0);
    Expect(!SosMemoriesRemember(&memories, &future, 9u), "a memory from the future is refused");
    Expect(SosMemoriesHash(&memories) == empty, "a refused memory changes nothing");

    /* Extreme ticks are safe. */
    const SosMemory oldest = MakeMemory(1u, 1u, 0u, SOS_Q16_ONE, 0);
    Expect(SosMemoriesRemember(&memories, &oldest, UINT64_MAX),
           "a memory can arrive at the end of time");
    Expect(SosMemoriesForget(&memories, UINT64_MAX) == 1u, "it has faded by the end of time");
}

/* The same sequence of events always gives the same memory. */
static void TestDeterminism(void)
{
    SosMemories first;
    SosMemories second;
    SosMemoriesInit(&first);
    SosMemoriesInit(&second);
    for (uint32_t index = 0u; index < 40u; ++index)
    {
        const SosMemory memory =
            MakeMemory(1u + index % 4u, 1u + index % 6u, (uint64_t)index * 100u, SOS_Q16_ONE / 2,
                       (int16_t)index);
        (void)SosMemoriesRemember(&first, &memory, (uint64_t)index * 100u + 50u);
        (void)SosMemoriesRemember(&second, &memory, (uint64_t)index * 100u + 50u);
    }
    Expect(SosMemoriesHash(&first) == SosMemoriesHash(&second), "a replay hashes alike");

    SosMemory a = {0};
    SosMemory b = {0};
    Expect(SosMemoriesRecall(&first, 0u, 0u, 4000u, &a) &&
               SosMemoriesRecall(&second, 0u, 0u, 4000u, &b),
           "both replays recall something");
    Expect(a.subject == b.subject && a.kind == b.kind && a.salience == b.salience &&
               a.place.x == b.place.x,
           "both replays recall the same memory");
}

/* Fixed points of the fold. Every platform and compiler must reproduce them. */
static void TestGoldenValues(void)
{
    SosMemories memories;
    SosMemoriesInit(&memories);
    for (uint32_t index = 0u; index < 20u; ++index)
    {
        const SosMemory memory =
            MakeMemory(1u + index % 5u, 100u + index % 3u, (uint64_t)index * SOS_TICKS_PER_HOUR,
                       (SosQ16)(SOS_Q16_ONE / 2 + (index % 4u) * (SOS_Q16_ONE / 8)),
                       (int16_t)(index % 2u == 0u ? 100 * (int)(index % 3u + 1u)
                                                  : -50 * (int)(index % 4u + 1u)));
        (void)SosMemoriesRemember(&memories, &memory, (uint64_t)index * SOS_TICKS_PER_HOUR);
    }
    const uint64_t hash = SosMemoriesHash(&memories);
    const uint64_t expected = UINT64_C(0x4199C2B1B728EE4B);
    if (hash != expected)
    {
        fprintf(stderr, "memories fingerprint: 0x%016" PRIX64 "\n", hash);
        Expect(0, "the memory fold matches the reference");
    }
}

int main(void)
{
    TestAddAndRecall();
    TestReinforcement();
    TestReinforcementAfterGap();
    TestOutOfOrderReinforcement();
    TestFootprint();
    TestReplacement();
    TestFading();
    TestFeeling();
    TestPlace();
    TestForget();
    TestEdges();
    TestDeterminism();
    TestGoldenValues();
    return 0;
}
