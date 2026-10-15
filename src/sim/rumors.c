#include "sim/rumors.h"

#include "sim/calendar.h"
#include "sim/links.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>

/* A belief halves this often while it is not refreshed: gossip that is not
 * retold within a fortnight is old news. */
#define BELIEF_HALF_LIFE (14 * SOS_TICKS_PER_DAY)
/* Each retelling multiplies the belief by a per-hop factor in this range,
 * mostly near one, so a rumor survives a village but is never preserved
 * exactly. */
#define DISTORT_MIN (3 * SOS_Q16_ONE / 4)
#define DISTORT_MAX (5 * SOS_Q16_ONE / 4)
/* The low bits of a hash used as a draw. */
#define DRAW_MASK UINT64_C(0xFFFF)

/* What one being knows of one rumor, keyed (being, rumor). A being's knowledge
 * is an entry in the shared SosLinks set, so it is walked without scanning the
 * table. */
typedef struct Knowledge
{
    uint64_t heardTick;
    SosQ16 belief;
    uint16_t hops;
} Knowledge;

/* The tick a fading sweep runs at and the links it must keep in step. */
typedef struct FadeContext
{
    uint64_t tick;
    SosLinks *links;
} FadeContext;

struct SosRumors
{
    uint64_t seed;
    /* The next rumor id; ids are serials that are never reused. */
    uint64_t nextId;
    SosTable *rumors;
    SosTable *knowledge;
    /* (being, rumor) pairs: what each being knows. */
    SosLinks *links;
};

static SosTableKey RumorKey(uint64_t id)
{
    return (SosTableKey){.first = id, .second = 0u};
}

static SosTableKey MemoryKey(uint64_t being, uint64_t rumor)
{
    return (SosTableKey){.first = being, .second = rumor};
}

static SosQ16 ClampBelief(int64_t value)
{
    if (value <= 0)
    {
        return 0;
    }
    return value >= SOS_Q16_ONE ? (SosQ16)SOS_Q16_ONE : (SosQ16)value;
}

/* The belief of a memory faded to `tick`; zero once nothing is left. */
static SosQ16 BeliefAt(const Knowledge *memory, uint64_t tick)
{
    if (memory->belief <= 0)
    {
        return 0;
    }
    const uint64_t elapsed = tick > memory->heardTick ? tick - memory->heardTick : 0u;
    return ClampBelief(SosDecay(memory->belief, elapsed, BELIEF_HALF_LIFE));
}

/* The distortion a rumor suffers on its `hops`-th retelling: a hash-derived
 * factor in [DISTORT_MIN, DISTORT_MAX], the same on every machine. */
static SosQ16 Distortion(const SosRumors *rumors, uint64_t rumorId, uint16_t hops)
{
    const uint64_t hash = SosHashCombine(SosHashCombine(rumors->seed, rumorId), hops);
    const uint32_t span = (uint32_t)(DISTORT_MAX - DISTORT_MIN) + 1u;
    return (SosQ16)(DISTORT_MIN + (int32_t)((hash & DRAW_MASK) % (uint64_t)span));
}

SosRumors *SosRumorsCreate(uint64_t seed)
{
    SosRumors *rumors = calloc(1u, sizeof(*rumors));
    if (rumors == NULL)
    {
        return NULL;
    }
    rumors->seed = SosHashCombine(seed, UINT64_C(0x72756d6f7273)); /* "rumors" */
    rumors->nextId = 1u;
    rumors->rumors = SosTableCreate(sizeof(SosRumor));
    rumors->knowledge = SosTableCreate(sizeof(Knowledge));
    rumors->links = SosLinksCreate();
    if (rumors->rumors == NULL || rumors->knowledge == NULL || rumors->links == NULL)
    {
        SosRumorsDestroy(rumors);
        return NULL;
    }
    return rumors;
}

void SosRumorsDestroy(SosRumors *rumors)
{
    if (rumors != NULL)
    {
        SosTableDestroy(rumors->knowledge);
        SosTableDestroy(rumors->rumors);
        SosLinksDestroy(rumors->links);
        free(rumors);
    }
}

/* Records that `being` knows `rumorId` with `belief`, `hops` hops from the
 * source as of `tick`. False and unchanged when memory ran out. */
static bool Remember(SosRumors *rumors, uint64_t being, uint64_t rumorId, SosQ16 belief,
                     uint16_t hops, uint64_t tick)
{
    bool created = false;
    Knowledge *memory =
        (Knowledge *)SosTableInsert(rumors->knowledge, MemoryKey(being, rumorId), &created);
    if (memory == NULL)
    {
        return false;
    }
    if (created && !SosLinksAdd(rumors->links, being, rumorId))
    {
        (void)SosTableRemove(rumors->knowledge, MemoryKey(being, rumorId));
        return false;
    }
    memory->heardTick = tick;
    memory->belief = belief;
    memory->hops = hops;
    return true;
}

uint64_t SosRumorsStart(SosRumors *rumors, uint32_t kind, uint64_t subject, SosPosition place,
                        uint64_t origin, SosQ16 truth, uint64_t tick)
{
    if (rumors == NULL || kind == SOS_EVENT_NONE || truth < 0 || truth > SOS_Q16_ONE ||
        rumors->nextId == 0u)
    {
        return 0u;
    }
    const uint64_t id = rumors->nextId;
    const SosRumor rumor = {
        .id = id,
        .kind = kind,
        .subject = subject,
        .place = place,
        .originTick = tick,
        .truth = truth,
    };
    void *record = SosTableInsert(rumors->rumors, RumorKey(id), NULL);
    if (record == NULL)
    {
        return 0u;
    }
    *(SosRumor *)record = rumor;
    rumors->nextId = id + 1u;
    if (origin != 0u && !Remember(rumors, origin, id, truth, 0u, tick))
    {
        (void)SosTableRemove(rumors->rumors, RumorKey(id));
        rumors->nextId = id;
        return 0u;
    }
    return id;
}

bool SosRumorsGet(const SosRumors *rumors, uint64_t id, SosRumor *outRumor)
{
    if (rumors == NULL || id == 0u || outRumor == NULL)
    {
        return false;
    }
    const SosRumor *rumor = (const SosRumor *)SosTableFind(rumors->rumors, RumorKey(id));
    if (rumor == NULL)
    {
        return false;
    }
    *outRumor = *rumor;
    return true;
}

bool SosRumorsTell(SosRumors *rumors, uint64_t teller, uint64_t listener, uint64_t rumor,
                   uint64_t tick, SosQ16 trust)
{
    if (rumors == NULL || teller == 0u || listener == 0u || teller == listener || rumor == 0u ||
        trust < 0 || trust > SOS_Q16_ONE)
    {
        return false;
    }
    if (SosTableFind(rumors->rumors, RumorKey(rumor)) == NULL)
    {
        return false;
    }
    const Knowledge *tellerMemory =
        (const Knowledge *)SosTableFind(rumors->knowledge, MemoryKey(teller, rumor));
    if (tellerMemory == NULL)
    {
        return false;
    }
    const SosQ16 tellerBelief = BeliefAt(tellerMemory, tick);
    if (tellerBelief <= 0)
    {
        return false;
    }
    const uint16_t hops =
        tellerMemory->hops < UINT16_MAX ? (uint16_t)(tellerMemory->hops + 1u) : UINT16_MAX;
    const SosQ16 incoming =
        ClampBelief(SosQ16Mul(SosQ16Mul(tellerBelief, trust), Distortion(rumors, rumor, hops)));

    Knowledge *listenerMemory =
        (Knowledge *)SosTableEdit(rumors->knowledge, MemoryKey(listener, rumor));
    if (listenerMemory == NULL)
    {
        return Remember(rumors, listener, rumor, incoming, hops, tick);
    }
    const SosQ16 heard = BeliefAt(listenerMemory, tick);
    if (heard > 0)
    {
        /* Reinforced: a second telling moves the belief toward certainty. */
        listenerMemory->belief = (SosQ16)SosQ16Lerp(heard, SOS_Q16_ONE, incoming);
        if (hops < listenerMemory->hops)
        {
            listenerMemory->hops = hops;
        }
    }
    else
    {
        listenerMemory->belief = incoming;
        listenerMemory->hops = hops;
    }
    listenerMemory->heardTick = tick;
    return true;
}

bool SosRumorsBelief(const SosRumors *rumors, uint64_t being, uint64_t rumor, uint64_t tick,
                     SosQ16 *outBelief)
{
    if (rumors == NULL || being == 0u || rumor == 0u || outBelief == NULL)
    {
        return false;
    }
    const Knowledge *memory =
        (const Knowledge *)SosTableFind(rumors->knowledge, MemoryKey(being, rumor));
    if (memory == NULL)
    {
        return false;
    }
    const SosQ16 belief = BeliefAt(memory, tick);
    if (belief <= 0)
    {
        return false;
    }
    *outBelief = belief;
    return true;
}

uint32_t SosRumorsKnownBy(const SosRumors *rumors, uint64_t being, uint64_t tick, uint64_t *outIds,
                          uint32_t capacity)
{
    if (rumors == NULL || being == 0u || (outIds == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t count = 0u;
    for (uint64_t id = SosLinksFirst(rumors->links, being); id != 0u;
         id = SosLinksNext(rumors->links, being, id))
    {
        const Knowledge *memory =
            (const Knowledge *)SosTableFind(rumors->knowledge, MemoryKey(being, id));
        if (memory == NULL)
        {
            continue;
        }
        if (BeliefAt(memory, tick) > 0)
        {
            if (count < capacity)
            {
                outIds[count] = id;
            }
            count += 1u;
        }
    }
    return count;
}

/* True for a belief that has faded to nothing at the tick in the context.
 * Removing it also drops its link, a different table, so the sweep is safe. */
static bool Faded(SosTableKey key, const void *record, void *context)
{
    const FadeContext *fade = context;
    if (BeliefAt((const Knowledge *)record, fade->tick) > 0)
    {
        return false;
    }
    (void)SosLinksRemove(fade->links, key.first, key.second);
    return true;
}

uint32_t SosRumorsForgetFaded(SosRumors *rumors, uint64_t tick)
{
    if (rumors == NULL)
    {
        return 0u;
    }
    const FadeContext fade = {.tick = tick, .links = rumors->links};
    return SosTableRemoveIf(rumors->knowledge, Faded, (void *)&fade);
}

uint32_t SosRumorsForgetBeing(SosRumors *rumors, uint64_t being)
{
    if (rumors == NULL || being == 0u)
    {
        return 0u;
    }
    /* The being's links name exactly the beliefs to drop, so forgetting costs
     * O(degree) rather than a sweep of the whole store. Removing beliefs leaves
     * the links untouched, so the walk stays valid; the links are then removed
     * in one go. */
    uint32_t removed = 0u;
    for (uint64_t id = SosLinksFirst(rumors->links, being); id != 0u;
         id = SosLinksNext(rumors->links, being, id))
    {
        if (SosTableRemove(rumors->knowledge, MemoryKey(being, id)))
        {
            ++removed;
        }
    }
    (void)SosLinksRemoveOwner(rumors->links, being);
    return removed;
}

uint64_t SosRumorsHash(const SosRumors *rumors, uint64_t tick)
{
    if (rumors == NULL)
    {
        return 0u;
    }
    /* Sums of per-record hashes do not depend on where the tables keep a
     * record, which is what makes the hash agree across machines. */
    uint64_t rumorSum = 0u;
    const uint32_t rumorSlots = SosTableSlotCount(rumors->rumors);
    for (uint32_t slot = 0u; slot < rumorSlots; ++slot)
    {
        SosTableKey key;
        const void *record;
        if (!SosTableSlot(rumors->rumors, slot, &key, &record))
        {
            continue;
        }
        const SosRumor *rumor = (const SosRumor *)record;
        uint64_t value = SosHashCombine(SosHash64(rumor->id), rumor->kind);
        value = SosHashCombine(value, rumor->subject);
        value = SosHashCombine(value, (uint64_t)rumor->place.x);
        value = SosHashCombine(value, (uint64_t)rumor->place.y);
        value = SosHashCombine(value, (uint64_t)rumor->place.z);
        value = SosHashCombine(value, rumor->originTick);
        value = SosHashCombine(value, (uint64_t)(int64_t)rumor->truth);
        rumorSum += SosHash64(value);
    }
    uint64_t memorySum = 0u;
    const uint32_t memorySlots = SosTableSlotCount(rumors->knowledge);
    for (uint32_t slot = 0u; slot < memorySlots; ++slot)
    {
        SosTableKey key;
        const void *record;
        if (!SosTableSlot(rumors->knowledge, slot, &key, &record) || key.second == 0u)
        {
            continue;
        }
        const Knowledge *memory = (const Knowledge *)record;
        const SosQ16 belief = BeliefAt(memory, tick);
        if (belief <= 0)
        {
            continue;
        }
        uint64_t value = SosHashCombine(SosHash64(key.first), key.second);
        value = SosHashCombine(value, memory->hops);
        value = SosHashCombine(value, (uint64_t)(int64_t)belief);
        memorySum += SosHash64(value);
    }
    return SosHashCombine(SosHashCombine(rumors->seed, rumors->nextId),
                          SosHashCombine(rumorSum, memorySum));
}
