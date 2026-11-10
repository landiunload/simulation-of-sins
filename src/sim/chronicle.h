#ifndef SOS_SIM_CHRONICLE_H
#define SOS_SIM_CHRONICLE_H

#include "sim/events.h"
#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* The world's history book.
 *
 * Everything worth remembering in the world is appended here once and never
 * rewritten: births and deaths, bonds and separations, groups founded and
 * dissolved, discoveries, migrations, conflicts, sins and disasters. The god
 * player reads it as a timeline, as the biography of one being or as the story
 * of one place. The log is append-only, so an event keeps its global index
 * forever and a replay that appended the same events reads the same history on
 * every machine.
 *
 * Events live in fixed-size chunks allocated as the log grows, so appending is
 * O(1) and never moves an earlier event. A per-being index in a SosTable points
 * at each being's newest event, and every stored event links back to the
 * previous event of its actor and of its subject, so a biography can be walked
 * backwards from the newest entry. Whole old chunks can be freed to bound
 * memory; their indices then read as absent, which keeps every surviving index
 * stable. Only changes are stored: nothing here is touched while idle time
 * passes. */

/* Events per chunk. A chunk is freed as a unit, so this is the granularity of
 * SosChronicleForgetBefore. */
#define SOS_CHRONICLE_EVENTS_PER_CHUNK 4096u

/* One thing that happened. `kind` is a SosEventKind; events.h owns the
 * vocabulary, so every layer that records or reacts to history names an event
 * the same way. `actor` is whoever did it and `subject` whoever it was done to,
 * either 0 for none; `group` names a group when the event is about one. `place`
 * is where it happened and `value` a signed quantity whose meaning follows from
 * the kind (a death, a debt, a birth). */
typedef struct SosEvent
{
    uint64_t tick;
    uint32_t kind;
    uint64_t actor;
    uint64_t subject;
    uint64_t group;
    SosPosition place;
    int64_t value;
} SosEvent;

typedef struct SosChronicle SosChronicle;

/* An empty chronicle, or NULL when memory ran out. */
SosChronicle *SosChronicleCreate(void);
void SosChronicleDestroy(SosChronicle *chronicle);

/* Appends `event` as the newest entry. Ticks must not decrease: an event older
 * than the newest appended one is rejected. `event->kind` must not be
 * SOS_EVENT_NONE, which is not a happening and is never stored. False without
 * changing anything when `chronicle` or `event` is NULL, when the kind is
 * SOS_EVENT_NONE, when `event->tick` is smaller than the previous event's tick,
 * or when memory ran out. */
bool SosChronicleAppend(SosChronicle *chronicle, const SosEvent *event);

/* How many events were ever appended. Event indices are global and stable, so
 * they are 0..count-1 whether or not the chunk holding one was forgotten. */
uint64_t SosChronicleCount(const SosChronicle *chronicle);

/* The event at the global `index`; false when it is out of range, was
 * forgotten, or any argument is NULL. */
bool SosChronicleGet(const SosChronicle *chronicle, uint64_t index, SosEvent *outEvent);

/* The index of the earliest stored event whose tick is at least `tick`, found
 * by binary search over the log; SosChronicleCount when every stored event is
 * earlier, and 0 for a NULL chronicle. Forgotten events are absent, so a tick
 * older than the oldest stored event yields the oldest stored index. */
uint64_t SosChronicleFirstAt(const SosChronicle *chronicle, uint64_t tick);

/* The stored indices of the events whose actor or subject is `serial`, newest
 * first, written into `outIndices` up to `capacity`. Returns how many there
 * are, which may exceed `capacity`. Zero when `chronicle` is NULL, `serial` is
 * 0, or `outIndices` is NULL while `capacity` is non-zero. Events whose chunk
 * was forgotten are not listed. */
uint32_t SosChronicleOfBeing(const SosChronicle *chronicle, uint64_t serial, uint64_t *outIndices,
                             uint32_t capacity);

/* Frees whole chunks all of whose events are older than `tick`. The chunk that
 * straddles `tick` is kept, so some older events may survive; forgotten
 * indices read as absent from then on. Does nothing for a NULL chronicle. */
void SosChronicleForgetBefore(SosChronicle *chronicle, uint64_t tick);

/* Folds the stored events into one value: equal histories give equal hashes, so
 * replays and machines can be compared. Zero for a NULL chronicle. */
uint64_t SosChronicleHash(const SosChronicle *chronicle);

#endif
