#ifndef SOS_SIM_MEMORIES_H
#define SOS_SIM_MEMORIES_H

#include "sim/events.h"
#include "sim/fixed.h"
#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Episodic memory.
 *
 * Every being carries a small, fixed set of what happened to it: what the
 * event was, who or what it was about, where it took place, when, how strongly
 * it was felt and whether it felt good or bad. Eight records cover a life, so
 * a being needs no allocation and can be stored as plain data; the weakest
 * memory makes room when a stronger one arrives.
 *
 * Nothing is decayed in place. A record keeps the salience it had when it was
 * laid down and the salience now follows from the elapsed time alone, exactly
 * as SosDecay does for every other fading quantity. What makes memory human is
 * that emotion decides how long it lasts: an event that moved a being strongly
 * halves far more slowly than a dull one, so the past is kept in proportion to
 * how much it mattered. A dull memory is still recalled until it fades to
 * nothing, and SosMemoriesForget then drops it. */

/* Records a being holds at once. Eight names the handful of people, places
 * and events that shape a day, and keeps a being small enough to store by
 * value for millions of them. */
#define SOS_MEMORIES_CAPACITY 8u

/* Strongest salience a record can hold. A repeat of an event adds salience, so
 * this ceiling saturates a memory that keeps being refreshed instead of letting
 * it run away; it is eight events at full strength. */
#define SOS_MEMORIES_MAX_SALIENCE ((SosQ16)(8 * SOS_Q16_ONE))

/* One remembered event. `subject` is the being or thing it is about (zero for
 * an event about no one), `place` where it happened, `tick` when it was laid
 * down, `kind` a SosEventKind, `salience` how strong it was then and
 * `valence` whether it felt bad (negative) or good (positive). The record is 56
 * bytes and no smaller without losing a field's exact value: a place is three
 * 64-bit coordinates because the world is unbounded, and subject and tick are
 * 64-bit too. kind, salience and valence fill 10 of the remaining bytes, which
 * the 8-byte alignment of the wide fields rounds up to 56; reordering only
 * moves the six bytes of tail padding, it saves nothing. */
typedef struct SosMemory
{
    uint64_t subject;
    SosPosition place;
    uint64_t tick;
    uint32_t kind;
    SosQ16 salience;
    int16_t valence;
} SosMemory;

/* A being's memory: up to SOS_MEMORIES_CAPACITY records in the order they were
 * first remembered. Plain data, safe to copy and to embed in a being. */
typedef struct SosMemories
{
    uint32_t count;
    SosMemory records[SOS_MEMORIES_CAPACITY];
} SosMemories;

/* Empties the memory. NULL is ignored. */
void SosMemoriesInit(SosMemories *memories);

/* Records `memory`, which happened at `memory->tick`, as felt at `tick`. A
 * repeat of the same kind and subject reinforces the one already held: the
 * older of the two is decayed to the later tick before their salience adds up
 * (capped at SOS_MEMORIES_MAX_SALIENCE), so the silence between two events is
 * not thrown away, and the place and valence become the newer event's. The
 * record's tick never moves backwards, even when events arrive out of order,
 * so a habit grows instead of filling memory with copies. When all
 * SOS_MEMORIES_CAPACITY records are held the least salient at `tick` is
 * replaced if `memory` is more salient at `tick`; ties keep the older memory.
 * Returns whether the memory was kept. False, changing nothing, for a NULL
 * argument, a `memory->kind` of SOS_EVENT_NONE, a salience outside
 * [0, SOS_MEMORIES_MAX_SALIENCE] or a `memory->tick` later than `tick`. */
bool SosMemoriesRemember(SosMemories *memories, const SosMemory *memory, uint64_t tick);

/* The most salient memory matching at `tick`, written to `outMemory` with its
 * salience decayed to `tick`. A `kind` of SOS_EVENT_NONE or a zero `subject` is
 * a wildcard, so both zero asks for the strongest memory of all. Of equally
 * salient memories the first in the record array wins, and one that faded to
 * nothing is not recalled. False for a NULL argument or no match, leaving
 * `outMemory` untouched. */
bool SosMemoriesRecall(const SosMemories *memories, uint32_t kind, uint64_t subject, uint64_t tick,
                       SosMemory *outMemory);

/* How `subject` feels to the being at `tick`: the salience-weighted mean of its
 * valence across every memory about it, in the valence range. Zero for a NULL
 * memory, nobody as the subject or no salience left to weigh. */
int32_t SosMemoriesFeelingAbout(const SosMemories *memories, uint64_t subject, uint64_t tick);

/* Where the most salient memory of `kind` (SOS_EVENT_NONE for any kind) took
 * place at `tick`, written to `outPlace`. False for a NULL argument or no
 * match, leaving `outPlace` untouched. */
bool SosMemoriesPlaceOf(const SosMemories *memories, uint32_t kind, uint64_t tick,
                        SosPosition *outPlace);

/* Drops the memories that decayed to no salience by `tick`, keeping the rest in
 * order, and returns how many went. Zero for a NULL memory. */
uint32_t SosMemoriesForget(SosMemories *memories, uint64_t tick);

/* Folds the state into one value. The fold does not depend on where each record
 * sits, so two machines that reached the same memories by different routes
 * agree; zero for a NULL memory. */
uint64_t SosMemoriesHash(const SosMemories *memories);

#endif
