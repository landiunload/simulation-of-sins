#ifndef SOS_SIM_RUMORS_H
#define SOS_SIM_RUMORS_H

#include "sim/events.h"
#include "sim/fixed.h"
#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Rumors: what people know about what happened, and how it travels.
 *
 * A rumor is one claim about the world: food was found, danger lies
 * somewhere, someone sinned, someone died, something was discovered. Its
 * kind is a SosEventKind, open-ended from SOS_EVENT_CUSTOM_FIRST upward, so
 * new kinds cost nothing. A being learns a rumor by
 * being told it, and what it believes is the teller's belief times the trust
 * it has in the teller times a per-hop distortion, so one event reaches the
 * far side of a village changed. Belief fades with time like any unkept
 * memory. Only what beings know is stored, so a being that has heard nothing
 * costs nothing and millions of them fit in the world function's sparse
 * layer.
 *
 * Ids are serials that are never reused: 0 is nobody, and a rumor id once
 * handed out stays meaningful for the life of the SosRumors. A rumor's record
 * is permanent history; only beliefs fade. Every function validates its
 * arguments and fails safely, and a function that runs out of memory leaves
 * the store as it was. */

/* A claim. `truth` is how true the original claim was, a Q16 fraction in
 * [0, SOS_Q16_ONE]; believe it with strength `truth` is what a first-hand
 * witness starts from. */
typedef struct SosRumor
{
    uint64_t id;
    /* The SosEventKind of the claim. */
    uint32_t kind;
    /* The being the claim is about, or 0 when it is about a place or a thing. */
    uint64_t subject;
    /* Where it happened. */
    SosPosition place;
    /* When it began to spread. */
    uint64_t originTick;
    SosQ16 truth;
} SosRumor;

typedef struct SosRumors SosRumors;

/* A store for the seed, or NULL when memory ran out. */
SosRumors *SosRumorsCreate(uint64_t seed);
void SosRumorsDestroy(SosRumors *rumors);

/* Starts a rumor. `kind` is a SosEventKind other than SOS_EVENT_NONE, which is
 * not a happening and is rejected. `origin` is the being who first tells it (0
 * for a rumor nobody knows yet, which can then never spread); it is remembered
 * as believing it with strength `truth` as of `tick`. Returns the new id, or 0
 * when a pointer is NULL, `kind` is SOS_EVENT_NONE, `truth` is outside
 * [0, SOS_Q16_ONE], no id is left or memory ran out. */
uint64_t SosRumorsStart(SosRumors *rumors, uint32_t kind, uint64_t subject, SosPosition place,
                        uint64_t origin, SosQ16 truth, uint64_t tick);

/* The rumor `id`, or false when there is none. */
bool SosRumorsGet(const SosRumors *rumors, uint64_t id, SosRumor *outRumor);

/* `teller` tells `listener` the rumor. The listener's belief becomes the
 * teller's belief times `trust` times a per-hop distortion, clamped to
 * [0, SOS_Q16_ONE]. A listener who already knows it is reinforced instead,
 * moving toward certainty, and keeps the shorter of the two hop counts.
 * False when a pointer or an id is 0, the teller is the listener, the rumor
 * is unknown, the teller does not know the rumor at `tick`, `trust` is
 * outside [0, SOS_Q16_ONE], or memory ran out (the store is left unchanged). */
bool SosRumorsTell(SosRumors *rumors, uint64_t teller, uint64_t listener, uint64_t rumor,
                   uint64_t tick, SosQ16 trust);

/* What `being` believes of `rumor` at `tick`, faded since it was last heard,
 * or false when it does not know it (never heard, or faded to nothing). */
bool SosRumorsBelief(const SosRumors *rumors, uint64_t being, uint64_t rumor, uint64_t tick,
                     SosQ16 *outBelief);

/* The rumors `being` still knows at `tick`, oldest first. Writes up to
 * `capacity` ids into `outIds` and returns how many there are, which may
 * exceed `capacity`. Returns 0 for a NULL store, a zero being, or a NULL
 * `outIds` with a nonzero `capacity`. */
uint32_t SosRumorsKnownBy(const SosRumors *rumors, uint64_t being, uint64_t tick, uint64_t *outIds,
                          uint32_t capacity);

/* Drops the beliefs that have faded to nothing at `tick` and returns how many
 * were forgotten. The rumors stay: history is not lost when a witness is. */
uint32_t SosRumorsForgetFaded(SosRumors *rumors, uint64_t tick);

/* Drops everything `being` knows and returns how many beliefs were forgotten,
 * for a being that has died or been forgotten. */
uint32_t SosRumorsForgetBeing(SosRumors *rumors, uint64_t being);

/* Folds the state at `tick` into one value: the rumors, and the beliefs still
 * above nothing once faded to `tick`, in an order-independent sum, so equal
 * worlds give equal hashes on every machine and long-faded memories do not
 * count. 0 for a NULL store. */
uint64_t SosRumorsHash(const SosRumors *rumors, uint64_t tick);

#endif
