#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/rumors.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define DAY SOS_TICKS_PER_DAY

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "rumors test failed: %s\n", message);
        exit(1);
    }
}

static const SosPosition PLACE = {.x = SOS_METRES(120), .y = -SOS_METRES(45), .z = SOS_METRES(3)};

static void TestStart(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    Expect(rumors != NULL, "rumors are created");
    const uint64_t first =
        SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 7u, PLACE, 100u, SOS_Q16_ONE, 0u);
    const uint64_t second =
        SosRumorsStart(rumors, SOS_EVENT_DANGER_SEEN, 0u, PLACE, 101u, SOS_Q16_ONE / 2, 30u);
    const uint64_t unknown =
        SosRumorsStart(rumors, SOS_EVENT_DISCOVERY, 0u, PLACE, 0u, SOS_Q16_ONE, 0u);
    Expect(first != 0u && second != 0u && first != second, "starting rumors gives distinct ids");

    SosRumor rumor;
    Expect(SosRumorsGet(rumors, first, &rumor), "a started rumor is found");
    Expect(rumor.id == first && rumor.kind == SOS_EVENT_FOOD_FOUND && rumor.subject == 7u &&
               rumor.place.x == PLACE.x && rumor.place.y == PLACE.y && rumor.place.z == PLACE.z &&
               rumor.originTick == 0u && rumor.truth == SOS_Q16_ONE,
           "a rumor keeps what it was started with");
    Expect(SosRumorsGet(rumors, second, &rumor) && rumor.originTick == 30u &&
               rumor.truth == SOS_Q16_ONE / 2,
           "a rumor keeps its own origin time and truth");

    SosQ16 belief = 0;
    Expect(SosRumorsBelief(rumors, 100u, first, 0u, &belief) && belief == SOS_Q16_ONE,
           "the witness believes it as strongly as its truth");
    Expect(!SosRumorsBelief(rumors, 1u, unknown, 0u, &belief),
           "a rumor with no witness is known by none");
    Expect(!SosRumorsGet(rumors, 0u, &rumor) && !SosRumorsGet(rumors, 999u, &rumor) &&
               !SosRumorsGet(rumors, first, NULL) && !SosRumorsGet(NULL, first, &rumor),
           "getting rejects what is absent");
    Expect(!SosRumorsBelief(rumors, 100u, first, 0u, NULL) &&
               !SosRumorsBelief(NULL, 100u, first, 0u, &belief) &&
               !SosRumorsBelief(rumors, 0u, first, 0u, &belief) &&
               !SosRumorsBelief(rumors, 100u, 0u, 0u, &belief),
           "believing rejects bad arguments");
    Expect(SosRumorsStart(NULL, SOS_EVENT_FOOD_FOUND, 0u, PLACE, 1u, SOS_Q16_ONE, 0u) == 0u &&
               SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 0u, PLACE, 1u, -1, 0u) == 0u &&
               SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 0u, PLACE, 1u, SOS_Q16_ONE + 1, 0u) ==
                   0u,
           "starting rejects bad arguments");
    Expect(SosRumorsStart(rumors, SOS_EVENT_NONE, 0u, PLACE, 1u, SOS_Q16_ONE, 0u) == 0u,
           "a none rumor is rejected");
    SosRumorsDestroy(rumors);
}

static void TestChain(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t sin = SosRumorsStart(rumors, SOS_EVENT_SIN, 42u, PLACE, 10u, SOS_Q16_ONE, 0u);
    Expect(sin != 0u, "a sin is started");
    SosQ16 belief = 0;
    Expect(SosRumorsTell(rumors, 10u, 11u, sin, 0u, SOS_Q16_ONE), "the witness tells a neighbour");
    SosQ16 second = 0;
    Expect(SosRumorsBelief(rumors, 11u, sin, 0u, &second) && second > 0 && second < SOS_Q16_ONE,
           "the neighbour believes a distorted version");
    Expect(SosRumorsTell(rumors, 11u, 12u, sin, 0u, SOS_Q16_ONE) &&
               SosRumorsBelief(rumors, 12u, sin, 0u, &belief) && belief > 0,
           "the neighbour passes it on");

    uint64_t known[8] = {0};
    Expect(SosRumorsKnownBy(rumors, 12u, 0u, known, 8u) == 1u && known[0] == sin,
           "the last listener knows exactly the one rumor");
    Expect(SosRumorsKnownBy(rumors, 10u, 0u, NULL, 0u) == 1u, "a being can be counted alone");
    Expect(SosRumorsKnownBy(rumors, 10u, 0u, known, 0u) == 1u &&
               SosRumorsKnownBy(rumors, 10u, 0u, NULL, 4u) == 0u,
           "counting without room and rejecting a NULL buffer");
    Expect(SosRumorsKnownBy(rumors, 99u, 0u, known, 8u) == 0u &&
               SosRumorsKnownBy(NULL, 10u, 0u, known, 8u) == 0u &&
               SosRumorsKnownBy(rumors, 0u, 0u, known, 8u) == 0u,
           "a stranger knows nothing");

    Expect(!SosRumorsTell(rumors, 10u, 10u, sin, 0u, SOS_Q16_ONE), "nobody tells themselves");
    Expect(!SosRumorsTell(rumors, 10u, 13u, 999u, 0u, SOS_Q16_ONE), "an unknown rumor is not told");
    Expect(!SosRumorsTell(rumors, 99u, 13u, sin, 0u, SOS_Q16_ONE), "an ignorant teller fails");
    Expect(!SosRumorsTell(NULL, 10u, 11u, sin, 0u, SOS_Q16_ONE), "no store tells nothing");
    Expect(!SosRumorsTell(rumors, 10u, 13u, sin, 0u, SOS_Q16_ONE + 1), "trust above one fails");
    Expect(!SosRumorsTell(rumors, 0u, 13u, sin, 0u, SOS_Q16_ONE), "a zero teller fails");
    SosRumorsDestroy(rumors);
}

static void TestReinforcement(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t death = SosRumorsStart(rumors, SOS_EVENT_DEATH, 5u, PLACE, 1u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(rumors, 1u, 2u, death, 0u, 30000), "a first telling");
    SosQ16 once = 0;
    Expect(SosRumorsBelief(rumors, 2u, death, 0u, &once) && once > 0 && once < SOS_Q16_ONE,
           "the first telling leaves room to grow");
    Expect(SosRumorsTell(rumors, 1u, 2u, death, 10u, 30000), "a second telling");
    SosQ16 twice = 0;
    Expect(SosRumorsBelief(rumors, 2u, death, 10u, &twice) && twice > once,
           "hearing it again strengthens the belief");

    /* The memory is refreshed: without the second telling it would have faded
     * farther by the same tick. */
    SosRumors *control = SosRumorsCreate(SEED);
    const uint64_t other = SosRumorsStart(control, SOS_EVENT_DEATH, 5u, PLACE, 1u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(control, 1u, 2u, other, 0u, 30000), "a single telling elsewhere");
    const uint64_t tick = (uint64_t)21 * DAY;
    SosQ16 stale = 0;
    Expect(SosRumorsBelief(control, 2u, other, tick, &stale), "the single telling still fades in");
    Expect(SosRumorsBelief(rumors, 2u, death, tick, &twice) && twice > stale,
           "a refreshed memory fades less than a stale one");
    SosRumorsDestroy(control);
    SosRumorsDestroy(rumors);
}

static void TestFading(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t discovery =
        SosRumorsStart(rumors, SOS_EVENT_DISCOVERY, 9u, PLACE, 1u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(rumors, 1u, 2u, discovery, 0u, SOS_Q16_ONE), "a discovery is told");
    SosQ16 fresh = 0;
    SosQ16 half = 0;
    Expect(SosRumorsBelief(rumors, 2u, discovery, 0u, &fresh) && fresh > 0, "known at once");
    Expect(SosRumorsBelief(rumors, 2u, discovery, (uint64_t)14 * DAY, &half) && half < fresh &&
               half > 0,
           "belief halves after a half-life but is not gone");
    Expect(!SosRumorsBelief(rumors, 2u, discovery, (uint64_t)64 * 14 * DAY, &half),
           "belief is gone after many half-lives");
    uint64_t known[4] = {0};
    Expect(SosRumorsKnownBy(rumors, 2u, 0u, known, 4u) == 1u &&
               SosRumorsKnownBy(rumors, 2u, (uint64_t)64 * 14 * DAY, known, 4u) == 0u,
           "knowledge fades out of the known list");
    SosRumorsDestroy(rumors);
}

static void TestForgetFaded(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t food =
        SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 1u, PLACE, 10u, SOS_Q16_ONE, 0u);
    const uint64_t danger =
        SosRumorsStart(rumors, SOS_EVENT_DANGER_SEEN, 0u, PLACE, 10u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(rumors, 10u, 20u, food, 0u, 40000) &&
               SosRumorsTell(rumors, 10u, 20u, danger, 0u, 40000) &&
               SosRumorsTell(rumors, 10u, 30u, food, 0u, 40000) &&
               SosRumorsTell(rumors, 10u, 30u, danger, 0u, 40000),
           "two listeners learn two rumors");

    uint64_t known[8] = {0};
    Expect(SosRumorsKnownBy(rumors, 20u, 0u, known, 8u) == 2u, "both are known before fading");
    const uint64_t far = (uint64_t)900 * DAY;
    const uint32_t removed = SosRumorsForgetFaded(rumors, far);
    Expect(removed == 6u, "every faded belief is forgotten");
    SosQ16 belief = 0;
    Expect(!SosRumorsBelief(rumors, 20u, food, far, &belief) &&
               !SosRumorsBelief(rumors, 20u, danger, far, &belief) &&
               SosRumorsKnownBy(rumors, 20u, far, known, 8u) == 0u,
           "forgotten beliefs are gone");
    SosRumor rumor;
    Expect(SosRumorsGet(rumors, food, &rumor) && SosRumorsGet(rumors, danger, &rumor),
           "the rumors themselves stay in history");
    Expect(SosRumorsForgetFaded(rumors, far) == 0u, "forgetting again forgets nothing");
    Expect(SosRumorsForgetFaded(NULL, 0u) == 0u, "no store forgets nothing");

    /* A fresh telling after the pruning is heard by a stranger again. */
    const uint64_t again =
        SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 1u, PLACE, 10u, SOS_Q16_ONE, far);
    Expect(again != 0u && SosRumorsTell(rumors, 10u, 20u, again, far, 40000) &&
               SosRumorsBelief(rumors, 20u, again, far, &belief) && belief > 0,
           "a pruned listener can learn again");
    SosRumorsDestroy(rumors);
}

static void TestForgetBeing(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t sin = SosRumorsStart(rumors, SOS_EVENT_SIN, 1u, PLACE, 1u, SOS_Q16_ONE, 0u);
    const uint64_t death = SosRumorsStart(rumors, SOS_EVENT_DEATH, 2u, PLACE, 1u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(rumors, 1u, 5u, sin, 0u, 50000) &&
               SosRumorsTell(rumors, 1u, 5u, death, 0u, 50000) &&
               SosRumorsTell(rumors, 1u, 6u, sin, 0u, 50000),
           "a being and a witness learn rumors");
    uint64_t known[8] = {0};
    Expect(SosRumorsKnownBy(rumors, 5u, 0u, known, 8u) == 2u, "the being knows two");
    Expect(SosRumorsForgetBeing(rumors, 5u) == 2u, "forgetting a being drops every belief");
    SosQ16 belief = 0;
    Expect(!SosRumorsBelief(rumors, 5u, sin, 0u, &belief) &&
               !SosRumorsBelief(rumors, 5u, death, 0u, &belief) &&
               SosRumorsKnownBy(rumors, 5u, 0u, known, 8u) == 0u,
           "the forgotten being knows nothing");
    Expect(SosRumorsBelief(rumors, 6u, sin, 0u, &belief), "another witness is untouched");
    Expect(SosRumorsForgetBeing(rumors, 5u) == 0u && SosRumorsForgetBeing(rumors, 0u) == 0u &&
               SosRumorsForgetBeing(NULL, 5u) == 0u,
           "forgetting an unknown being is safe");
    SosRumorsDestroy(rumors);
}

/* Forgetting one being drops exactly its own beliefs and leaves every other
 * being's records intact. */
static void TestForgetIsolation(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const uint64_t food =
        SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 1u, PLACE, 1u, SOS_Q16_ONE, 0u);
    const uint64_t danger =
        SosRumorsStart(rumors, SOS_EVENT_DANGER_SEEN, 2u, PLACE, 2u, SOS_Q16_ONE, 0u);
    Expect(SosRumorsTell(rumors, 1u, 5u, food, 0u, 50000) &&
               SosRumorsTell(rumors, 2u, 5u, danger, 0u, 50000) &&
               SosRumorsTell(rumors, 1u, 6u, food, 0u, 40000) &&
               SosRumorsTell(rumors, 2u, 7u, danger, 0u, 30000),
           "three beings learn their rumors");
    SosQ16 foodBefore = 0;
    SosQ16 dangerBefore = 0;
    Expect(SosRumorsBelief(rumors, 6u, food, 0u, &foodBefore) &&
               SosRumorsBelief(rumors, 7u, danger, 0u, &dangerBefore),
           "the surviving witnesses know their rumors");

    Expect(SosRumorsForgetBeing(rumors, 5u) == 2u, "the dead being forgets both beliefs");
    SosQ16 after = 0;
    Expect(SosRumorsBelief(rumors, 6u, food, 0u, &after) && after == foodBefore,
           "another being's belief is untouched");
    Expect(SosRumorsBelief(rumors, 7u, danger, 0u, &after) && after == dangerBefore,
           "another being's belief is untouched by the forgetting");
    uint64_t known[4] = {0};
    Expect(SosRumorsKnownBy(rumors, 6u, 0u, known, 4u) == 1u && known[0] == food &&
               SosRumorsKnownBy(rumors, 7u, 0u, known, 4u) == 1u && known[0] == danger,
           "another being's knowledge list is untouched");
    Expect(!SosRumorsBelief(rumors, 5u, food, 0u, &after) &&
               !SosRumorsBelief(rumors, 5u, danger, 0u, &after) &&
               SosRumorsKnownBy(rumors, 5u, 0u, known, 4u) == 0u,
           "the forgotten being knows nothing");
    SosRumor rumor;
    Expect(SosRumorsGet(rumors, food, &rumor) && SosRumorsGet(rumors, danger, &rumor),
           "the rumors survive the forgetting");
    SosRumorsDestroy(rumors);
}

/* Runs the same history twice and checks that nothing observable differs. */
static void TestDeterminism(void)
{
    SosRumors *first = SosRumorsCreate(SEED);
    SosRumors *second = SosRumorsCreate(SEED);
    for (int run = 0; run < 2; ++run)
    {
        SosRumors *rumors = run == 0 ? first : second;
        const uint64_t food =
            SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 3u, PLACE, 1u, SOS_Q16_ONE, 0u);
        const uint64_t danger = SosRumorsStart(rumors, SOS_EVENT_DANGER_SEEN, 0u,
                                               (SosPosition){1, 2, 3}, 2u, SOS_Q16_ONE / 3, 7u);
        (void)SosRumorsTell(rumors, 1u, 3u, food, 1u, 40000);
        (void)SosRumorsTell(rumors, 3u, 4u, food, 2u, 50000);
        (void)SosRumorsTell(rumors, 2u, 4u, danger, 8u, 20000);
        (void)SosRumorsTell(rumors, 4u, 3u, danger, 9u, 20000);
    }
    const uint64_t tick = 5u * DAY;
    Expect(SosRumorsHash(first, tick) == SosRumorsHash(second, tick),
           "the same history gives the same hash");
    for (uint64_t being = 1u; being <= 4u; ++being)
    {
        uint64_t a[8] = {0};
        uint64_t b[8] = {0};
        const uint32_t countA = SosRumorsKnownBy(first, being, tick, a, 8u);
        const uint32_t countB = SosRumorsKnownBy(second, being, tick, b, 8u);
        Expect(countA == countB, "the same history gives the same knowledge count");
        for (uint32_t index = 0u; index < countA; ++index)
        {
            Expect(a[index] == b[index], "the same history gives the same knowledge");
        }
    }
    Expect(SosRumorsHash(NULL, 0u) == 0u, "no store hashes to zero");
    SosRumorsDestroy(second);
    SosRumorsDestroy(first);
}

/* Fixed points of the rumor store. Every platform and compiler must spread
 * exactly these beliefs from these tellings. */
static void TestGoldenValues(void)
{
    SosRumors *rumors = SosRumorsCreate(SEED);
    const SosPosition granary = {.x = SOS_METRES(10), .y = SOS_METRES(-20), .z = SOS_METRES(2)};
    const SosPosition camp = {.x = -SOS_METRES(5), .y = SOS_METRES(8), .z = 0};
    const uint64_t food =
        SosRumorsStart(rumors, SOS_EVENT_FOOD_FOUND, 1u, granary, 100u, SOS_Q16_ONE, 0u);
    const uint64_t sin =
        SosRumorsStart(rumors, SOS_EVENT_SIN, 200u, camp, 101u, 3 * SOS_Q16_ONE / 4, 5u);
    (void)SosRumorsTell(rumors, 100u, 200u, food, 1u, 5 * SOS_Q16_ONE / 8);
    (void)SosRumorsTell(rumors, 200u, 300u, food, 2u, 7 * SOS_Q16_ONE / 8);
    (void)SosRumorsTell(rumors, 101u, 200u, sin, 6u, SOS_Q16_ONE / 2);
    (void)SosRumorsTell(rumors, 200u, 101u, food, 7u, SOS_Q16_ONE);
    const uint64_t fingerprint = SosRumorsHash(rumors, (uint64_t)3 * DAY);
    const uint64_t expected = UINT64_C(0x0A181DC07B833BC6);
    if (fingerprint != expected)
    {
        fprintf(stderr, "rumor fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the rumor store matches the reference");
    }
    SosRumorsDestroy(rumors);
}

int main(void)
{
    TestStart();
    TestChain();
    TestReinforcement();
    TestFading();
    TestForgetFaded();
    TestForgetBeing();
    TestForgetIsolation();
    TestDeterminism();
    TestGoldenValues();
    return 0;
}
