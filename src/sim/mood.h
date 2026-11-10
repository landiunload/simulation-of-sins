#ifndef SOS_SIM_MOOD_H
#define SOS_SIM_MOOD_H

#include "sim/events.h"
#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Mood: how one being feels right now.
 *
 * A mood is a baseline valence (the pleasantness a being returns to, later
 * supplied by personality) plus six emotions, each an intensity in [0, 1]. It
 * is plain value data held per being, not a container, so a step, a save or a
 * test copies it by value.
 *
 * Emotions only ever decay, so idle state costs nothing to keep: the stored
 * intensities describe a mood as of `lastTick` and the ones at any later tick
 * follow from SosDecay with a fixed half-life per emotion. SosMoodAt evaluates
 * without mutating; SosMoodSettle folds the elapsed decay into the stored
 * state at a moment a being step already touches it, and the two agree.
 *
 * SosMoodFeel and SosMoodReact add impulses, saturating at 0 and 1, and
 * SosMoodContagion pulls one being's emotions toward another's. SosMoodValence
 * and SosMoodDominant summarise a mood for a step or a UI. Everything is
 * integer and derived only from the arguments, so equal inputs give equal
 * results on every machine. */

typedef enum SosEmotion
{
    /* Slot zero: no emotion dominates. It is never felt. */
    SOS_MOOD_NONE = 0,
    SOS_MOOD_JOY,
    SOS_MOOD_SADNESS,
    SOS_MOOD_FEAR,
    SOS_MOOD_ANGER,
    SOS_MOOD_DISGUST,
    SOS_MOOD_SURPRISE,
    SOS_MOOD_EMOTION_COUNT,
} SosEmotion;

/* The mood of one being, stored compactly. It is valid in any state but a
 * freshly initialised one. */
typedef struct SosMood
{
    /* Pleasantness the being returns to when nothing stirs it, in
     * [-SOS_Q16_ONE, SOS_Q16_ONE]. */
    SosQ16 baseline;
    /* Intensities as of `lastTick`, in [0, SOS_Q16_ONE]; slot SOS_MOOD_NONE
     * is unused and always zero. */
    SosQ16 emotions[SOS_MOOD_EMOTION_COUNT];
    /* The tick the stored intensities were last brought up to date. */
    uint64_t lastTick;
} SosMood;

/* An evaluated mood at one tick, for presentation and tests. */
typedef struct SosMoodState
{
    /* The input valence in [-SOS_Q16_ONE, SOS_Q16_ONE]. */
    SosQ16 baseline;
    /* The baseline shaped by the emotions, in [-SOS_Q16_ONE, SOS_Q16_ONE]. */
    SosQ16 valence;
    /* Intensities in [0, SOS_Q16_ONE]; slot SOS_MOOD_NONE is always zero. */
    SosQ16 emotions[SOS_MOOD_EMOTION_COUNT];
} SosMoodState;

/* Starts a calm mood at `tick`. `baseline` is clamped to
 * [-SOS_Q16_ONE, SOS_Q16_ONE]. Does nothing when `mood` is NULL. */
void SosMoodInit(SosMood *mood, SosQ16 baseline, uint64_t tick);

/* Adds a signed impulse to one emotion at `tick`, after decaying the mood to
 * it, saturating the result at 0 and SOS_Q16_ONE. False for a NULL mood, an
 * emotion that is not one of the six, or a `tick` before the stored one, which
 * leaves the mood unchanged. */
bool SosMoodFeel(SosMood *mood, SosEmotion emotion, SosQ16 intensity, uint64_t tick);

/* Writes the mood at `tick` to `outState` without mutating `mood`; a tick
 * before the stored one leaves the mood as of its last update. False when
 * either pointer is NULL. */
bool SosMoodAt(const SosMood *mood, uint64_t tick, SosMoodState *outState);

/* Folds the decay from the stored tick up to `tick` into the mood, so later
 * queries need not recompute it; Settle then At at the same tick equals At.
 * False for a NULL mood or a `tick` before the stored one, which leaves the
 * mood unchanged. */
bool SosMoodSettle(SosMood *mood, uint64_t tick);

/* Pleasantness of the mood at `tick`, in [-SOS_Q16_ONE, SOS_Q16_ONE]; zero
 * when `mood` is NULL. */
SosQ16 SosMoodValence(const SosMood *mood, uint64_t tick);

/* The strongest emotion at `tick`, earliest in enum order on a tie, or
 * SOS_MOOD_NONE when none is felt or `mood` is NULL. */
SosEmotion SosMoodDominant(const SosMood *mood, uint64_t tick);

/* Spreads emotions from `source` toward `target` at `tick`: each of the
 * target's emotions moves the fraction `closeness` of the way to the source's
 * current one. `closeness` is in [0, SOS_Q16_ONE]; one means the two moods
 * match. False for NULL pointers, a `closeness` out of range, or a `tick`
 * before the target's stored one, which leaves the target unchanged. The
 * source is never changed. */
bool SosMoodContagion(const SosMood *source, SosMood *target, SosQ16 closeness, uint64_t tick);

/* Applies the emotion impulses of an event, scaled by `magnitude` in
 * [0, SOS_Q16_ONE], after decaying the mood to `tick`, saturating each emotion
 * at 0 and SOS_Q16_ONE. `event` is a core SosEventKind; a kind the impulse table
 * has no entry for leaves the emotions alone. False for a NULL mood, an event
 * outside (SOS_EVENT_NONE, SOS_EVENT_KIND_COUNT), a `magnitude` out of range, or
 * a `tick` before the stored one, which leaves the mood unchanged. */
bool SosMoodReact(SosMood *mood, SosEventKind event, SosQ16 magnitude, uint64_t tick);

/* Folds the mood evaluated at `tick` into one value, so equal worlds give
 * equal hashes however the stored ticks differ. Zero for a NULL mood. */
uint64_t SosMoodHash(const SosMood *mood, uint64_t tick);

#endif
