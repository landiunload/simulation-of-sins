#include "sim/chronicle.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define BIG_EVENTS (2u * SOS_CHRONICLE_EVENTS_PER_CHUNK + 8u)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "chronicle test failed: %s\n", message);
        exit(1);
    }
}

static SosEvent MakeEvent(uint64_t tick, uint32_t kind, uint64_t actor, uint64_t subject,
                          uint64_t group, int64_t value)
{
    return (SosEvent){
        .tick = tick,
        .kind = kind,
        .actor = actor,
        .subject = subject,
        .group = group,
        .place = {.x = (int64_t)tick, .y = -(int64_t)tick, .z = 0},
        .value = value,
    };
}

static void TestCreateAndAppend(void)
{
    SosChronicle *chronicle = SosChronicleCreate();
    Expect(chronicle != NULL, "a chronicle is created");
    Expect(SosChronicleCount(chronicle) == 0u, "a new chronicle is empty");
    SosEvent event;
    Expect(!SosChronicleGet(chronicle, 0u, &event), "an empty chronicle has no event zero");
    Expect(!SosChronicleAppend(chronicle, NULL), "a NULL event is rejected");
    Expect(!SosChronicleAppend(NULL, &event), "a NULL chronicle is rejected");

    const SosEvent first = MakeEvent(100u, SOS_EVENT_BIRTH, 11u, 12u, 7u, -5);
    Expect(SosChronicleAppend(chronicle, &first), "an event is appended");
    Expect(SosChronicleCount(chronicle) == 1u, "the count grows");
    Expect(SosChronicleGet(chronicle, 0u, &event) && event.tick == 100u &&
               event.kind == SOS_EVENT_BIRTH && event.actor == 11u && event.subject == 12u &&
               event.group == 7u && event.place.x == 100 && event.place.y == -100 &&
               event.value == -5,
           "an event round-trips exactly");

    const SosEvent older = MakeEvent(99u, SOS_EVENT_DEATH, 11u, 0u, 0u, 0);
    Expect(!SosChronicleAppend(chronicle, &older), "a decreasing tick is rejected");
    const SosEvent equal = MakeEvent(100u, SOS_EVENT_DEATH, 11u, 0u, 0u, 0);
    Expect(SosChronicleAppend(chronicle, &equal), "an equal tick is allowed");
    Expect(SosChronicleCount(chronicle) == 2u, "the rejected event did not count");

    /* SOS_EVENT_NONE is not a happening and must never enter the log. */
    const SosEvent none = MakeEvent(101u, SOS_EVENT_NONE, 11u, 0u, 0u, 0);
    Expect(!SosChronicleAppend(chronicle, &none), "a none event is rejected");
    Expect(SosChronicleCount(chronicle) == 2u, "the rejected none did not count");

    Expect(!SosChronicleGet(chronicle, 2u, &event), "an index past the end is absent");
    Expect(!SosChronicleGet(chronicle, 1u, NULL), "a NULL output is rejected");
    Expect(!SosChronicleGet(NULL, 0u, &event), "a NULL chronicle is rejected");
    SosChronicleDestroy(chronicle);
}

static void TestFirstAt(void)
{
    SosChronicle *chronicle = SosChronicleCreate();
    Expect(SosChronicleFirstAt(chronicle, 0u) == 0u, "an empty chronicle searches to zero");
    const uint64_t ticks[5] = {10u, 20u, 20u, 30u, 40u};
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        const SosEvent event = MakeEvent(ticks[index], SOS_EVENT_DISASTER, 1u, 0u, 0u, 0);
        Expect(SosChronicleAppend(chronicle, &event), "the search log is built");
    }
    Expect(SosChronicleFirstAt(chronicle, 0u) == 0u, "a tick before all events finds the first");
    Expect(SosChronicleFirstAt(chronicle, 10u) == 0u, "an exact first tick finds its event");
    Expect(SosChronicleFirstAt(chronicle, 15u) == 1u, "a gap finds the next event");
    Expect(SosChronicleFirstAt(chronicle, 20u) == 1u, "a repeated tick finds the earliest");
    Expect(SosChronicleFirstAt(chronicle, 21u) == 3u, "a search jumps past the repeats");
    Expect(SosChronicleFirstAt(chronicle, 40u) == 4u, "the last tick finds the last event");
    Expect(SosChronicleFirstAt(chronicle, 41u) == 5u,
           "a tick past the end finds the one-past index");
    Expect(SosChronicleFirstAt(NULL, 10u) == 0u, "a NULL chronicle searches to zero");
    SosChronicleDestroy(chronicle);
}

static void TestBiography(void)
{
    SosChronicle *chronicle = SosChronicleCreate();
    const uint64_t a = 100u;
    const uint64_t b = 200u;
    const uint64_t c = 300u;
    const uint64_t d = 400u;
    const SosEvent events[4] = {
        MakeEvent(1u, SOS_EVENT_BOND, a, b, 0u, 0),
        MakeEvent(2u, SOS_EVENT_BOND, a, c, 0u, 0),
        MakeEvent(3u, SOS_EVENT_CONFLICT, d, a, 0u, 0),
        MakeEvent(4u, SOS_EVENT_SEPARATION, a, b, 0u, 0),
    };
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        Expect(SosChronicleAppend(chronicle, &events[index]), "the biography log is built");
    }
    uint64_t indices[8];
    const uint32_t forA = SosChronicleOfBeing(chronicle, a, indices, 8u);
    Expect(forA == 4u && indices[0] == 3u && indices[1] == 2u && indices[2] == 1u &&
               indices[3] == 0u,
           "a being's biography walks newest first over both roles");
    const uint32_t forB = SosChronicleOfBeing(chronicle, b, indices, 8u);
    Expect(forB == 2u && indices[0] == 3u && indices[1] == 0u, "a subject-only biography is right");
    const uint32_t forC = SosChronicleOfBeing(chronicle, c, indices, 8u);
    Expect(forC == 1u && indices[0] == 1u, "a one-event biography is right");
    const uint32_t forD = SosChronicleOfBeing(chronicle, d, indices, 8u);
    Expect(forD == 1u && indices[0] == 2u, "an actor-only biography is right");

    Expect(SosChronicleOfBeing(chronicle, a, indices, 2u) == 4u && indices[0] == 3u &&
               indices[1] == 2u,
           "a small capacity truncates but still counts");
    Expect(SosChronicleOfBeing(chronicle, a, NULL, 0u) == 4u, "counting needs no storage");
    Expect(SosChronicleOfBeing(chronicle, 999u, indices, 8u) == 0u, "an unknown being has none");
    Expect(SosChronicleOfBeing(chronicle, 0u, indices, 8u) == 0u, "nobody has no biography");
    Expect(SosChronicleOfBeing(chronicle, a, NULL, 4u) == 0u,
           "a NULL output with room is rejected");
    Expect(SosChronicleOfBeing(NULL, a, indices, 8u) == 0u, "a NULL chronicle has no biography");

    /* A being that acts on itself is one event, not two. */
    const SosEvent self = MakeEvent(5u, SOS_EVENT_SIN, a, a, 0u, 0);
    Expect(SosChronicleAppend(chronicle, &self), "a self event is appended");
    Expect(SosChronicleOfBeing(chronicle, a, indices, 8u) == 5u && indices[0] == 4u,
           "acting on oneself counts once");
    SosChronicleDestroy(chronicle);
}

static void TestForget(void)
{
    SosChronicle *chronicle = SosChronicleCreate();
    const uint64_t being = 7u;
    for (uint32_t index = 0u; index < BIG_EVENTS; ++index)
    {
        const SosEvent event = MakeEvent(index, SOS_EVENT_MIGRATION, being, 0u, 0u, (int64_t)index);
        Expect(SosChronicleAppend(chronicle, &event), "the long log is built");
    }
    const uint64_t beforeHash = SosChronicleHash(chronicle);
    Expect(SosChronicleCount(chronicle) == BIG_EVENTS, "every event counted");
    SosEvent event;
    Expect(SosChronicleGet(chronicle, 0u, &event), "the first chunk is present before forgetting");
    SosChronicleForgetBefore(chronicle, SOS_CHRONICLE_EVENTS_PER_CHUNK);
    Expect(SosChronicleCount(chronicle) == BIG_EVENTS, "forgetting keeps the count");
    Expect(!SosChronicleGet(chronicle, 0u, &event), "a forgotten event reads as absent");
    Expect(!SosChronicleGet(chronicle, SOS_CHRONICLE_EVENTS_PER_CHUNK - 1u, &event),
           "the last forgotten event reads as absent");
    Expect(SosChronicleGet(chronicle, SOS_CHRONICLE_EVENTS_PER_CHUNK, &event) &&
               event.tick == SOS_CHRONICLE_EVENTS_PER_CHUNK,
           "the first kept event survives");
    Expect(!SosChronicleGet(chronicle, BIG_EVENTS, &event), "an out-of-range index stays absent");
    Expect(SosChronicleFirstAt(chronicle, 0u) == SOS_CHRONICLE_EVENTS_PER_CHUNK,
           "a search cannot reach a forgotten event");
    Expect(SosChronicleFirstAt(chronicle, BIG_EVENTS - 1u) == BIG_EVENTS - 1u,
           "a search finds the newest kept event");
    SosChronicleForgetBefore(chronicle, 0u);
    Expect(SosChronicleCount(chronicle) == BIG_EVENTS, "a forget before everything is harmless");
    Expect(SosChronicleGet(chronicle, SOS_CHRONICLE_EVENTS_PER_CHUNK, &event),
           "nothing was forgotten by an old tick");

    /* A second whole chunk can be freed while the newest one survives. */
    const uint64_t secondChunk = (uint64_t)SOS_CHRONICLE_EVENTS_PER_CHUNK * 2u;
    SosChronicleForgetBefore(chronicle, secondChunk);
    Expect(!SosChronicleGet(chronicle, SOS_CHRONICLE_EVENTS_PER_CHUNK, &event),
           "the second chunk is forgotten too");
    Expect(SosChronicleGet(chronicle, secondChunk, &event), "the newest chunk survives");
    Expect(SosChronicleFirstAt(chronicle, 0u) == secondChunk, "searches start at the kept chunk");
    uint64_t indices[64];
    const uint64_t kept = BIG_EVENTS - secondChunk;
    Expect(SosChronicleOfBeing(chronicle, being, indices, 64u) == kept &&
               indices[0] == BIG_EVENTS - 1u && indices[kept - 1u] == secondChunk,
           "a forgotten biography starts at the first kept event");

    /* Appending after forgetting keeps working and continues the chain. */
    const SosEvent newer = MakeEvent(9999u, SOS_EVENT_DEATH, being, 0u, 0u, 0);
    Expect(SosChronicleAppend(chronicle, &newer), "the log grows past a forget");
    Expect(SosChronicleGet(chronicle, BIG_EVENTS, &event) && event.tick == 9999u,
           "the new event has the next global index");
    Expect(SosChronicleHash(chronicle) != beforeHash, "forgetting and appending change the hash");
    Expect(SosChronicleOfBeing(chronicle, being, indices, 64u) == kept + 1u &&
               indices[0] == BIG_EVENTS,
           "the new event joins the biography");
    SosChronicleDestroy(chronicle);
}

static void BuildSequence(SosChronicle *chronicle, int count)
{
    SosRandom random = SosRandomStream(SEED, 5u);
    uint64_t tick = 0u;
    for (int step = 0; step < count; ++step)
    {
        tick += (uint64_t)SosRandomBelow(&random, 7u);
        const SosEvent event =
            MakeEvent(tick, 1u + SosRandomBelow(&random, 19u), 1u + SosRandomBelow(&random, 5u),
                      1u + SosRandomBelow(&random, 5u), SosRandomBelow(&random, 3u),
                      SosRandomRange(&random, -100, 100));
        Expect(SosChronicleAppend(chronicle, &event), "the deterministic log is built");
    }
}

static void TestDeterminism(void)
{
    SosChronicle *first = SosChronicleCreate();
    SosChronicle *again = SosChronicleCreate();
    BuildSequence(first, 500);
    BuildSequence(again, 500);
    Expect(SosChronicleHash(first) == SosChronicleHash(again), "equal logs hash equally");
    Expect(SosChronicleOfBeing(first, 3u, NULL, 0u) == SosChronicleOfBeing(again, 3u, NULL, 0u),
           "equal logs give equal biographies");

    /* Two more logs that differ in one event differ in the hash. */
    SosEvent extra = MakeEvent(SosChronicleCount(first) * 10u, SOS_EVENT_SIN, 3u, 4u, 0u, 1);
    Expect(SosChronicleAppend(again, &extra), "the differing log grows");
    Expect(SosChronicleHash(first) != SosChronicleHash(again),
           "a different log hashes differently");
    SosChronicleDestroy(again);
    SosChronicleDestroy(first);
}

/* Fixed points of the log. Every platform and compiler must fold exactly this
 * value. Kinds start at one because SOS_EVENT_NONE is never stored. */
static void TestGolden(void)
{
    SosChronicle *chronicle = SosChronicleCreate();
    for (uint32_t index = 0u; index < 64u; ++index)
    {
        const SosEvent event =
            MakeEvent((uint64_t)index * 3u, 1u + index % 13u, 1u + index % 5u,
                      1u + (index * 7u) % 5u, index % 3u, (int64_t)(index * index) - 100);
        Expect(SosChronicleAppend(chronicle, &event), "the golden log is built");
    }
    const uint64_t hash = SosChronicleHash(chronicle);
    const uint64_t expected = UINT64_C(0xE6651BC7FD674857);
    if (hash != expected)
    {
        fprintf(stderr, "chronicle hash: 0x%016" PRIX64 "\n", hash);
        Expect(0, "the chronicle folds to the reference value");
    }
    SosChronicleDestroy(chronicle);
}

int main(void)
{
    TestCreateAndAppend();
    TestFirstAt();
    TestBiography();
    TestForget();
    TestDeterminism();
    TestGolden();
    SosChronicleDestroy(NULL);
    SosChronicleForgetBefore(NULL, 10u);
    Expect(SosChronicleCount(NULL) == 0u && SosChronicleHash(NULL) == 0u &&
               SosChronicleFirstAt(NULL, 0u) == 0u,
           "a NULL chronicle is safe to query");
    return 0;
}
