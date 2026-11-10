#include "sim/lineage.h"

#include "sim/random.h"
#include "sim/table.h"

#include <stddef.h>
#include <stdlib.h>

/* The most ancestors one being can have within SOS_LINEAGE_MAX_DEPTH
 * generations, if they were all distinct: 2^(depth + 1) - 1. A fixed array of
 * this size keeps a kinship query allocation-free. */
#define ANCESTOR_CAPACITY ((UINT32_C(1) << (SOS_LINEAGE_MAX_DEPTH + 1u)) - 1u)

/* Space for the record keys: every table key uses `first` as the serial and
 * leaves the second key unused. */
#define RECORD_KEY(serial) ((SosTableKey){.first = (serial), .second = 0u})

typedef struct Ancestor
{
    uint64_t serial;
    uint32_t depth;
} Ancestor;

/* A common ancestor of the queried pair, with the distance from each. */
typedef struct CommonAncestor
{
    uint64_t serial;
    uint32_t depthA;
    uint32_t depthB;
} CommonAncestor;

typedef enum SiblingKind
{
    SIBLING_NONE = 0,
    SIBLING_HALF,
    SIBLING_FULL,
} SiblingKind;

struct SosLineage
{
    SosTable *records;
};

SosLineage *SosLineageCreate(void)
{
    SosLineage *lineage = malloc(sizeof(*lineage));
    if (lineage == NULL)
    {
        return NULL;
    }
    lineage->records = SosTableCreate(sizeof(SosLineageRecord));
    if (lineage->records == NULL)
    {
        free(lineage);
        return NULL;
    }
    return lineage;
}

void SosLineageDestroy(SosLineage *lineage)
{
    if (lineage != NULL)
    {
        SosTableDestroy(lineage->records);
        free(lineage);
    }
}

static const SosLineageRecord *RecordOf(const SosLineage *lineage, uint64_t serial)
{
    if (serial == 0u)
    {
        return NULL;
    }
    return SosTableFind(lineage->records, RECORD_KEY(serial));
}

/* Whether `serial` is already in `ancestors`; serials are unique, so the first
 * sighting of a being is also its shortest path from the start. */
static bool AncestorSeen(const Ancestor *ancestors, uint32_t count, uint64_t serial)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (ancestors[index].serial == serial)
        {
            return true;
        }
    }
    return false;
}

/* The start being and its strict ancestors up to SOS_LINEAGE_MAX_DEPTH
 * generations up, each at its shortest distance; the start is at depth 0. */
static uint32_t CollectAncestors(const SosLineage *lineage, uint64_t serial, Ancestor *ancestors)
{
    uint32_t count = 0u;
    ancestors[count++] = (Ancestor){.serial = serial, .depth = 0u};
    for (uint32_t head = 0u; head < count; ++head)
    {
        const Ancestor current = ancestors[head];
        if (current.depth >= SOS_LINEAGE_MAX_DEPTH)
        {
            continue;
        }
        const SosLineageRecord *record = RecordOf(lineage, current.serial);
        if (record == NULL)
        {
            continue;
        }
        const uint64_t parents[2] = {record->mother, record->father};
        for (size_t side = 0u; side < 2u; ++side)
        {
            const uint64_t parent = parents[side];
            if (parent == 0u || AncestorSeen(ancestors, count, parent))
            {
                continue;
            }
            if (count == ANCESTOR_CAPACITY)
            {
                /* The set is full: every other ancestor is too far to matter
                 * for the bounded queries below. */
                return count;
            }
            ancestors[count++] = (Ancestor){.serial = parent, .depth = current.depth + 1u};
        }
    }
    return count;
}

/* The generation of a child of these parents: founders are 0, everyone else
 * one past the highest recorded parent, an unrecorded parent counting as a
 * founder and the value saturating rather than wrapping. */
static uint32_t ChildGeneration(const SosLineage *lineage, uint64_t mother, uint64_t father)
{
    if (mother == 0u && father == 0u)
    {
        return 0u;
    }
    const uint64_t parents[2] = {mother, father};
    uint32_t highest = 0u;
    for (size_t side = 0u; side < 2u; ++side)
    {
        const SosLineageRecord *record =
            parents[side] == 0u ? NULL : RecordOf(lineage, parents[side]);
        if (record != NULL && record->generation > highest)
        {
            highest = record->generation;
        }
    }
    return highest == UINT32_MAX ? UINT32_MAX : highest + 1u;
}

/* Prepends `child` to `parent`'s child list, through the link for the side
 * that `parent` stands on. Missing records leave the list untouched. */
static void AttachChild(SosLineage *lineage, uint64_t parent, uint64_t child, bool ofMother)
{
    if (parent == 0u)
    {
        return;
    }
    SosLineageRecord *parentRecord = SosTableEdit(lineage->records, RECORD_KEY(parent));
    SosLineageRecord *childRecord = SosTableEdit(lineage->records, RECORD_KEY(child));
    if (parentRecord == NULL || childRecord == NULL)
    {
        return;
    }
    if (ofMother)
    {
        childRecord->nextSiblingOfMother = parentRecord->firstChild;
    }
    else
    {
        childRecord->nextSiblingOfFather = parentRecord->firstChild;
    }
    parentRecord->firstChild = child;
}

bool SosLineageRecordBirth(SosLineage *lineage, uint64_t serial, uint64_t mother, uint64_t father,
                           uint64_t birthTick)
{
    if (lineage == NULL || serial == 0u || mother == serial || father == serial ||
        (mother != 0u && mother == father))
    {
        return false;
    }
    const uint32_t generation = ChildGeneration(lineage, mother, father);
    bool created = false;
    SosLineageRecord *record = SosTableInsert(lineage->records, RECORD_KEY(serial), &created);
    if (record == NULL || !created)
    {
        return false;
    }
    *record = (SosLineageRecord){
        .mother = mother,
        .father = father,
        .birthTick = birthTick,
        .generation = generation,
        .firstChild = 0u,
        .nextSiblingOfMother = 0u,
        .nextSiblingOfFather = 0u,
    };
    AttachChild(lineage, mother, serial, true);
    AttachChild(lineage, father, serial, false);
    return true;
}

bool SosLineageGet(const SosLineage *lineage, uint64_t serial, SosLineageRecord *outRecord)
{
    if (lineage == NULL || outRecord == NULL || serial == 0u)
    {
        return false;
    }
    const SosLineageRecord *record = RecordOf(lineage, serial);
    if (record == NULL)
    {
        return false;
    }
    *outRecord = *record;
    return true;
}

uint32_t SosLineageChildren(const SosLineage *lineage, uint64_t serial, uint64_t *out,
                            uint32_t capacity)
{
    if (lineage == NULL || serial == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    const SosLineageRecord *record = RecordOf(lineage, serial);
    if (record == NULL)
    {
        return 0u;
    }
    /* Every child is a distinct record, so a list longer than the table is a
     * cycle and stops rather than running forever. */
    const uint32_t limit = SosTableCount(lineage->records);
    uint32_t found = 0u;
    uint64_t child = record->firstChild;
    while (child != 0u && found < limit)
    {
        const SosLineageRecord *childRecord = RecordOf(lineage, child);
        if (childRecord == NULL)
        {
            break;
        }
        if (found < capacity)
        {
            out[found] = child;
        }
        ++found;
        if (childRecord->mother == serial)
        {
            child = childRecord->nextSiblingOfMother;
        }
        else if (childRecord->father == serial)
        {
            child = childRecord->nextSiblingOfFather;
        }
        else
        {
            child = 0u;
        }
    }
    return found;
}

bool SosLineageIsAncestor(const SosLineage *lineage, uint64_t ancestor, uint64_t descendant)
{
    if (lineage == NULL || ancestor == 0u || descendant == 0u || ancestor == descendant ||
        RecordOf(lineage, ancestor) == NULL || RecordOf(lineage, descendant) == NULL)
    {
        return false;
    }
    Ancestor upward[ANCESTOR_CAPACITY];
    const uint32_t count = CollectAncestors(lineage, descendant, upward);
    /* Index 0 is the descendant itself, which is no strict ancestor. */
    for (uint32_t index = 1u; index < count; ++index)
    {
        if (upward[index].serial == ancestor)
        {
            return true;
        }
    }
    return false;
}

SosQ16 SosLineageRelatedness(const SosLineage *lineage, uint64_t a, uint64_t b)
{
    if (lineage == NULL || a == 0u || b == 0u)
    {
        return 0;
    }
    if (a == b)
    {
        return SOS_Q16_ONE;
    }
    if (RecordOf(lineage, a) == NULL || RecordOf(lineage, b) == NULL)
    {
        return 0;
    }
    Ancestor fromA[ANCESTOR_CAPACITY];
    Ancestor fromB[ANCESTOR_CAPACITY];
    const uint32_t countA = CollectAncestors(lineage, a, fromA);
    const uint32_t countB = CollectAncestors(lineage, b, fromB);
    CommonAncestor shared[ANCESTOR_CAPACITY];
    uint32_t sharedCount = 0u;
    for (uint32_t indexA = 0u; indexA < countA; ++indexA)
    {
        for (uint32_t indexB = 0u; indexB < countB; ++indexB)
        {
            if (fromA[indexA].serial == fromB[indexB].serial)
            {
                shared[sharedCount] = (CommonAncestor){.serial = fromA[indexA].serial,
                                                       .depthA = fromA[indexA].depth,
                                                       .depthB = fromB[indexB].depth};
                ++sharedCount;
            }
        }
    }
    /* Only a most recent common ancestor contributes: one that is itself an
     * ancestor of another shared ancestor would repeat a path the nearer one
     * already counts, which is what keeps half siblings at a quarter rather
     * than adding their grandparents on top. */
    bool dominated[ANCESTOR_CAPACITY] = {false};
    for (uint32_t index = 0u; index < sharedCount; ++index)
    {
        Ancestor upward[ANCESTOR_CAPACITY];
        const uint32_t count = CollectAncestors(lineage, shared[index].serial, upward);
        for (uint32_t up = 1u; up < count; ++up)
        {
            for (uint32_t other = 0u; other < sharedCount; ++other)
            {
                if (shared[other].serial == upward[up].serial)
                {
                    dominated[other] = true;
                }
            }
        }
    }
    int64_t total = 0;
    for (uint32_t index = 0u; index < sharedCount; ++index)
    {
        if (!dominated[index])
        {
            total += SOS_Q16_ONE >> (shared[index].depthA + shared[index].depthB);
        }
    }
    return (SosQ16)(total > SOS_Q16_ONE ? SOS_Q16_ONE : total);
}

/* Whether `parent` gave birth to, or fathered, `child`. */
static bool IsParent(const SosLineage *lineage, uint64_t parent, uint64_t child)
{
    if (parent == 0u)
    {
        return false;
    }
    const SosLineageRecord *record = RecordOf(lineage, child);
    return record != NULL && (record->mother == parent || record->father == parent);
}

/* How two beings share parents; an unknown parent is never shared, so two
 * founders are not siblings. */
static SiblingKind Siblings(const SosLineage *lineage, uint64_t a, uint64_t b)
{
    if (a == b)
    {
        return SIBLING_NONE;
    }
    const SosLineageRecord *recordA = RecordOf(lineage, a);
    const SosLineageRecord *recordB = RecordOf(lineage, b);
    if (recordA == NULL || recordB == NULL)
    {
        return SIBLING_NONE;
    }
    const bool sameMother = recordA->mother != 0u && recordA->mother == recordB->mother;
    const bool sameFather = recordA->father != 0u && recordA->father == recordB->father;
    if (sameMother && sameFather)
    {
        return SIBLING_FULL;
    }
    return sameMother || sameFather ? SIBLING_HALF : SIBLING_NONE;
}

/* Whether `grandparent` is a parent of one of `child`'s parents. */
static bool IsGrandparent(const SosLineage *lineage, uint64_t grandparent, uint64_t child)
{
    const SosLineageRecord *record = RecordOf(lineage, child);
    return record != NULL && (IsParent(lineage, grandparent, record->mother) ||
                              IsParent(lineage, grandparent, record->father));
}

/* Whether `relative` is a sibling of one of `person`'s parents. */
static bool IsSiblingOfParent(const SosLineage *lineage, uint64_t relative, uint64_t person)
{
    const SosLineageRecord *record = RecordOf(lineage, person);
    if (record == NULL)
    {
        return false;
    }
    return (record->mother != 0u && Siblings(lineage, relative, record->mother) != SIBLING_NONE) ||
           (record->father != 0u && Siblings(lineage, relative, record->father) != SIBLING_NONE);
}

/* Whether a parent of each being are siblings, which makes the beings first
 * cousins. Shared parents make siblings instead and are not cousins. */
static bool IsFirstCousin(const SosLineage *lineage, uint64_t a, uint64_t b)
{
    const SosLineageRecord *recordA = RecordOf(lineage, a);
    const SosLineageRecord *recordB = RecordOf(lineage, b);
    if (recordA == NULL || recordB == NULL)
    {
        return false;
    }
    const uint64_t parentsA[2] = {recordA->mother, recordA->father};
    const uint64_t parentsB[2] = {recordB->mother, recordB->father};
    for (size_t sideA = 0u; sideA < 2u; ++sideA)
    {
        for (size_t sideB = 0u; sideB < 2u; ++sideB)
        {
            if (parentsA[sideA] != 0u && parentsB[sideB] != 0u &&
                Siblings(lineage, parentsA[sideA], parentsB[sideB]) != SIBLING_NONE)
            {
                return true;
            }
        }
    }
    return false;
}

SosLineageKin SosLineageKinOf(const SosLineage *lineage, uint64_t a, uint64_t b)
{
    if (lineage == NULL || a == 0u || b == 0u)
    {
        return SOS_LINEAGE_KIN_UNRELATED;
    }
    if (a == b)
    {
        return SOS_LINEAGE_KIN_SELF;
    }
    if (RecordOf(lineage, a) == NULL || RecordOf(lineage, b) == NULL)
    {
        return SOS_LINEAGE_KIN_UNRELATED;
    }
    if (IsParent(lineage, b, a))
    {
        return SOS_LINEAGE_KIN_PARENT;
    }
    if (IsParent(lineage, a, b))
    {
        return SOS_LINEAGE_KIN_CHILD;
    }
    if (IsGrandparent(lineage, b, a))
    {
        return SOS_LINEAGE_KIN_GRANDPARENT;
    }
    if (IsGrandparent(lineage, a, b))
    {
        return SOS_LINEAGE_KIN_GRANDCHILD;
    }
    const SiblingKind siblings = Siblings(lineage, a, b);
    if (siblings == SIBLING_FULL)
    {
        return SOS_LINEAGE_KIN_FULL_SIBLING;
    }
    if (siblings == SIBLING_HALF)
    {
        return SOS_LINEAGE_KIN_HALF_SIBLING;
    }
    if (IsSiblingOfParent(lineage, b, a))
    {
        return SOS_LINEAGE_KIN_AUNT_UNCLE;
    }
    if (IsSiblingOfParent(lineage, a, b))
    {
        return SOS_LINEAGE_KIN_NIECE_NEPHEW;
    }
    if (IsFirstCousin(lineage, a, b))
    {
        return SOS_LINEAGE_KIN_FIRST_COUSIN;
    }
    return SosLineageRelatedness(lineage, a, b) > 0 ? SOS_LINEAGE_KIN_OTHER_RELATIVE
                                                    : SOS_LINEAGE_KIN_UNRELATED;
}

static uint64_t HashLineageRecord(SosTableKey key, const void *record, void *context)
{
    (void)context;
    const SosLineageRecord *entry = record;
    uint64_t hash = SosHashCombine(SosHash64(key.first), entry->mother);
    hash = SosHashCombine(hash, entry->father);
    hash = SosHashCombine(hash, entry->birthTick);
    hash = SosHashCombine(hash, entry->generation);
    return SosHash64(hash);
}

uint64_t SosLineageHash(const SosLineage *lineage)
{
    return lineage != NULL ? SosTableFold(lineage->records, HashLineageRecord, NULL) : 0u;
}
