#include "sim/status.h"

#include "sim/random.h"

#include <stddef.h>
#include <stdlib.h>

/* What has been stored for one being. Both measures are held including the
 * baseline, so reading a record is a plain decay of its fields. */
typedef struct StatusRecord
{
    SosQ16 prestige;
    SosQ16 dominance;
    uint64_t lastTick;
} StatusRecord;

struct SosStatus
{
    uint64_t seed;
    SosTable *records;
};

/* All state of one being lives under a single key; a second key is left for a
 * future pair-shaped fact such as a grudge. */
static SosTableKey Key(uint64_t being)
{
    return (SosTableKey){.first = being, .second = 0u};
}

SosStatus *SosStatusCreate(uint64_t worldSeed)
{
    SosStatus *status = calloc(1u, sizeof(*status));
    if (status == NULL)
    {
        return NULL;
    }
    status->records = SosTableCreate(sizeof(StatusRecord));
    if (status->records == NULL)
    {
        free(status);
        return NULL;
    }
    status->seed = SosHashCombine(worldSeed, UINT64_C(0x737461747573)); /* "status" */
    return status;
}

void SosStatusDestroy(SosStatus *status)
{
    if (status != NULL)
    {
        SosTableDestroy(status->records);
        free(status);
    }
}

/* `value` after the time from `since` to `now` without renewal, moving toward
 * the baseline from either side: below it a shamed being climbs back, above it
 * earned standing fades. */
static SosQ16 DecayToBaseline(SosQ16 value, uint64_t since, uint64_t now, uint64_t halfLife)
{
    const int64_t excess = (int64_t)value - SOS_STATUS_BASELINE;
    const int64_t decayed = SOS_STATUS_BASELINE + SosDecaySince(excess, since, now, halfLife);
    return SosQ16Clamp((SosQ16)decayed, 0, SOS_Q16_ONE);
}

/* Reads a record as it stands at `tick`. */
static void Stamp(const StatusRecord *record, uint64_t tick, SosStanding *outStanding)
{
    outStanding->prestige =
        DecayToBaseline(record->prestige, record->lastTick, tick, SOS_STATUS_PRESTIGE_HALF_LIFE);
    outStanding->dominance =
        DecayToBaseline(record->dominance, record->lastTick, tick, SOS_STATUS_DOMINANCE_HALF_LIFE);
}

/* A positive gain is scaled by the square of the room left above the current
 * standing: the curve of diminishing returns. Linear headroom would let a long
 * career creep to the cap in a few great deeds; squaring it makes each step
 * near the top small while the same deed still moves someone unknown. */
static SosQ16 Diminish(SosQ16 delta, SosQ16 current)
{
    const SosQ16 headroom = (SosQ16)(SOS_Q16_ONE - current);
    return SosQ16Mul(SosQ16Mul(delta, headroom), headroom);
}

/* The value after one delta: gains shrink near the top, losses are direct. */
static SosQ16 Applied(SosQ16 value, SosQ16 delta)
{
    if (delta > 0)
    {
        const int64_t raised = (int64_t)value + Diminish(delta, value);
        return SosQ16Clamp((SosQ16)raised, 0, SOS_Q16_ONE);
    }
    const int64_t lowered = (int64_t)value + delta;
    return SosQ16Clamp((SosQ16)lowered, 0, SOS_Q16_ONE);
}

/* Writes `standing` as the record of `being` at `tick`, creating it when
 * needed. False when memory ran out, leaving the table unchanged. A tick from
 * before the record's last write keeps the later stamp, so decay never runs
 * backwards. */
static bool Store(SosStatus *status, uint64_t being, uint64_t tick, SosStanding standing)
{
    bool created = false;
    StatusRecord *record = SosTableInsert(status->records, Key(being), &created);
    if (record == NULL)
    {
        return false;
    }
    record->prestige = standing.prestige;
    record->dominance = standing.dominance;
    if (tick > record->lastTick)
    {
        record->lastTick = tick;
    }
    return true;
}

bool SosStatusGet(const SosStatus *status, uint64_t being, uint64_t tick, SosStanding *outStanding)
{
    if (status == NULL || outStanding == NULL || being == 0u)
    {
        return false;
    }
    const StatusRecord *record = SosTableFind(status->records, Key(being));
    if (record == NULL)
    {
        *outStanding =
            (SosStanding){.prestige = SOS_STATUS_BASELINE, .dominance = SOS_STATUS_BASELINE};
    }
    else
    {
        Stamp(record, tick, outStanding);
    }
    return true;
}

bool SosStatusAward(SosStatus *status, uint64_t being, uint64_t tick, SosQ16 prestigeDelta,
                    SosQ16 dominanceDelta)
{
    if (status == NULL || being == 0u)
    {
        return false;
    }
    if (prestigeDelta == 0 && dominanceDelta == 0)
    {
        return true;
    }
    SosStanding current;
    (void)SosStatusGet(status, being, tick, &current);
    const SosStanding standing = {
        .prestige = Applied(current.prestige, prestigeDelta),
        .dominance = Applied(current.dominance, dominanceDelta),
    };
    return Store(status, being, tick, standing);
}

/* The weighted standing that ranking compares. */
static int64_t Score(const SosStatus *status, uint64_t tick, uint64_t being)
{
    SosStanding standing = {.prestige = SOS_STATUS_BASELINE, .dominance = SOS_STATUS_BASELINE};
    (void)SosStatusGet(status, being, tick, &standing);
    return (int64_t)standing.prestige * SOS_STATUS_RANK_PRESTIGE_WEIGHT +
           (int64_t)standing.dominance * SOS_STATUS_RANK_DOMINANCE_WEIGHT;
}

static bool ComesBefore(const SosStatus *status, uint64_t tick, uint64_t a, uint64_t b)
{
    const int64_t scoreA = Score(status, tick, a);
    const int64_t scoreB = Score(status, tick, b);
    if (scoreA != scoreB)
    {
        return scoreA > scoreB;
    }
    return a < b;
}

static void Swap(uint64_t *a, uint64_t *b)
{
    const uint64_t value = *a;
    *a = *b;
    *b = value;
}

/* Restores the heap property below `root`, where a parent comes before its
 * children. */
static void SiftDown(const SosStatus *status, uint64_t tick, uint64_t *order, uint32_t count,
                     uint32_t root)
{
    for (;;)
    {
        const uint32_t left = 2u * root + 1u;
        if (left >= count)
        {
            return;
        }
        uint32_t child = left;
        if (left + 1u < count && ComesBefore(status, tick, order[left + 1u], order[left]))
        {
            child = left + 1u;
        }
        if (!ComesBefore(status, tick, order[child], order[root]))
        {
            return;
        }
        Swap(&order[root], &order[child]);
        root = child;
    }
}

/* In-place heapsort: no allocation, so a ranking cannot fail halfway, and the
 * comparator is a total order, so no machine can differ. */
static void Sort(const SosStatus *status, uint64_t tick, uint64_t *order, uint32_t count)
{
    for (uint32_t start = count / 2u; start > 0u; --start)
    {
        SiftDown(status, tick, order, count, start - 1u);
    }
    for (uint32_t end = count; end > 1u; --end)
    {
        Swap(&order[0], &order[end - 1u]);
        SiftDown(status, tick, order, end - 1u, 0u);
    }
    /* Heapsort leaves the least first; a ranking reads the greatest first. */
    for (uint32_t index = 0u; index < count / 2u; ++index)
    {
        Swap(&order[index], &order[count - 1u - index]);
    }
}

uint32_t SosStatusRank(const SosStatus *status, uint64_t tick, const uint64_t *beings,
                       uint32_t count, uint64_t *outOrdered)
{
    if (status == NULL || count == 0u)
    {
        return 0u;
    }
    if (beings == NULL || outOrdered == NULL)
    {
        return 0u;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        outOrdered[index] = beings[index];
    }
    Sort(status, tick, outOrdered, count);
    return count;
}

bool SosStatusInherit(SosStatus *status, uint64_t child, uint64_t mother, uint64_t father,
                      uint64_t tick)
{
    if (status == NULL || child == 0u || (mother == 0u && father == 0u))
    {
        return false;
    }
    int64_t total = 0;
    uint32_t parents = 0u;
    if (mother != 0u)
    {
        SosStanding parent;
        (void)SosStatusGet(status, mother, tick, &parent);
        total += parent.prestige;
        ++parents;
    }
    if (father != 0u)
    {
        SosStanding parent;
        (void)SosStatusGet(status, father, tick, &parent);
        total += parent.prestige;
        ++parents;
    }
    const SosQ16 inherited =
        SosQ16Mul((SosQ16)(total / (int64_t)parents), SOS_STATUS_INHERIT_SHARE);
    if (inherited == 0)
    {
        return true;
    }
    SosStanding childStanding;
    (void)SosStatusGet(status, child, tick, &childStanding);
    const int64_t raised = (int64_t)childStanding.prestige + inherited;
    const SosStanding standing = {
        .prestige = SosQ16Clamp((SosQ16)raised, 0, SOS_Q16_ONE),
        .dominance = childStanding.dominance,
    };
    return Store(status, child, tick, standing);
}

bool SosStatusForget(SosStatus *status, uint64_t being)
{
    if (status == NULL || being == 0u)
    {
        return false;
    }
    return SosTableRemove(status->records, Key(being));
}

/* The context RemoveIf carries: the tick decay is measured at. */
static bool PruneRecord(SosTableKey key, const void *record, void *context)
{
    (void)key;
    SosStanding standing;
    Stamp((const StatusRecord *)record, *(const uint64_t *)context, &standing);
    return standing.prestige == SOS_STATUS_BASELINE && standing.dominance == SOS_STATUS_BASELINE;
}

uint32_t SosStatusPrune(SosStatus *status, uint64_t tick)
{
    if (status == NULL)
    {
        return 0u;
    }
    return SosTableRemoveIf(status->records, PruneRecord, &tick);
}

static uint64_t HashStatus(SosTableKey key, const void *record, void *context)
{
    const uint64_t tick = *(const uint64_t *)context;
    SosStanding standing;
    Stamp((const StatusRecord *)record, tick, &standing);
    if (standing.prestige == SOS_STATUS_BASELINE && standing.dominance == SOS_STATUS_BASELINE)
    {
        /* A record at the baseline is left out so pruning cannot change it. */
        return 0u;
    }
    const uint64_t values = SosHashCombine((uint64_t)(uint32_t)standing.prestige,
                                           (uint64_t)(uint32_t)standing.dominance);
    return SosHash64(SosHashCombine(key.first, values));
}

uint64_t SosStatusHash(const SosStatus *status, uint64_t tick)
{
    if (status == NULL)
    {
        return 0u;
    }
    return SosHashCombine(status->seed, SosTableFold(status->records, HashStatus, &tick));
}
