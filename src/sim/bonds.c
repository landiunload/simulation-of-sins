#include "sim/bonds.h"

#include "sim/links.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>

/* A bond's payload. The partner lists that used to be threaded through here
 * now live in the shared SosLinks set, so a record holds only what callers
 * read back. */
typedef struct BondRecord
{
    SosBondKind kind;
    SosBondEnd endReason;
    uint32_t reunions;
    uint64_t formedTick;
    uint64_t endedTick;
} BondRecord;

/* How many active bonds a being holds, kept so the partner limit is answered
 * without walking the being's history. The history size itself is the shared
 * set's count for the being. */
typedef struct BeingBonds
{
    uint32_t active;
} BeingBonds;

struct SosBonds
{
    /* Keyed by the ordered pair, both halves non-zero. */
    SosTable *pairs;
    /* One pair (a, b) and (b, a) per bond, in the order the bonds formed. */
    SosLinks *links;
    /* Keyed (being, 0). */
    SosTable *counts;
};

/* The key of a bond does not depend on the order the pair was named in. */
static SosTableKey PairKey(uint64_t a, uint64_t b)
{
    return a < b ? (SosTableKey){.first = a, .second = b} : (SosTableKey){.first = b, .second = a};
}

/* Bond keys always have two non-zero halves, so a zero second half can only
 * name a being's own count entry. */
static SosTableKey BeingKey(uint64_t being)
{
    return (SosTableKey){.first = being, .second = 0u};
}

/* Casting through unsigned rejects negative values that a cast to the enum
 * could otherwise smuggle in. */
static bool ValidKind(SosBondKind kind)
{
    return (unsigned)kind < (unsigned)SOS_BOND_KIND_COUNT;
}

static bool ValidReason(SosBondEnd reason)
{
    return reason != SOS_BOND_END_NONE && (unsigned)reason < (unsigned)SOS_BOND_END_COUNT;
}

/* The being's active-bond count, created zero when missing. NULL when memory
 * ran out; `outCreated` (may be NULL) then tells whether this call made it. */
static BeingBonds *EnsureCount(SosBonds *bonds, uint64_t being, bool *outCreated)
{
    bool created = false;
    BeingBonds *count = (BeingBonds *)SosTableInsert(bonds->counts, BeingKey(being), &created);
    if (count != NULL && outCreated != NULL)
    {
        *outCreated = created;
    }
    return count;
}

/* Undoes the count entries a failed form created, so the set is unchanged. */
static void DropCounts(SosBonds *bonds, uint64_t a, bool createdA, uint64_t b, bool createdB)
{
    if (createdA)
    {
        (void)SosTableRemove(bonds->counts, BeingKey(a));
    }
    if (createdB)
    {
        (void)SosTableRemove(bonds->counts, BeingKey(b));
    }
}

SosBonds *SosBondsCreate(void)
{
    SosBonds *bonds = calloc(1u, sizeof(*bonds));
    if (bonds == NULL)
    {
        return NULL;
    }
    bonds->pairs = SosTableCreate(sizeof(BondRecord));
    bonds->links = SosLinksCreate();
    bonds->counts = SosTableCreate(sizeof(BeingBonds));
    if (bonds->pairs == NULL || bonds->links == NULL || bonds->counts == NULL)
    {
        SosBondsDestroy(bonds);
        return NULL;
    }
    return bonds;
}

void SosBondsDestroy(SosBonds *bonds)
{
    if (bonds != NULL)
    {
        SosTableDestroy(bonds->pairs);
        SosLinksDestroy(bonds->links);
        SosTableDestroy(bonds->counts);
        free(bonds);
    }
}

bool SosBondsForm(SosBonds *bonds, uint64_t a, uint64_t b, SosBondKind kind, uint64_t tick,
                  uint32_t maxPartners)
{
    if (bonds == NULL || a == 0u || b == 0u || a == b || !ValidKind(kind))
    {
        return false;
    }
    const SosTableKey pairKey = PairKey(a, b);
    const BondRecord *existing = (const BondRecord *)SosTableFind(bonds->pairs, pairKey);
    if (existing != NULL && existing->endReason == SOS_BOND_END_NONE)
    {
        return false;
    }
    const BeingBonds *countA = (const BeingBonds *)SosTableFind(bonds->counts, BeingKey(a));
    const BeingBonds *countB = (const BeingBonds *)SosTableFind(bonds->counts, BeingKey(b));
    if ((countA != NULL ? countA->active : 0u) >= maxPartners ||
        (countB != NULL ? countB->active : 0u) >= maxPartners)
    {
        return false;
    }
    const bool newPair = existing == NULL;
    bool createdPair = false;
    BondRecord *record = (BondRecord *)SosTableInsert(bonds->pairs, pairKey, &createdPair);
    if (record == NULL)
    {
        return false;
    }
    bool createdA = false;
    bool createdB = false;
    if (EnsureCount(bonds, a, &createdA) == NULL || EnsureCount(bonds, b, &createdB) == NULL)
    {
        DropCounts(bonds, a, createdA, b, createdB);
        if (createdPair)
        {
            (void)SosTableRemove(bonds->pairs, pairKey);
        }
        return false;
    }
    /* A fresh pair is added to both partner lists; a reunion is already listed
     * once and adding it again changes nothing. */
    if (newPair && !SosLinksAdd(bonds->links, a, b))
    {
        DropCounts(bonds, a, createdA, b, createdB);
        if (createdPair)
        {
            (void)SosTableRemove(bonds->pairs, pairKey);
        }
        return false;
    }
    if (!SosLinksAdd(bonds->links, b, a))
    {
        if (newPair)
        {
            (void)SosLinksRemove(bonds->links, a, b);
        }
        DropCounts(bonds, a, createdA, b, createdB);
        if (createdPair)
        {
            (void)SosTableRemove(bonds->pairs, pairKey);
        }
        return false;
    }
    record = (BondRecord *)SosTableEdit(bonds->pairs, pairKey);
    BeingBonds *activeA = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(a));
    BeingBonds *activeB = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(b));
    if (record == NULL || activeA == NULL || activeB == NULL)
    {
        return false;
    }
    record->reunions = newPair ? 0u : record->reunions + 1u;
    record->kind = kind;
    record->formedTick = tick;
    record->endedTick = 0u;
    record->endReason = SOS_BOND_END_NONE;
    activeA->active += 1u;
    activeB->active += 1u;
    return true;
}

bool SosBondsEnd(SosBonds *bonds, uint64_t a, uint64_t b, SosBondEnd reason, uint64_t tick)
{
    if (bonds == NULL || a == 0u || b == 0u || a == b || !ValidReason(reason))
    {
        return false;
    }
    BondRecord *record = (BondRecord *)SosTableEdit(bonds->pairs, PairKey(a, b));
    if (record == NULL || record->endReason != SOS_BOND_END_NONE || tick < record->formedTick)
    {
        return false;
    }
    record->endReason = reason;
    record->endedTick = tick;
    BeingBonds *countA = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(a));
    BeingBonds *countB = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(b));
    if (countA != NULL && countA->active > 0u)
    {
        countA->active -= 1u;
    }
    if (countB != NULL && countB->active > 0u)
    {
        countB->active -= 1u;
    }
    return true;
}

uint32_t SosBondsEndAllOf(SosBonds *bonds, uint64_t being, SosBondEnd reason, uint64_t tick)
{
    if (bonds == NULL || being == 0u || !ValidReason(reason))
    {
        return 0u;
    }
    BeingBonds *count = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(being));
    if (count == NULL)
    {
        return 0u;
    }
    uint32_t ended = 0u;
    for (uint64_t partner = SosLinksFirst(bonds->links, being); partner != 0u;
         partner = SosLinksNext(bonds->links, being, partner))
    {
        BondRecord *record = (BondRecord *)SosTableEdit(bonds->pairs, PairKey(being, partner));
        if (record == NULL || record->endReason != SOS_BOND_END_NONE || tick < record->formedTick)
        {
            continue;
        }
        record->endReason = reason;
        record->endedTick = tick;
        BeingBonds *other = (BeingBonds *)SosTableEdit(bonds->counts, BeingKey(partner));
        if (count->active > 0u)
        {
            count->active -= 1u;
        }
        if (other != NULL && other->active > 0u)
        {
            other->active -= 1u;
        }
        ended += 1u;
    }
    return ended;
}

bool SosBondsGet(const SosBonds *bonds, uint64_t a, uint64_t b, SosBond *outBond)
{
    if (bonds == NULL || outBond == NULL || a == 0u || b == 0u || a == b)
    {
        return false;
    }
    const SosTableKey pairKey = PairKey(a, b);
    const BondRecord *record = (const BondRecord *)SosTableFind(bonds->pairs, pairKey);
    if (record == NULL)
    {
        return false;
    }
    *outBond = (SosBond){
        .first = pairKey.first,
        .second = pairKey.second,
        .kind = record->kind,
        .endReason = record->endReason,
        .reunions = record->reunions,
        .formedTick = record->formedTick,
        .endedTick = record->endedTick,
    };
    return true;
}

uint32_t SosBondsPartners(const SosBonds *bonds, uint64_t being, bool activeOnly, uint64_t *out,
                          uint32_t capacity)
{
    if (bonds == NULL || being == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t found = 0u;
    for (uint64_t partner = SosLinksFirst(bonds->links, being); partner != 0u;
         partner = SosLinksNext(bonds->links, being, partner))
    {
        const BondRecord *record =
            (const BondRecord *)SosTableFind(bonds->pairs, PairKey(being, partner));
        if (record == NULL)
        {
            continue;
        }
        if (!activeOnly || record->endReason == SOS_BOND_END_NONE)
        {
            if (found < capacity)
            {
                out[found] = partner;
            }
            found += 1u;
        }
    }
    return found;
}

uint32_t SosBondsHistoryCount(const SosBonds *bonds, uint64_t being)
{
    if (bonds == NULL || being == 0u)
    {
        return 0u;
    }
    return SosLinksCount(bonds->links, being);
}

static uint64_t HashBond(SosTableKey key, const void *record, void *context)
{
    (void)context;
    const BondRecord *bond = record;
    uint64_t value = SosHashCombine(SosHash64(key.first), key.second);
    value = SosHashCombine(value, (uint64_t)bond->kind);
    value = SosHashCombine(value, (uint64_t)bond->endReason);
    value = SosHashCombine(value, bond->reunions);
    value = SosHashCombine(value, bond->formedTick);
    value = SosHashCombine(value, bond->endedTick);
    return SosHash64(value);
}

uint64_t SosBondsHash(const SosBonds *bonds)
{
    return bonds != NULL ? SosTableFold(bonds->pairs, HashBond, NULL) : 0u;
}
