#ifndef SOS_SIM_MOTIVES_H
#define SOS_SIM_MOTIVES_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Motives.
 *
 * Hunger, thirst and fatigue live in beings; beyond them every being carries a
 * set of drives: warmth, safety, companionship, intimacy, esteem, curiosity,
 * comfort, play and purpose. A drive fills lazily with time, from 0
 * (satisfied) to SOS_Q16_ONE (unbearable), at a per-drive rate scaled by
 * personality, and an action satisfies it. Nothing runs per tick: a level
 * follows from the stored level and the ticks since it was last touched,
 * exactly like plant regrowth or SosDecay. The part of a drive that has not
 * yet become a whole unit is kept as a fraction, so a level never depends on
 * how often it is advanced: satisfying one drive every tick leaves every other
 * drive growing exactly as if it were never called.
 *
 * Only the drives named by SosMotive are tracked. They are the ones the
 * simulation acts on, so their ids stay bounded; any other id, including
 * SOS_MOTIVE_NONE, is unknown here and yields 0 or false instead of a level
 * that nothing could ever satisfy.
 *
 * A being is one small SosMotives value: one level per drive as of a single
 * tick. Personality is not copied into beings; the caller keeps one shared
 * SosMotiveRates, from a species or a trait table, and passes it wherever a
 * level or a choice needs it, so a million beings stay affordable. Urgency
 * bends a level into a convex curve, so one drive near unbearable outweighs
 * several half-full ones and steers choices.
 *
 * Every public function tolerates NULL, the unknown drive SOS_MOTIVE_NONE and
 * extreme ticks, and fails safely with 0, false or SOS_MOTIVE_NONE. */

/* (motive, satisfaction) pairs an option may offer. */
#define SOS_MOTIVE_OPTION_SLOTS 4u
/* Personality multiplies a drive's growth by at most this much, which keeps
 * the lazy growth arithmetic well inside 64 bits. */
#define SOS_MOTIVE_SCALE_MAX (8 * SOS_Q16_ONE)

typedef enum SosMotive
{
    /* Not a drive; a caller may use it for "none". Every function rejects it. */
    SOS_MOTIVE_NONE = 0,
    SOS_MOTIVE_WARMTH,
    SOS_MOTIVE_SAFETY,
    SOS_MOTIVE_COMPANIONSHIP,
    SOS_MOTIVE_INTIMACY,
    SOS_MOTIVE_ESTEEM,
    SOS_MOTIVE_CURIOSITY,
    SOS_MOTIVE_COMFORT,
    SOS_MOTIVE_PLAY,
    SOS_MOTIVE_PURPOSE,
    SOS_MOTIVE_COUNT,
} SosMotive;

/* How personality scales the drives: one Q16 multiplier per drive, where
 * SOS_Q16_ONE is neutral and 0 stops the drive growing. One value is shared by
 * every being of a kind, so each function clamps every multiplier it reads to
 * [0, SOS_MOTIVE_SCALE_MAX] rather than trusting the caller. A NULL rates
 * pointer means neutral personality. */
typedef struct SosMotiveRates
{
    SosQ16 scale[SOS_MOTIVE_COUNT];
} SosMotiveRates;

/* One being's drives: every level as it stood at `lastTick`, the fractional
 * growth each drive has already earned toward its next unit, plus that tick.
 * The index SOS_MOTIVE_NONE is unused. Levels are read through the shared
 * rates, which is what keeps this value small. A drive's `carry` is a numerator
 * below that drive's period, so growing it in one long step or in many short
 * ones gives exactly the same level. */
typedef struct SosMotives
{
    SosQ16 level[SOS_MOTIVE_COUNT];
    uint32_t carry[SOS_MOTIVE_COUNT];
    /* The tick every level was last advanced to; never moves backwards. */
    uint64_t lastTick;
} SosMotives;

/* One thing an action satisfies: `satisfaction` is the Q16 amount gained,
 * clamped to [0, SOS_Q16_ONE]; a pair with no satisfaction, or one naming an
 * untracked drive, adds nothing. */
typedef struct SosMotivePair
{
    uint32_t motive;
    SosQ16 satisfaction;
} SosMotivePair;

/* One thing a being may do: up to SOS_MOTIVE_OPTION_SLOTS satisfactions and a
 * Q16 cost. Negative costs count as zero. */
typedef struct SosMotiveOption
{
    SosMotivePair pairs[SOS_MOTIVE_OPTION_SLOTS];
    SosQ16 cost;
} SosMotiveOption;

/* Neutral personality: every drive at SOS_Q16_ONE, so drives fill at their
 * natural rate. */
SosMotiveRates SosMotiveRatesNeutral(void);

/* Readies `m` with every drive satisfied at `tick`. A NULL `m` does nothing. */
void SosMotivesInit(SosMotives *m, uint64_t tick);

/* The drive's level at `tick`, in [0, SOS_Q16_ONE], grown from the stored
 * level by the ticks since it was last touched. `rates` may be NULL for
 * neutral personality. Zero for a null `m` or an id that is not one of
 * SOS_MOTIVE_WARMTH..SOS_MOTIVE_PURPOSE. A `tick` before `m`'s counter leaves
 * the stored level alone. */
SosQ16 SosMotivesLevel(const SosMotives *m, const SosMotiveRates *rates, uint32_t motive,
                       uint64_t tick);

/* Grows every tracked drive to `tick` and takes `amount` off the level of
 * `motive`, never below 0, then remembers `tick`. The other drives keep their
 * fractional growth, so calling this as often as the caller likes leaves their
 * levels exactly where they would have been. False for a null `m`, an
 * untracked id or a non-positive `amount`, which leaves `m` unchanged. */
bool SosMotivesSatisfy(SosMotives *m, const SosMotiveRates *rates, uint32_t motive, SosQ16 amount,
                       uint64_t tick);

/* The drive's level squared: convex, so an urgent drive dominates. Zero for a
 * null `m` or an untracked id. */
SosQ16 SosMotivesUrgency(const SosMotives *m, const SosMotiveRates *rates, uint32_t motive,
                         uint64_t tick);

/* The tracked drive with the greatest urgency at `tick`; ties go to the lower
 * enum value. SOS_MOTIVE_NONE when `m` is null. */
SosMotive SosMotivesMostUrgent(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick);

/* The index of the option whose urgency-weighted satisfactions minus cost is
 * greatest, or -1 for a null `m`, a null `options` or no options. Satisfactions
 * of untracked drives add nothing. Ties go to the lower index. */
int32_t SosMotivesChoose(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick,
                         const SosMotiveOption *options, uint32_t count);

/* Folds the drives' levels and the fractional growth each has earned at `tick`
 * into one value, order-independently, so equal worlds give equal hashes and
 * two beings that only look equal now, but whose carried fractions differ, do
 * not: those grow apart later. Zero for a null `m`. */
uint64_t SosMotivesHash(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick);

#endif
