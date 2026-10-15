#include "sim/relations.h"

#include "sim/links.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>

/* The stored relation: the public feeling plus the tick it was last touched,
 * from which it decays lazily. The threads that used to make up each being's
 * outgoing list now live in the shared SosLinks set. */
typedef struct StoredRelation
{
    SosQ16 affinity;
    SosQ16 trust;
    SosQ16 familiarity;
    uint64_t lastTick;
} StoredRelation;

struct SosRelations
{
    /* Keyed (from, to), both non-zero. */
    SosTable *relations;
    /* (from, to) pairs: a being's outgoing list in the order relations arose. */
    SosLinks *links;
};

/* The tick a prune runs at and the links it must keep in step. */
typedef struct PruneContext
{
    uint64_t tick;
    SosLinks *links;
} PruneContext;

static SosTableKey RelationKey(uint64_t from, uint64_t to)
{
    return (SosTableKey){.first = from, .second = to};
}

static const StoredRelation *FindStored(const SosRelations *relations, uint64_t from, uint64_t to)
{
    return (const StoredRelation *)SosTableFind(relations->relations, RelationKey(from, to));
}

static SosQ16 ClampFeeling(int64_t value)
{
    if (value > SOS_Q16_ONE)
    {
        return SOS_Q16_ONE;
    }
    return value < -SOS_Q16_ONE ? -SOS_Q16_ONE : (SosQ16)value;
}

static SosQ16 ClampFamiliarity(int64_t value)
{
    if (value > SOS_Q16_ONE)
    {
        return SOS_Q16_ONE;
    }
    return value < 0 ? 0 : (SosQ16)value;
}

SosRelations *SosRelationsCreate(void)
{
    SosRelations *relations = calloc(1u, sizeof(*relations));
    if (relations != NULL)
    {
        relations->relations = SosTableCreate(sizeof(StoredRelation));
        relations->links = SosLinksCreate();
        if (relations->relations == NULL || relations->links == NULL)
        {
            SosRelationsDestroy(relations);
            return NULL;
        }
    }
    return relations;
}

void SosRelationsDestroy(SosRelations *relations)
{
    if (relations != NULL)
    {
        SosTableDestroy(relations->relations);
        SosLinksDestroy(relations->links);
        free(relations);
    }
}

/* The relation decayed to `tick` from the tick it was last touched. */
static SosRelation Evaluate(const StoredRelation *stored, uint64_t tick)
{
    SosRelation relation;
    relation.affinity = (SosQ16)SosDecaySince(stored->affinity, stored->lastTick, tick,
                                              SOS_RELATIONS_AFFINITY_HALF_LIFE);
    relation.trust =
        (SosQ16)SosDecaySince(stored->trust, stored->lastTick, tick, SOS_RELATIONS_TRUST_HALF_LIFE);
    relation.familiarity = (SosQ16)SosDecaySince(stored->familiarity, stored->lastTick, tick,
                                                 SOS_RELATIONS_FAMILIARITY_HALF_LIFE);
    return relation;
}

/* The affinity `from` has for `to` at `tick`, neutral for a stranger. */
static SosQ16 AffinityAt(const SosRelations *relations, uint64_t from, uint64_t to, uint64_t tick)
{
    const StoredRelation *stored = FindStored(relations, from, to);
    return stored != NULL ? Evaluate(stored, tick).affinity : 0;
}

bool SosRelationsGet(const SosRelations *relations, uint64_t from, uint64_t to, uint64_t tick,
                     SosRelation *out)
{
    if (out != NULL)
    {
        *out = (SosRelation){0, 0, 0};
    }
    if (relations == NULL || out == NULL || from == 0u || to == 0u)
    {
        return false;
    }
    const StoredRelation *stored = FindStored(relations, from, to);
    if (stored == NULL)
    {
        return false;
    }
    *out = Evaluate(stored, tick);
    return true;
}

bool SosRelationsAdjust(SosRelations *relations, uint64_t from, uint64_t to, uint64_t tick,
                        SosQ16 affinityDelta, SosQ16 trustDelta)
{
    if (relations == NULL || from == 0u || to == 0u || from == to)
    {
        return false;
    }
    bool created = false;
    StoredRelation *stored =
        (StoredRelation *)SosTableInsert(relations->relations, RelationKey(from, to), &created);
    if (stored == NULL)
    {
        return false;
    }
    if (created && !SosLinksAdd(relations->links, from, to))
    {
        (void)SosTableRemove(relations->relations, RelationKey(from, to));
        return false;
    }
    stored->affinity = ClampFeeling(
        SosDecaySince(stored->affinity, stored->lastTick, tick, SOS_RELATIONS_AFFINITY_HALF_LIFE) +
        affinityDelta);
    stored->trust = ClampFeeling(
        SosDecaySince(stored->trust, stored->lastTick, tick, SOS_RELATIONS_TRUST_HALF_LIFE) +
        trustDelta);
    stored->familiarity =
        ClampFamiliarity(SosDecaySince(stored->familiarity, stored->lastTick, tick,
                                       SOS_RELATIONS_FAMILIARITY_HALF_LIFE) +
                         SOS_RELATIONS_FAMILIARITY_STEP);
    stored->lastTick = tick;
    return true;
}

SosQ16 SosRelationsMutual(const SosRelations *relations, uint64_t a, uint64_t b, uint64_t tick)
{
    if (relations == NULL || a == 0u || b == 0u || a == b)
    {
        return 0;
    }
    const int64_t both =
        (int64_t)AffinityAt(relations, a, b, tick) + (int64_t)AffinityAt(relations, b, a, tick);
    return (SosQ16)(both / 2);
}

uint32_t SosRelationsOf(const SosRelations *relations, uint64_t from, uint64_t tick,
                        SosRelationEntry *out, uint32_t capacity)
{
    if (relations == NULL || from == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t count = 0u;
    for (uint64_t to = SosLinksFirst(relations->links, from); to != 0u;
         to = SosLinksNext(relations->links, from, to))
    {
        const StoredRelation *stored = FindStored(relations, from, to);
        if (stored == NULL)
        {
            continue;
        }
        if (count < capacity)
        {
            out[count].other = to;
            out[count].relation = Evaluate(stored, tick);
        }
        ++count;
    }
    return count;
}

/* True when a candidate ranks before an existing one: stronger first, ties to
 * the lower serial, so every machine breaks a tie the same way. */
static bool RanksBefore(bool positive, SosQ16 candidate, uint64_t candidateSerial, SosQ16 existing,
                        uint64_t existingSerial)
{
    if (candidate != existing)
    {
        return positive ? candidate > existing : candidate < existing;
    }
    return candidateSerial < existingSerial;
}

uint32_t SosRelationsStrongest(const SosRelations *relations, uint64_t from, uint64_t tick,
                               bool positive, uint64_t *out, uint32_t capacity)
{
    if (relations == NULL || from == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t count = 0u;
    uint32_t filled = 0u;
    for (uint64_t to = SosLinksFirst(relations->links, from); to != 0u;
         to = SosLinksNext(relations->links, from, to))
    {
        const StoredRelation *stored = FindStored(relations, from, to);
        if (stored == NULL)
        {
            continue;
        }
        ++count;
        const SosQ16 affinity = Evaluate(stored, tick).affinity;
        uint32_t position = filled;
        while (position > 0u && RanksBefore(positive, affinity, to,
                                            AffinityAt(relations, from, out[position - 1u], tick),
                                            out[position - 1u]))
        {
            --position;
        }
        if (position < capacity)
        {
            /* Shifting right drops the weakest when the buffer is already full. */
            const uint32_t last = filled < capacity ? filled : capacity - 1u;
            for (uint32_t index = last; index > position; --index)
            {
                out[index] = out[index - 1u];
            }
            out[position] = to;
            if (filled < capacity)
            {
                ++filled;
            }
        }
    }
    return count;
}

uint32_t SosRelationsForget(SosRelations *relations, uint64_t being)
{
    if (relations == NULL || being == 0u)
    {
        return 0u;
    }
    /* The being's links name exactly the payload records to drop, so forgetting
     * costs O(degree) rather than a sweep of the whole graph. Removing payload
     * records leaves the links untouched, so the walk stays valid; the links are
     * then removed in one go. */
    uint32_t forgotten = 0u;
    for (uint64_t to = SosLinksFirst(relations->links, being); to != 0u;
         to = SosLinksNext(relations->links, being, to))
    {
        if (SosTableRemove(relations->relations, RelationKey(being, to)))
        {
            ++forgotten;
        }
    }
    (void)SosLinksRemoveOwner(relations->links, being);
    return forgotten;
}

/* True for a relation that has faded below the prune thresholds at the tick
 * held in the context. Removing a payload record also drops its link, which is
 * a different table and so cannot disturb this sweep. */
static bool FadedToNothing(SosTableKey key, const void *record, void *context)
{
    const PruneContext *prune = context;
    const SosRelation value = Evaluate((const StoredRelation *)record, prune->tick);
    if (value.familiarity < SOS_RELATIONS_PRUNE_FAMILIARITY &&
        value.affinity > -SOS_RELATIONS_PRUNE_FEELING &&
        value.affinity < SOS_RELATIONS_PRUNE_FEELING &&
        value.trust > -SOS_RELATIONS_PRUNE_FEELING && value.trust < SOS_RELATIONS_PRUNE_FEELING)
    {
        (void)SosLinksRemove(prune->links, key.first, key.second);
        return true;
    }
    return false;
}

uint32_t SosRelationsPrune(SosRelations *relations, uint64_t tick)
{
    if (relations == NULL)
    {
        return 0u;
    }
    const PruneContext prune = {.tick = tick, .links = relations->links};
    return SosTableRemoveIf(relations->relations, FadedToNothing, (void *)&prune);
}

static uint64_t HashRelation(SosTableKey key, const void *record, void *context)
{
    if (key.second == 0u)
    {
        return 0u;
    }
    const uint64_t tick = *(const uint64_t *)context;
    const SosRelation value = Evaluate((const StoredRelation *)record, tick);
    if (value.affinity == 0 && value.trust == 0 && value.familiarity == 0)
    {
        /* An entry that has decayed to neutral counts as absent. */
        return 0u;
    }
    uint64_t entry = SosHashCombine(SosHash64(key.first), key.second);
    entry = SosHashCombine(entry, (uint64_t)(int64_t)value.affinity);
    entry = SosHashCombine(entry, (uint64_t)(int64_t)value.trust);
    entry = SosHashCombine(entry, (uint64_t)(int64_t)value.familiarity);
    return entry;
}

uint64_t SosRelationsHash(const SosRelations *relations, uint64_t tick)
{
    if (relations == NULL)
    {
        return 0u;
    }
    return SosTableFold(relations->relations, HashRelation, &tick);
}
