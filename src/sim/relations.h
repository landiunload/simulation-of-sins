#ifndef SOS_SIM_RELATIONS_H
#define SOS_SIM_RELATIONS_H

#include "sim/calendar.h"
#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* What each being thinks of the others: a sparse directed graph.
 *
 * The feelings one being has for another are a layer over the world function:
 * most pairs have never met and are neutral, so only pairs that interacted
 * are stored. That keeps the graph sparse however many millions of beings
 * live. Every entry decays lazily toward neutral from the tick it was last
 * touched, so an idle relation costs nothing until it is read, when the time
 * that has passed is applied to it (the pattern plant regrowth uses).
 *
 * A shared SosLinks set keeps one being's relations as a list of `to`s, in the
 * order they arose, so one being's relations are enumerated in O(degree)
 * instead of by scanning the whole graph. */

/* Timescales of an opinion. Affinity and trust are momentary feelings and
 * halve in a year, so a slight cools quickly; familiarity is the memory of
 * time spent together and halves in two, so the company of someone stays
 * recalled after the liking has gone. */
#define SOS_RELATIONS_AFFINITY_HALF_LIFE SOS_TICKS_PER_YEAR
#define SOS_RELATIONS_TRUST_HALF_LIFE SOS_TICKS_PER_YEAR
#define SOS_RELATIONS_FAMILIARITY_HALF_LIFE (2 * SOS_TICKS_PER_YEAR)

/* How much one interaction raises familiarity, in [0, ONE]: sixteen meetings
 * make strangers into acquaintances. */
#define SOS_RELATIONS_FAMILIARITY_STEP (SOS_Q16_ONE / 16)

/* Prune drops a relation once familiarity has faded below this and both
 * feelings are nearer neutral than SOS_RELATIONS_PRUNE_FEELING: what is left
 * is finer than a one-off meeting, indistinguishable from never having met. */
#define SOS_RELATIONS_PRUNE_FAMILIARITY (SOS_Q16_ONE / 16)
#define SOS_RELATIONS_PRUNE_FEELING (SOS_Q16_ONE / 64)

/* How a being feels about another at the queried tick. */
typedef struct SosRelation
{
    /* Liking, [-ONE, ONE]: positive is a friend, negative an enemy. */
    SosQ16 affinity;
    /* How far the other is believed, [-ONE, ONE]. */
    SosQ16 trust;
    /* How well the other is known, [0, ONE]; every contact raises it. */
    SosQ16 familiarity;
} SosRelation;

typedef struct SosRelationEntry
{
    /* The being the relation points to, never 0. */
    uint64_t other;
    SosRelation relation;
} SosRelationEntry;

typedef struct SosRelations SosRelations;

/* An empty graph, or NULL when memory ran out. */
SosRelations *SosRelationsCreate(void);
void SosRelationsDestroy(SosRelations *relations);

/* How `from` feels about `to` at `tick`, decayed from the last contact.
 * Returns false and fills `out` with a neutral feeling for a stranger, and
 * false without touching `out` for a NULL `relations` or `out` or a zero
 * `from` or `to`. */
bool SosRelationsGet(const SosRelations *relations, uint64_t from, uint64_t to, uint64_t tick,
                     SosRelation *out);

/* Records an interaction: decays the feeling to `tick`, adds the deltas,
 * clamps affinity and trust to [-ONE, ONE] and raises familiarity by
 * SOS_RELATIONS_FAMILIARITY_STEP. Returns false for a zero `from` or `to`, a
 * relation of a being with itself, or when memory ran out; on failure the
 * graph is unchanged. */
bool SosRelationsAdjust(SosRelations *relations, uint64_t from, uint64_t to, uint64_t tick,
                        SosQ16 affinityDelta, SosQ16 trustDelta);

/* The affinity they have for one another: the average of `a` for `b` and `b`
 * for `a` at `tick`, and neutral (0) when either is a stranger, for the same
 * being twice, or for invalid arguments. Symmetric. */
SosQ16 SosRelationsMutual(const SosRelations *relations, uint64_t a, uint64_t b, uint64_t tick);

/* The outgoing relations of `from` at `tick`, oldest first. Writes up to
 * `capacity` of them and returns how many there are, which may exceed
 * `capacity`. Zero for a NULL `relations`, a NULL `out` with a non-zero
 * `capacity`, or a zero `from`. */
uint32_t SosRelationsOf(const SosRelations *relations, uint64_t from, uint64_t tick,
                        SosRelationEntry *out, uint32_t capacity);

/* The beings `from` likes most (`positive`) or least (negative), strongest
 * first, ties to the lower serial. Writes up to `capacity` serials and
 * returns how many relations there are, which may exceed `capacity`. Zero for
 * a NULL `relations`, a NULL `out` with a non-zero `capacity`, or a zero
 * `from`. Only the affinity decides the order. */
uint32_t SosRelationsStrongest(const SosRelations *relations, uint64_t from, uint64_t tick,
                               bool positive, uint64_t *out, uint32_t capacity);

/* Forgets every relation `being` holds for others, as when it dies, and
 * returns how many. Relations others hold for it stay as their memory of it.
 * Zero for a NULL `relations` or a zero `being`. */
uint32_t SosRelationsForget(SosRelations *relations, uint64_t being);

/* Drops the relations that have faded to indistinguishable from never having
 * met at `tick`, and returns how many, keeping the lists consistent. Zero for
 * a NULL `relations`. */
uint32_t SosRelationsPrune(SosRelations *relations, uint64_t tick);

/* Folds the effective state at `tick` into one value: relations that have
 * decayed to the same feelings count as equal however long ago they were
 * touched, so a replay and another machine can be compared. Zero for a NULL
 * `relations`. */
uint64_t SosRelationsHash(const SosRelations *relations, uint64_t tick);

#endif
