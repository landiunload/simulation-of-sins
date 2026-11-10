#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/mood.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "mood test failed: %s\n", message);
        exit(1);
    }
}

/* The intensity of one emotion at a tick, without mutating the mood. */
static SosQ16 EmotionAt(const SosMood *mood, SosEmotion emotion, uint64_t tick)
{
    SosMoodState state;
    Expect(SosMoodAt(mood, tick, &state), "a mood evaluates");
    return state.emotions[emotion];
}

static void TestInit(void)
{
    SosMood mood;
    SosMoodInit(&mood, SOS_Q16_ONE / 2, 100u);
    Expect(mood.baseline == SOS_Q16_ONE / 2 && mood.lastTick == 100u, "init stores its inputs");
    for (int emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        Expect(mood.emotions[emotion] == 0, "a fresh mood has no emotion");
    }
    Expect(SosMoodValence(&mood, 100u) == SOS_Q16_ONE / 2, "a calm mood is its baseline");
    Expect(SosMoodDominant(&mood, 1000000000u) == SOS_MOOD_NONE,
           "a calm mood has no dominant emotion");

    SosMoodInit(&mood, (SosQ16)(2 * SOS_Q16_ONE), 0u);
    Expect(mood.baseline == SOS_Q16_ONE, "a baseline above one is clamped");
    SosMoodInit(&mood, (SosQ16)(-2 * SOS_Q16_ONE), 0u);
    Expect(mood.baseline == -SOS_Q16_ONE, "a baseline below minus one is clamped");
    SosMoodInit(NULL, 0, 0u);
    Expect(SosMoodValence(NULL, 0u) == 0 && SosMoodDominant(NULL, 0u) == SOS_MOOD_NONE &&
               SosMoodHash(NULL, 0u) == 0u,
           "NULL is refused");
    SosMoodState state;
    Expect(!SosMoodAt(NULL, 0u, &state) && !SosMoodAt(&mood, 0u, NULL),
           "NULL at arguments are refused");
}

static void TestHalfLives(void)
{
    SosMood mood;
    SosMoodInit(&mood, 0, 0u);
    Expect(SosMoodFeel(&mood, SOS_MOOD_JOY, SOS_Q16_ONE, 0u), "joy is felt");
    Expect(SosMoodFeel(&mood, SOS_MOOD_SADNESS, SOS_Q16_ONE, 0u), "sadness is felt");
    Expect(SosMoodFeel(&mood, SOS_MOOD_FEAR, SOS_Q16_ONE, 0u), "fear is felt");
    Expect(SosMoodFeel(&mood, SOS_MOOD_ANGER, SOS_Q16_ONE, 0u), "anger is felt");
    Expect(SosMoodFeel(&mood, SOS_MOOD_SURPRISE, SOS_Q16_ONE, 0u), "surprise is felt");
    Expect(EmotionAt(&mood, SOS_MOOD_JOY, 8 * SOS_TICKS_PER_HOUR) == SOS_Q16_ONE / 2,
           "joy halves in eight hours");
    Expect(EmotionAt(&mood, SOS_MOOD_SADNESS, 3 * SOS_TICKS_PER_DAY) == SOS_Q16_ONE / 2,
           "sadness halves in three days");
    Expect(EmotionAt(&mood, SOS_MOOD_FEAR, 6 * SOS_TICKS_PER_HOUR) == SOS_Q16_ONE / 2,
           "fear halves in six hours");
    Expect(EmotionAt(&mood, SOS_MOOD_ANGER, 4 * SOS_TICKS_PER_HOUR) == SOS_Q16_ONE / 2,
           "anger halves in four hours");
    Expect(EmotionAt(&mood, SOS_MOOD_SURPRISE, 5 * SOS_TICKS_PER_MINUTE) == SOS_Q16_ONE / 2,
           "surprise halves in five minutes");
    Expect(EmotionAt(&mood, SOS_MOOD_JOY, 512 * SOS_TICKS_PER_HOUR) == 0,
           "an emotion fades to nothing");
}

static void TestFeelSaturates(void)
{
    SosMood mood;
    SosMoodInit(&mood, 0, 0u);
    Expect(SosMoodFeel(&mood, SOS_MOOD_JOY, SOS_Q16_ONE, 0u), "joy can be felt");
    Expect(SosMoodFeel(&mood, SOS_MOOD_JOY, SOS_Q16_ONE / 2, 0u), "joy can be felt again");
    Expect(EmotionAt(&mood, SOS_MOOD_JOY, 0u) == SOS_Q16_ONE, "joy saturates at one");
    Expect(SosMoodFeel(&mood, SOS_MOOD_JOY, -4 * SOS_Q16_ONE, 0u), "joy can be calmed");
    Expect(EmotionAt(&mood, SOS_MOOD_JOY, 0u) == 0, "joy saturates at zero");
    Expect(!SosMoodFeel(&mood, SOS_MOOD_NONE, SOS_Q16_ONE, 0u) &&
               !SosMoodFeel(&mood, SOS_MOOD_EMOTION_COUNT, SOS_Q16_ONE, 0u),
           "the none and count slots are not emotions");
    Expect(!SosMoodFeel(NULL, SOS_MOOD_JOY, SOS_Q16_ONE, 0u), "a NULL mood is refused");

    /* A feeling lands on the decayed intensity, not the stored one: surprise
     * is one at tick zero, one quarter two half-lives later, so the added
     * quarter leaves a half. */
    SosMood later;
    SosMoodInit(&later, 0, 0u);
    Expect(SosMoodFeel(&later, SOS_MOOD_SURPRISE, SOS_Q16_ONE, 0u), "surprise is felt");
    Expect(SosMoodFeel(&later, SOS_MOOD_SURPRISE, SOS_Q16_ONE / 4, 10 * SOS_TICKS_PER_MINUTE),
           "surprise is felt again later");
    Expect(EmotionAt(&later, SOS_MOOD_SURPRISE, 10 * SOS_TICKS_PER_MINUTE) == SOS_Q16_ONE / 2,
           "a later feeling adds to the decayed intensity");
}

static void TestContagion(void)
{
    SosMood source;
    SosMood target;
    SosMoodInit(&source, 0, 0u);
    SosMoodInit(&target, 0, 0u);
    Expect(SosMoodFeel(&source, SOS_MOOD_JOY, SOS_Q16_ONE, 0u) &&
               SosMoodFeel(&source, SOS_MOOD_FEAR, SOS_Q16_ONE / 2, 0u),
           "the source feels something");
    Expect(SosMoodContagion(&source, &target, SOS_Q16_ONE, 0u), "emotions spread when close");
    Expect(EmotionAt(&target, SOS_MOOD_JOY, 0u) == SOS_Q16_ONE &&
               EmotionAt(&target, SOS_MOOD_FEAR, 0u) == SOS_Q16_ONE / 2,
           "at full closeness the moods match");
    Expect(EmotionAt(&source, SOS_MOOD_JOY, 0u) == SOS_Q16_ONE, "the source is unchanged");

    SosMood stranger;
    SosMoodInit(&stranger, 0, 0u);
    Expect(SosMoodContagion(&source, &stranger, SOS_Q16_ONE / 2, 0u),
           "half closeness spreads half");
    Expect(EmotionAt(&stranger, SOS_MOOD_JOY, 0u) == SOS_Q16_ONE / 2 &&
               EmotionAt(&stranger, SOS_MOOD_FEAR, 0u) == SOS_Q16_ONE / 4,
           "half closeness moves halfway");

    /* A calm neighbour calms a stirred one. */
    SosMood stirred;
    SosMood calm;
    SosMoodInit(&stirred, 0, 0u);
    SosMoodInit(&calm, 0, 0u);
    Expect(SosMoodFeel(&stirred, SOS_MOOD_ANGER, SOS_Q16_ONE, 0u), "the target is angry");
    Expect(SosMoodContagion(&calm, &stirred, SOS_Q16_ONE / 2, 0u), "calm spreads");
    Expect(EmotionAt(&stirred, SOS_MOOD_ANGER, 0u) == SOS_Q16_ONE / 2, "anger is halved by calm");

    Expect(!SosMoodContagion(&source, &target, -1, 0u) &&
               !SosMoodContagion(&source, &target, SOS_Q16_ONE + 1, 0u) &&
               !SosMoodContagion(NULL, &target, SOS_Q16_ONE, 0u) &&
               !SosMoodContagion(&source, NULL, SOS_Q16_ONE, 0u),
           "an invalid spread is refused");
}

static void TestReact(void)
{
    SosMood mood;
    SosMoodInit(&mood, 0, 0u);
    Expect(SosMoodReact(&mood, SOS_EVENT_MEAL, SOS_Q16_ONE, 0u), "a meal is felt");
    Expect(EmotionAt(&mood, SOS_MOOD_JOY, 0u) == SOS_Q16_ONE, "a meal brings joy");

    SosMood grieving;
    SosMoodInit(&grieving, 0, 0u);
    Expect(SosMoodReact(&grieving, SOS_EVENT_KIN_DIED, SOS_Q16_ONE, 0u), "a death is felt");
    Expect(EmotionAt(&grieving, SOS_MOOD_SADNESS, 0u) == SOS_Q16_ONE &&
               EmotionAt(&grieving, SOS_MOOD_ANGER, 0u) == SOS_Q16_ONE / 4,
           "grief is sadness with a spark of anger");

    SosMood afraid;
    SosMoodInit(&afraid, 0, 0u);
    Expect(SosMoodReact(&afraid, SOS_EVENT_THREAT, SOS_Q16_ONE / 2, 0u), "a threat is felt");
    Expect(EmotionAt(&afraid, SOS_MOOD_FEAR, 0u) == SOS_Q16_ONE / 2, "half a threat is half fear");

    SosMood witness;
    SosMoodInit(&witness, 0, 0u);
    Expect(SosMoodReact(&witness, SOS_EVENT_SIN_WITNESSED, SOS_Q16_ONE, 0u), "a sin is seen");
    Expect(SosMoodDominant(&witness, 0u) == SOS_MOOD_DISGUST, "disgust dominates a witnessed sin");

    Expect(!SosMoodReact(&mood, SOS_EVENT_NONE, SOS_Q16_ONE, 0u) &&
               !SosMoodReact(&mood, SOS_EVENT_KIND_COUNT, SOS_Q16_ONE, 0u) &&
               !SosMoodReact(&mood, SOS_EVENT_GIFT, -1, 0u) &&
               !SosMoodReact(&mood, SOS_EVENT_GIFT, SOS_Q16_ONE + 1, 0u) &&
               !SosMoodReact(NULL, SOS_EVENT_GIFT, SOS_Q16_ONE, 0u),
           "an invalid reaction is refused");

    /* A core kind the table has no entry for is valid and moves nothing, while
     * a custom kind is outside the core vocabulary and is refused. */
    SosMood calm;
    SosMoodInit(&calm, 0, 0u);
    Expect(SosMoodReact(&calm, SOS_EVENT_BIRTH, SOS_Q16_ONE, 0u) &&
               SosMoodDominant(&calm, 0u) == SOS_MOOD_NONE,
           "a kind with no impulse moves nothing");
    Expect(!SosMoodReact(&calm, (SosEventKind)SOS_EVENT_CUSTOM_FIRST, SOS_Q16_ONE, 0u),
           "a custom kind is not a core event");
}

static void TestSettleConsistency(void)
{
    SosMood mood;
    SosMoodInit(&mood, SOS_Q16_ONE / 8, 0u);
    Expect(SosMoodReact(&mood, SOS_EVENT_CHILD_BORN, SOS_Q16_ONE, 100u) &&
               SosMoodFeel(&mood, SOS_MOOD_FEAR, SOS_Q16_ONE / 2, 200u) &&
               SosMoodReact(&mood, SOS_EVENT_INSULT, SOS_Q16_ONE, 300u),
           "a busy life is set up");
    const uint64_t ticks[] = {300u, 1000u, 100000u, 100000000u, 1000000000000u};
    for (int index = 0; index < 5; ++index)
    {
        SosMoodState evaluated;
        Expect(SosMoodAt(&mood, ticks[index], &evaluated), "the mood evaluates");
        SosMood settled = mood;
        Expect(SosMoodSettle(&settled, ticks[index]), "the mood settles");
        SosMoodState after;
        Expect(SosMoodAt(&settled, ticks[index], &after), "the settled mood evaluates");
        for (int emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
        {
            Expect(after.emotions[emotion] == evaluated.emotions[emotion],
                   "settle followed by at equals at");
        }
        Expect(after.valence == evaluated.valence, "settled valence matches");
    }
    /* Time only moves forward: a stale settle is refused. */
    Expect(!SosMoodSettle(&mood, 0u) && !SosMoodFeel(&mood, SOS_MOOD_JOY, SOS_Q16_ONE, 0u),
           "a tick before the stored one is refused");
}

/* The same deterministic life, used for the determinism and golden checks. */
static SosMood BuildSequence(void)
{
    SosMood mood;
    SosMoodInit(&mood, SOS_Q16_ONE / 4, 0u);
    Expect(SosMoodFeel(&mood, SOS_MOOD_JOY, SOS_Q16_ONE, 100u), "sequence: joy");
    Expect(SosMoodReact(&mood, SOS_EVENT_CHILD_BORN, SOS_Q16_ONE / 2, 200u), "sequence: child");
    Expect(SosMoodFeel(&mood, SOS_MOOD_FEAR, SOS_Q16_ONE, 1000u), "sequence: fear");
    Expect(SosMoodReact(&mood, SOS_EVENT_INSULT, 3 * SOS_Q16_ONE / 4, 5000u), "sequence: insult");
    Expect(SosMoodReact(&mood, SOS_EVENT_KIN_DIED, SOS_Q16_ONE, 1000000u), "sequence: grief");
    Expect(SosMoodFeel(&mood, SOS_MOOD_DISGUST, SOS_Q16_ONE / 2, 10000000u), "sequence: disgust");
    Expect(SosMoodReact(&mood, SOS_EVENT_SIN_WITNESSED, SOS_Q16_ONE, 20000000u), "sequence: sin");
    return mood;
}

/* Fixed points of the mood model. Every platform and compiler must reach
 * exactly this state. */
static void TestGoldenValues(void)
{
    const uint64_t tick = 30000000u;
    const SosMood first = BuildSequence();
    const SosMood second = BuildSequence();
    Expect(SosMoodHash(&first, tick) == SosMoodHash(&second, tick),
           "the same life gives the same mood");
    SosMoodState state;
    Expect(SosMoodAt(&first, tick, &state), "the golden mood evaluates");
    uint64_t fingerprint = (uint64_t)(int64_t)state.valence;
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(int64_t)SosMoodDominant(&first, tick));
    for (int emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(int64_t)state.emotions[emotion]);
    }
    const uint64_t expected = UINT64_C(0x0DBC4776952BCF3C);
    if (fingerprint != expected)
    {
        fprintf(stderr, "mood fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the mood matches the reference");
    }
}

int main(void)
{
    TestInit();
    TestHalfLives();
    TestFeelSaturates();
    TestContagion();
    TestReact();
    TestSettleConsistency();
    TestGoldenValues();
    return 0;
}
