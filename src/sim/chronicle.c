#include "sim/chronicle.h"

#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>
#include <string.h>

/* Index arithmetic widens the public constant once and for all. */
#define EVENTS_PER_CHUNK ((uint64_t)SOS_CHRONICLE_EVENTS_PER_CHUNK)
/* First size of the chunk-pointer array; most chronicles stay short. */
#define INITIAL_CHUNK_CAPACITY 4u
/* A link to a previous event is stored as index + 1, so zero can mean "none"
 * even though event index zero is a real event. */
#define LINK_NONE UINT64_C(0)

/* A stored event plus the links that make biographies walkable. */
typedef struct EventRecord
{
    SosEvent event;
    /* Index + 1 of the previous event with the same actor, or LINK_NONE. */
    uint64_t previousActor;
    /* Index + 1 of the previous event with the same subject, or LINK_NONE. */
    uint64_t previousSubject;
} EventRecord;

typedef struct EventChunk
{
    EventRecord events[SOS_CHRONICLE_EVENTS_PER_CHUNK];
} EventChunk;

/* Per being: the newest event it acted in and the newest it was the subject of,
 * each as index + 1 so zero means "none". */
typedef struct BeingIndex
{
    uint64_t newestActor;
    uint64_t newestSubject;
} BeingIndex;

struct SosChronicle
{
    SosTable *beings;
    /* Chunk `firstChunk + slot` is chunks[slot]; NULL never occurs among the
     * first chunkCount slots. */
    EventChunk **chunks;
    uint32_t chunkCapacity;
    uint32_t chunkCount;
    uint64_t firstChunk;
    /* Events ever appended; the newest index is count - 1. */
    uint64_t count;
    /* Tick of the newest event, kept after forgetting so ordering still holds. */
    uint64_t lastTick;
    bool hasEvents;
};

SosChronicle *SosChronicleCreate(void)
{
    SosChronicle *chronicle = calloc(1u, sizeof(*chronicle));
    if (chronicle == NULL)
    {
        return NULL;
    }
    chronicle->beings = SosTableCreate(sizeof(BeingIndex));
    if (chronicle->beings == NULL)
    {
        free(chronicle);
        return NULL;
    }
    return chronicle;
}

void SosChronicleDestroy(SosChronicle *chronicle)
{
    if (chronicle != NULL)
    {
        for (uint32_t slot = 0u; slot < chronicle->chunkCount; ++slot)
        {
            free(chronicle->chunks[slot]);
        }
        free((void *)chronicle->chunks);
        SosTableDestroy(chronicle->beings);
        free(chronicle);
    }
}

/* The first index the log can still hold; everything before it was forgotten. */
static uint64_t FirstIndex(const SosChronicle *chronicle)
{
    return chronicle->firstChunk * EVENTS_PER_CHUNK;
}

/* The record at a global index, or NULL when it is out of range, lies in a
 * forgotten chunk or the chunk is missing. */
static const EventRecord *FindRecord(const SosChronicle *chronicle, uint64_t index)
{
    if (index < FirstIndex(chronicle) || index >= chronicle->count)
    {
        return NULL;
    }
    const uint64_t slot = index / EVENTS_PER_CHUNK - chronicle->firstChunk;
    if (slot >= (uint64_t)chronicle->chunkCount)
    {
        return NULL;
    }
    const EventChunk *chunk = chronicle->chunks[(size_t)slot];
    if (chunk == NULL)
    {
        return NULL;
    }
    return &chunk->events[(size_t)(index % EVENTS_PER_CHUNK)];
}

/* Makes room for one more chunk pointer when the array is full. */
static bool GrowChunks(SosChronicle *chronicle)
{
    if (chronicle->chunkCount < chronicle->chunkCapacity)
    {
        return true;
    }
    uint32_t capacity;
    if (chronicle->chunkCapacity == 0u)
    {
        capacity = INITIAL_CHUNK_CAPACITY;
    }
    else
    {
        if (chronicle->chunkCapacity > UINT32_MAX / 2u)
        {
            return false;
        }
        capacity = chronicle->chunkCapacity * 2u;
    }
    EventChunk **chunks =
        (EventChunk **)realloc((void *)chronicle->chunks, (size_t)capacity * sizeof(*chunks));
    if (chunks == NULL)
    {
        return false;
    }
    chronicle->chunks = chunks;
    chronicle->chunkCapacity = capacity;
    return true;
}

/* The record at `index`, allocating the chunk that holds it when needed; NULL
 * on out-of-order indices or when memory ran out, leaving the log unchanged. */
static EventRecord *EnsureRecord(SosChronicle *chronicle, uint64_t index)
{
    const uint64_t chunk = index / EVENTS_PER_CHUNK;
    if (chunk < chronicle->firstChunk)
    {
        return NULL;
    }
    const uint64_t slot = chunk - chronicle->firstChunk;
    if (slot > (uint64_t)chronicle->chunkCount)
    {
        return NULL;
    }
    if (slot == (uint64_t)chronicle->chunkCount)
    {
        if (!GrowChunks(chronicle))
        {
            return NULL;
        }
        EventChunk *memory = malloc(sizeof(*memory));
        if (memory == NULL)
        {
            return NULL;
        }
        chronicle->chunks[(size_t)slot] = memory;
        chronicle->chunkCount += 1u;
    }
    return &chronicle->chunks[(size_t)slot]->events[(size_t)(index % EVENTS_PER_CHUNK)];
}

static SosTableKey BeingKey(uint64_t serial)
{
    return (SosTableKey){.first = serial, .second = 0u};
}

bool SosChronicleAppend(SosChronicle *chronicle, const SosEvent *event)
{
    if (chronicle == NULL || event == NULL || event->kind == SOS_EVENT_NONE)
    {
        return false;
    }
    if (chronicle->hasEvents && event->tick < chronicle->lastTick)
    {
        return false;
    }
    const uint64_t index = chronicle->count;
    EventRecord *record = EnsureRecord(chronicle, index);
    if (record == NULL)
    {
        return false;
    }
    /* Index the being before touching the log; a failure here leaves the log
     * exactly as it was. */
    bool createdActor = false;
    if (event->actor != 0u &&
        SosTableInsert(chronicle->beings, BeingKey(event->actor), &createdActor) == NULL)
    {
        return false;
    }
    if (event->subject != 0u && event->subject != event->actor &&
        SosTableInsert(chronicle->beings, BeingKey(event->subject), NULL) == NULL)
    {
        if (createdActor)
        {
            (void)SosTableRemove(chronicle->beings, BeingKey(event->actor));
        }
        return false;
    }
    /* Records move when the table changes, so the pointers are taken only after
     * the last insert. */
    BeingIndex *actor =
        event->actor != 0u ? SosTableEdit(chronicle->beings, BeingKey(event->actor)) : NULL;
    BeingIndex *subject =
        event->subject != 0u ? SosTableEdit(chronicle->beings, BeingKey(event->subject)) : NULL;
    const uint64_t previousActor = actor != NULL ? actor->newestActor : LINK_NONE;
    const uint64_t previousSubject = subject != NULL ? subject->newestSubject : LINK_NONE;

    record->event = *event;
    record->previousActor = previousActor;
    record->previousSubject = previousSubject;

    if (actor != NULL)
    {
        actor->newestActor = index + 1u;
    }
    if (subject != NULL)
    {
        subject->newestSubject = index + 1u;
    }
    chronicle->count = index + 1u;
    chronicle->lastTick = event->tick;
    chronicle->hasEvents = true;
    return true;
}

uint64_t SosChronicleCount(const SosChronicle *chronicle)
{
    return chronicle != NULL ? chronicle->count : 0u;
}

bool SosChronicleGet(const SosChronicle *chronicle, uint64_t index, SosEvent *outEvent)
{
    if (chronicle == NULL || outEvent == NULL)
    {
        return false;
    }
    const EventRecord *record = FindRecord(chronicle, index);
    if (record == NULL)
    {
        return false;
    }
    *outEvent = record->event;
    return true;
}

/* The earliest index whose event tick is at least `tick`; count when there is
 * none. Ticks never decrease, so the range is sorted and binary search is
 * exact. */
static uint64_t LowerBound(const SosChronicle *chronicle, uint64_t tick)
{
    uint64_t low = FirstIndex(chronicle);
    uint64_t high = chronicle->count;
    while (low < high)
    {
        const uint64_t middle = low + (high - low) / 2u;
        const EventRecord *record = FindRecord(chronicle, middle);
        if (record != NULL && record->event.tick < tick)
        {
            low = middle + 1u;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

uint64_t SosChronicleFirstAt(const SosChronicle *chronicle, uint64_t tick)
{
    return chronicle != NULL ? LowerBound(chronicle, tick) : 0u;
}

uint32_t SosChronicleOfBeing(const SosChronicle *chronicle, uint64_t serial, uint64_t *outIndices,
                             uint32_t capacity)
{
    if (chronicle == NULL || serial == 0u || (outIndices == NULL && capacity != 0u))
    {
        return 0u;
    }
    const BeingIndex *index = SosTableFind(chronicle->beings, BeingKey(serial));
    if (index == NULL)
    {
        return 0u;
    }
    /* Two cursors, one per chain, each an index + 1; the newest of the two is
     * the next event of the biography, and stepping it back to its own
     * predecessor keeps the merged walk descending. */
    uint64_t actor = index->newestActor;
    uint64_t subject = index->newestSubject;
    uint32_t found = 0u;
    while (actor != LINK_NONE || subject != LINK_NONE)
    {
        const uint64_t current =
            actor != LINK_NONE && (subject == LINK_NONE || actor >= subject) ? actor : subject;
        const uint64_t eventIndex = current - 1u;
        const EventRecord *record = FindRecord(chronicle, eventIndex);
        if (record == NULL)
        {
            /* Forgetting removes a prefix, so nothing older survives either. */
            break;
        }
        if (found < capacity)
        {
            outIndices[found] = eventIndex;
        }
        if (found != UINT32_MAX)
        {
            found += 1u;
        }
        if (actor == current)
        {
            actor = record->previousActor;
        }
        if (subject == current)
        {
            subject = record->previousSubject;
        }
    }
    return found;
}

void SosChronicleForgetBefore(SosChronicle *chronicle, uint64_t tick)
{
    if (chronicle == NULL)
    {
        return;
    }
    const uint64_t keep = LowerBound(chronicle, tick) / EVENTS_PER_CHUNK;
    if (keep <= chronicle->firstChunk)
    {
        return;
    }
    const uint64_t release = keep - chronicle->firstChunk;
    for (uint64_t slot = 0u; slot < release; ++slot)
    {
        free(chronicle->chunks[(size_t)slot]);
    }
    const uint32_t remaining = chronicle->chunkCount - (uint32_t)release;
    memmove((void *)chronicle->chunks, (const void *)(chronicle->chunks + (size_t)release),
            (size_t)remaining * sizeof(*chronicle->chunks));
    chronicle->chunkCount = remaining;
    chronicle->firstChunk = keep;
}

static uint64_t EventHash(const SosEvent *event)
{
    uint64_t hash = SosHash64(event->tick);
    hash = SosHashCombine(hash, (uint64_t)event->kind);
    hash = SosHashCombine(hash, event->actor);
    hash = SosHashCombine(hash, event->subject);
    hash = SosHashCombine(hash, event->group);
    hash = SosHashCombine(hash, (uint64_t)event->place.x);
    hash = SosHashCombine(hash, (uint64_t)event->place.y);
    hash = SosHashCombine(hash, (uint64_t)event->place.z);
    hash = SosHashCombine(hash, (uint64_t)event->value);
    return hash;
}

uint64_t SosChronicleHash(const SosChronicle *chronicle)
{
    if (chronicle == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on where the chunks keep each event, and mixing in
     * the index still catches a reordering. */
    uint64_t sum = 0u;
    for (uint64_t index = FirstIndex(chronicle); index < chronicle->count; ++index)
    {
        const EventRecord *record = FindRecord(chronicle, index);
        if (record != NULL)
        {
            sum += SosHashCombine(SosHash64(index), EventHash(&record->event));
        }
    }
    return SosHashCombine(SosHashCombine(chronicle->count, chronicle->lastTick), sum);
}
