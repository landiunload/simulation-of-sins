#include "sim/mood.h"

#include "sim/calendar.h"
#include "sim/random.h"

/* Half-life of each emotion in ticks, indexed by SosEmotion. Joy is a good
 * mood that lingers into the day, sadness is grief that takes days to lift,
 * fear and anger are acute but shorter, disgust sours a whole day, and
 * surprise is a startle that is gone within the hour. */
static const uint64_t emotionHalfLife[SOS_MOOD_EMOTION_COUNT] = {
    [SOS_MOOD_NONE] = 0u,
    [SOS_MOOD_JOY] = 8 * SOS_TICKS_PER_HOUR,
    [SOS_MOOD_SADNESS] = 3 * SOS_TICKS_PER_DAY,
    [SOS_MOOD_FEAR] = 6 * SOS_TICKS_PER_HOUR,
    [SOS_MOOD_ANGER] = 4 * SOS_TICKS_PER_HOUR,
    [SOS_MOOD_DISGUST] = 12 * SOS_TICKS_PER_HOUR,
    [SOS_MOOD_SURPRISE] = 5 * SOS_TICKS_PER_MINUTE,
};

/* How much each emotion moves valence: joy lifts it, the aversive emotions
 * lower it, and surprise is arousal rather than pleasantness, so it leaves it
 * alone. */
static const SosQ16 emotionWeight[SOS_MOOD_EMOTION_COUNT] = {
    [SOS_MOOD_NONE] = 0,
    [SOS_MOOD_JOY] = SOS_Q16_ONE,
    [SOS_MOOD_SADNESS] = -SOS_Q16_ONE,
    [SOS_MOOD_FEAR] = -SOS_Q16_ONE,
    [SOS_MOOD_ANGER] = -SOS_Q16_ONE,
    [SOS_MOOD_DISGUST] = -SOS_Q16_ONE,
    [SOS_MOOD_SURPRISE] = 0,
};

/* The impulse each event gives every emotion, before `magnitude` scales it.
 * What makes a being feel something is a rule of the world, so it is a table
 * indexed by SosEventKind rather than scattered code, and later culture can
 * replace entries. A kind with no entry - most of the vocabulary - moves
 * nothing. */
static const SosQ16 eventImpulse[SOS_EVENT_KIND_COUNT][SOS_MOOD_EMOTION_COUNT] = {
    [SOS_EVENT_MEAL] = {[SOS_MOOD_JOY] = SOS_Q16_ONE, [SOS_MOOD_SADNESS] = -SOS_Q16_ONE / 4},
    [SOS_EVENT_HUNGER] = {[SOS_MOOD_JOY] = -SOS_Q16_ONE / 2,
                          [SOS_MOOD_SADNESS] = SOS_Q16_ONE / 2,
                          [SOS_MOOD_ANGER] = SOS_Q16_ONE / 2},
    [SOS_EVENT_CHILD_BORN] = {[SOS_MOOD_JOY] = SOS_Q16_ONE, [SOS_MOOD_SURPRISE] = SOS_Q16_ONE / 2},
    [SOS_EVENT_KIN_DIED] = {[SOS_MOOD_SADNESS] = SOS_Q16_ONE, [SOS_MOOD_ANGER] = SOS_Q16_ONE / 4},
    [SOS_EVENT_INSULT] = {[SOS_MOOD_SADNESS] = SOS_Q16_ONE / 4, [SOS_MOOD_ANGER] = SOS_Q16_ONE},
    [SOS_EVENT_GIFT] = {[SOS_MOOD_JOY] = SOS_Q16_ONE, [SOS_MOOD_SURPRISE] = SOS_Q16_ONE / 2},
    [SOS_EVENT_THREAT] = {[SOS_MOOD_FEAR] = SOS_Q16_ONE, [SOS_MOOD_ANGER] = SOS_Q16_ONE / 4},
    [SOS_EVENT_DISCOVERY] = {[SOS_MOOD_JOY] = SOS_Q16_ONE / 2, [SOS_MOOD_SURPRISE] = SOS_Q16_ONE},
    [SOS_EVENT_SIN_WITNESSED] = {[SOS_MOOD_ANGER] = SOS_Q16_ONE / 2,
                                 [SOS_MOOD_DISGUST] = SOS_Q16_ONE,
                                 [SOS_MOOD_SURPRISE] = SOS_Q16_ONE / 4},
};

/* Ticks from `from` to `tick`, never negative. */
static uint64_t Elapsed(uint64_t tick, uint64_t from)
{
    return tick > from ? tick - from : 0u;
}

static SosQ16 ClampValence(int64_t value)
{
    if (value > SOS_Q16_ONE)
    {
        return SOS_Q16_ONE;
    }
    if (value < -SOS_Q16_ONE)
    {
        return -SOS_Q16_ONE;
    }
    return (SosQ16)value;
}

/* value + delta, held in [0, SOS_Q16_ONE]. */
static SosQ16 SaturateAdd(SosQ16 value, SosQ16 delta)
{
    const int64_t sum = (int64_t)value + delta;
    if (sum > SOS_Q16_ONE)
    {
        return SOS_Q16_ONE;
    }
    if (sum < 0)
    {
        return 0;
    }
    return (SosQ16)sum;
}

/* The six emotions at `tick`, decayed from the stored intensities. */
static void Evaluate(const SosMood *mood, uint64_t tick, SosQ16 out[SOS_MOOD_EMOTION_COUNT])
{
    const uint64_t elapsed = Elapsed(tick, mood->lastTick);
    out[SOS_MOOD_NONE] = 0;
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        out[emotion] = (SosQ16)SosDecay(mood->emotions[emotion], elapsed, emotionHalfLife[emotion]);
    }
}

static SosQ16 ValenceOf(SosQ16 baseline, const SosQ16 emotions[SOS_MOOD_EMOTION_COUNT])
{
    int64_t sum = baseline;
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        sum += SosQ16Mul(emotionWeight[emotion], emotions[emotion]);
    }
    return ClampValence(sum);
}

void SosMoodInit(SosMood *mood, SosQ16 baseline, uint64_t tick)
{
    if (mood == NULL)
    {
        return;
    }
    /* The compound literal zero-fills the emotions and the unused slot. */
    *mood = (SosMood){.baseline = ClampValence(baseline), .lastTick = tick};
}

bool SosMoodAt(const SosMood *mood, uint64_t tick, SosMoodState *outState)
{
    if (mood == NULL || outState == NULL)
    {
        return false;
    }
    outState->baseline = mood->baseline;
    Evaluate(mood, tick, outState->emotions);
    outState->valence = ValenceOf(mood->baseline, outState->emotions);
    return true;
}

bool SosMoodSettle(SosMood *mood, uint64_t tick)
{
    if (mood == NULL || tick < mood->lastTick)
    {
        return false;
    }
    const uint64_t elapsed = tick - mood->lastTick;
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        mood->emotions[emotion] =
            (SosQ16)SosDecay(mood->emotions[emotion], elapsed, emotionHalfLife[emotion]);
    }
    mood->lastTick = tick;
    return true;
}

SosQ16 SosMoodValence(const SosMood *mood, uint64_t tick)
{
    SosMoodState state;
    if (!SosMoodAt(mood, tick, &state))
    {
        return 0;
    }
    return state.valence;
}

SosEmotion SosMoodDominant(const SosMood *mood, uint64_t tick)
{
    SosMoodState state;
    if (!SosMoodAt(mood, tick, &state))
    {
        return SOS_MOOD_NONE;
    }
    /* A strict comparison keeps the earliest emotion in enum order on a tie,
     * and leaves SOS_MOOD_NONE when nothing is felt. */
    SosEmotion dominant = SOS_MOOD_NONE;
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        if (state.emotions[emotion] > state.emotions[dominant])
        {
            dominant = (SosEmotion)emotion;
        }
    }
    return dominant;
}

bool SosMoodFeel(SosMood *mood, SosEmotion emotion, SosQ16 intensity, uint64_t tick)
{
    if (mood == NULL || emotion <= SOS_MOOD_NONE || emotion >= SOS_MOOD_EMOTION_COUNT ||
        !SosMoodSettle(mood, tick))
    {
        return false;
    }
    mood->emotions[emotion] = SaturateAdd(mood->emotions[emotion], intensity);
    return true;
}

bool SosMoodContagion(const SosMood *source, SosMood *target, SosQ16 closeness, uint64_t tick)
{
    if (source == NULL || target == NULL || closeness < 0 || closeness > SOS_Q16_ONE)
    {
        return false;
    }
    SosMoodState sourceState;
    if (!SosMoodAt(source, tick, &sourceState) || !SosMoodSettle(target, tick))
    {
        return false;
    }
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        const SosQ16 gap = (SosQ16)(sourceState.emotions[emotion] - target->emotions[emotion]);
        target->emotions[emotion] =
            SaturateAdd(target->emotions[emotion], SosQ16Mul(closeness, gap));
    }
    return true;
}

bool SosMoodReact(SosMood *mood, SosEventKind event, SosQ16 magnitude, uint64_t tick)
{
    if (mood == NULL || event <= SOS_EVENT_NONE || event >= SOS_EVENT_KIND_COUNT || magnitude < 0 ||
        magnitude > SOS_Q16_ONE || !SosMoodSettle(mood, tick))
    {
        return false;
    }
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        mood->emotions[emotion] = SaturateAdd(mood->emotions[emotion],
                                              SosQ16Mul(eventImpulse[event][emotion], magnitude));
    }
    return true;
}

uint64_t SosMoodHash(const SosMood *mood, uint64_t tick)
{
    if (mood == NULL)
    {
        return 0u;
    }
    SosMoodState state;
    (void)SosMoodAt(mood, tick, &state);
    uint64_t hash = SosHashCombine(SosHash64((uint64_t)(int64_t)state.baseline),
                                   (uint64_t)(int64_t)state.valence);
    for (uint32_t emotion = SOS_MOOD_JOY; emotion < SOS_MOOD_EMOTION_COUNT; ++emotion)
    {
        hash = SosHashCombine(hash, (uint64_t)(int64_t)state.emotions[emotion]);
    }
    return hash;
}
