#include "sim/groups.h"

#include "sim/links.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>

/* What is known about one group. `memberCount` is kept here so that callers
 * and the hash never have to walk the member list to learn the size. */
typedef struct GroupRecord
{
    SosGroupKind kind;
    uint64_t parent;
    uint64_t leader;
    uint64_t foundedTick;
    uint32_t memberCount;
} GroupRecord;

/* One being's place in one group. The two lists that thread it into the group's
 * members and the being's groups live in the shared SosLinks sets. */
typedef struct Membership
{
    uint64_t joinedTick;
    SosGroupRole role;
} Membership;

struct SosGroups
{
    /* Keyed (group, 0). */
    SosTable *groups;
    /* Keyed (group, being): the payload of a membership. */
    SosTable *members;
    /* (group, being) pairs: a group's members in joining order. */
    SosLinks *memberLinks;
    /* (being, group) pairs: a being's groups in joining order. */
    SosLinks *beingLinks;
    /* The next id to hand out; 0 when the counter is exhausted. */
    uint64_t nextId;
};

static SosTableKey GroupKey(uint64_t group)
{
    return (SosTableKey){.first = group, .second = 0u};
}

static SosTableKey MemberKey(uint64_t group, uint64_t being)
{
    return (SosTableKey){.first = group, .second = being};
}

static GroupRecord *EditGroup(SosGroups *groups, uint64_t group)
{
    return (GroupRecord *)SosTableEdit(groups->groups, GroupKey(group));
}

static const GroupRecord *FindGroup(const SosGroups *groups, uint64_t group)
{
    return (const GroupRecord *)SosTableFind(groups->groups, GroupKey(group));
}

static Membership *EditMembership(SosGroups *groups, uint64_t group, uint64_t being)
{
    return (Membership *)SosTableEdit(groups->members, MemberKey(group, being));
}

static const Membership *FindMembership(const SosGroups *groups, uint64_t group, uint64_t being)
{
    return (const Membership *)SosTableFind(groups->members, MemberKey(group, being));
}

/* Records that `being` has joined `group` at `joinedTick`, adding it to both
 * lists. False, leaving everything unchanged, when the being is already a
 * member or memory ran out. The caller adjusts the leader. */
static bool InsertMember(SosGroups *groups, uint64_t group, uint64_t being, uint64_t joinedTick,
                         SosGroupRole role)
{
    GroupRecord *record = EditGroup(groups, group);
    if (record == NULL)
    {
        return false;
    }
    bool created = false;
    Membership *membership =
        (Membership *)SosTableInsert(groups->members, MemberKey(group, being), &created);
    if (membership == NULL || !created)
    {
        return false;
    }
    if (!SosLinksAdd(groups->memberLinks, group, being))
    {
        (void)SosTableRemove(groups->members, MemberKey(group, being));
        return false;
    }
    if (!SosLinksAdd(groups->beingLinks, being, group))
    {
        (void)SosLinksRemove(groups->memberLinks, group, being);
        (void)SosTableRemove(groups->members, MemberKey(group, being));
        return false;
    }
    *membership = (Membership){.joinedTick = joinedTick, .role = role};
    record->memberCount += 1u;
    return true;
}

/* Removes the membership of a being that is known to belong, unlinking it from
 * both lists and dropping the payload. */
static void EraseMember(SosGroups *groups, uint64_t group, uint64_t being)
{
    GroupRecord *record = EditGroup(groups, group);
    const Membership *membership = FindMembership(groups, group, being);
    if (record == NULL || membership == NULL)
    {
        return;
    }
    (void)SosLinksRemove(groups->memberLinks, group, being);
    (void)SosLinksRemove(groups->beingLinks, being, group);
    (void)SosTableRemove(groups->members, MemberKey(group, being));
    if (record->memberCount > 0u)
    {
        record->memberCount -= 1u;
    }
}

/* Makes the member that joined earliest the leader, ties going to the lower
 * serial, and marks the roles. A group with no members loses its leader. */
static void ElectLeader(SosGroups *groups, uint64_t group)
{
    GroupRecord *record = EditGroup(groups, group);
    if (record == NULL)
    {
        return;
    }
    uint64_t best = 0u;
    uint64_t bestTick = 0u;
    for (uint64_t being = SosLinksFirst(groups->memberLinks, group); being != 0u;
         being = SosLinksNext(groups->memberLinks, group, being))
    {
        Membership *membership = EditMembership(groups, group, being);
        if (membership == NULL)
        {
            continue;
        }
        membership->role = SOS_GROUP_ROLE_MEMBER;
        if (best == 0u || membership->joinedTick < bestTick ||
            (membership->joinedTick == bestTick && being < best))
        {
            best = being;
            bestTick = membership->joinedTick;
        }
    }
    record->leader = best;
    if (best != 0u)
    {
        Membership *leader = EditMembership(groups, group, best);
        if (leader != NULL)
        {
            leader->role = SOS_GROUP_ROLE_LEADER;
        }
    }
}

/* Re-parents every direct child of `from` to `to`, before either is removed. */
static void ReparentChildren(SosGroups *groups, uint64_t from, uint64_t to)
{
    const uint32_t slots = SosTableSlotCount(groups->groups);
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        SosTableKey key;
        if (!SosTableSlot(groups->groups, slot, &key, NULL) || key.first == from)
        {
            continue;
        }
        GroupRecord *child = EditGroup(groups, key.first);
        if (child != NULL && child->parent == from)
        {
            child->parent = to;
        }
    }
}

/* Reserves everything a move of `moved` members into one group can allocate:
 * the membership records and the two link heads. False when memory ran out,
 * leaving the observable state unchanged. The caller has already checked the
 * target group exists, so no group record is needed here. */
static bool ReserveMembers(SosGroups *groups, uint32_t moved)
{
    const uint32_t memberCount = SosTableCount(groups->members);
    if (moved > SOS_TABLE_MAX_COUNT - memberCount ||
        !SosTableReserve(groups->members, memberCount + moved))
    {
        return false;
    }
    /* One owner head under the target group, one pair per member in each of
     * the two link sets, and one being head per member. */
    return SosLinksReserve(groups->memberLinks, 1u, moved) &&
           SosLinksReserve(groups->beingLinks, moved, moved);
}

/* Moves every member of `from` into `to` at `tick`, dropping a member already
 * in `to`. Both groups must exist and the caller has reserved the tables with
 * ReserveMembers, so a false InsertMember means the being was already in `to`,
 * never that memory ran out. Removing `from` and re-parenting its children are
 * left to the caller. */
static void MoveMembers(SosGroups *groups, uint64_t to, uint64_t from, uint64_t tick)
{
    for (uint64_t being = SosLinksFirst(groups->memberLinks, from); being != 0u;)
    {
        /* Removing the current member only relinks the others, so its successor
         * is still the one to visit next. */
        const uint64_t next = SosLinksNext(groups->memberLinks, from, being);
        EraseMember(groups, from, being);
        (void)InsertMember(groups, to, being, tick, SOS_GROUP_ROLE_MEMBER);
        being = next;
    }
}

SosGroups *SosGroupsCreate(void)
{
    SosGroups *groups = calloc(1u, sizeof(*groups));
    if (groups == NULL)
    {
        return NULL;
    }
    groups->groups = SosTableCreate(sizeof(GroupRecord));
    groups->members = SosTableCreate(sizeof(Membership));
    groups->memberLinks = SosLinksCreate();
    groups->beingLinks = SosLinksCreate();
    if (groups->groups == NULL || groups->members == NULL || groups->memberLinks == NULL ||
        groups->beingLinks == NULL)
    {
        SosGroupsDestroy(groups);
        return NULL;
    }
    /* Zero means nobody, so the first id is one. */
    groups->nextId = 1u;
    return groups;
}

void SosGroupsDestroy(SosGroups *groups)
{
    if (groups != NULL)
    {
        SosTableDestroy(groups->groups);
        SosTableDestroy(groups->members);
        SosLinksDestroy(groups->memberLinks);
        SosLinksDestroy(groups->beingLinks);
        free(groups);
    }
}

uint64_t SosGroupsFound(SosGroups *groups, SosGroupKind kind, uint64_t parent, uint64_t founder,
                        uint64_t tick)
{
    if (groups == NULL || kind == SOS_GROUP_NONE)
    {
        return 0u;
    }
    if (parent != 0u && FindGroup(groups, parent) == NULL)
    {
        return 0u;
    }
    if (groups->nextId == 0u)
    {
        return 0u;
    }
    const uint64_t id = groups->nextId;
    bool created = false;
    GroupRecord *record = (GroupRecord *)SosTableInsert(groups->groups, GroupKey(id), &created);
    if (record == NULL || !created)
    {
        return 0u;
    }
    groups->nextId = id + 1u;
    *record = (GroupRecord){.kind = kind, .parent = parent, .foundedTick = tick};
    if (founder != 0u)
    {
        if (!InsertMember(groups, id, founder, tick, SOS_GROUP_ROLE_LEADER))
        {
            (void)SosTableRemove(groups->groups, GroupKey(id));
            return 0u;
        }
        record = EditGroup(groups, id);
        if (record != NULL)
        {
            record->leader = founder;
        }
    }
    return id;
}

bool SosGroupsInfo(const SosGroups *groups, uint64_t group, SosGroupInfo *out)
{
    if (groups == NULL || group == 0u || out == NULL)
    {
        return false;
    }
    const GroupRecord *record = FindGroup(groups, group);
    if (record == NULL)
    {
        return false;
    }
    *out = (SosGroupInfo){
        .kind = record->kind,
        .parent = record->parent,
        .leader = record->leader,
        .foundedTick = record->foundedTick,
        .memberCount = record->memberCount,
    };
    return true;
}

bool SosGroupsJoin(SosGroups *groups, uint64_t group, uint64_t being, uint64_t tick)
{
    if (groups == NULL || group == 0u || being == 0u)
    {
        return false;
    }
    if (!InsertMember(groups, group, being, tick, SOS_GROUP_ROLE_MEMBER))
    {
        return false;
    }
    GroupRecord *record = EditGroup(groups, group);
    if (record != NULL && record->leader == 0u)
    {
        Membership *membership = EditMembership(groups, group, being);
        if (membership != NULL)
        {
            membership->role = SOS_GROUP_ROLE_LEADER;
            record->leader = being;
        }
    }
    return true;
}

bool SosGroupsLeave(SosGroups *groups, uint64_t group, uint64_t being, uint64_t tick)
{
    (void)tick;
    if (groups == NULL || group == 0u || being == 0u)
    {
        return false;
    }
    const GroupRecord *record = FindGroup(groups, group);
    if (record == NULL || FindMembership(groups, group, being) == NULL)
    {
        return false;
    }
    const bool wasLeader = record->leader == being;
    EraseMember(groups, group, being);
    if (wasLeader)
    {
        ElectLeader(groups, group);
    }
    return true;
}

uint32_t SosGroupsLeaveAll(SosGroups *groups, uint64_t being, uint64_t tick)
{
    (void)tick;
    if (groups == NULL || being == 0u)
    {
        return 0u;
    }
    uint32_t removed = 0u;
    /* Capture each next group before erasing, since EraseMember rewrites the
     * being's list. */
    for (uint64_t group = SosLinksFirst(groups->beingLinks, being); group != 0u;)
    {
        const uint64_t next = SosLinksNext(groups->beingLinks, being, group);
        const GroupRecord *record = FindGroup(groups, group);
        const bool wasLeader = record != NULL && record->leader == being;
        EraseMember(groups, group, being);
        if (wasLeader)
        {
            ElectLeader(groups, group);
        }
        removed += 1u;
        group = next;
    }
    return removed;
}

bool SosGroupsDissolve(SosGroups *groups, uint64_t group)
{
    if (groups == NULL || group == 0u)
    {
        return false;
    }
    const GroupRecord *record = FindGroup(groups, group);
    if (record == NULL)
    {
        return false;
    }
    const uint64_t parent = record->parent;
    /* Capture the list before releasing it, since erasing relinks the rest. */
    for (uint64_t being = SosLinksFirst(groups->memberLinks, group); being != 0u;)
    {
        const uint64_t next = SosLinksNext(groups->memberLinks, group, being);
        EraseMember(groups, group, being);
        being = next;
    }
    ReparentChildren(groups, group, parent);
    (void)SosTableRemove(groups->groups, GroupKey(group));
    return true;
}

bool SosGroupsMerge(SosGroups *groups, uint64_t into, uint64_t from, uint64_t tick)
{
    if (groups == NULL || into == 0u || from == 0u || into == from)
    {
        return false;
    }
    const GroupRecord *source = FindGroup(groups, from);
    if (FindGroup(groups, into) == NULL || source == NULL)
    {
        return false;
    }
    /* Folding a group into its own descendant would make a cycle in the
     * parent chain once the children are re-parented. */
    if (SosGroupsIsWithin(groups, into, from))
    {
        return false;
    }
    /* Reserve before touching anything, so a failure leaves both groups
     * exactly as they were rather than half moved. */
    if (!ReserveMembers(groups, source->memberCount))
    {
        return false;
    }
    MoveMembers(groups, into, from, tick);
    ReparentChildren(groups, from, into);
    const GroupRecord *target = FindGroup(groups, into);
    if (target != NULL && target->leader == 0u)
    {
        ElectLeader(groups, into);
    }
    (void)SosTableRemove(groups->groups, GroupKey(from));
    return true;
}

uint64_t SosGroupsSplit(SosGroups *groups, uint64_t group, const uint64_t *beings, uint32_t count,
                        uint64_t tick)
{
    if (groups == NULL || group == 0u || beings == NULL || count == 0u)
    {
        return 0u;
    }
    const GroupRecord *source = FindGroup(groups, group);
    if (source == NULL || groups->nextId == 0u)
    {
        return 0u;
    }
    /* Reserve the new group record and everything the move can allocate before
     * founding the half, so a failure returns 0 with nothing changed rather
     * than a half-populated group nobody asked for. */
    const uint32_t groupCount = SosTableCount(groups->groups);
    if (groupCount >= SOS_TABLE_MAX_COUNT || !SosTableReserve(groups->groups, groupCount + 1u) ||
        !ReserveMembers(groups, count))
    {
        return 0u;
    }
    const SosGroupKind kind = source->kind;
    const uint64_t parent = source->parent;
    const uint64_t id = groups->nextId;
    bool created = false;
    GroupRecord *record = (GroupRecord *)SosTableInsert(groups->groups, GroupKey(id), &created);
    if (record == NULL || !created)
    {
        return 0u;
    }
    groups->nextId = id + 1u;
    *record = (GroupRecord){.kind = kind, .parent = parent, .foundedTick = tick};
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint64_t being = beings[index];
        const Membership *membership = being != 0u ? FindMembership(groups, group, being) : NULL;
        if (membership == NULL)
        {
            continue;
        }
        const uint64_t joinedTick = membership->joinedTick;
        EraseMember(groups, group, being);
        (void)InsertMember(groups, id, being, joinedTick, SOS_GROUP_ROLE_MEMBER);
    }
    const uint64_t leader = SosGroupsLeader(groups, group);
    if (leader != 0u && FindMembership(groups, group, leader) == NULL)
    {
        ElectLeader(groups, group);
    }
    ElectLeader(groups, id);
    return id;
}

uint64_t SosGroupsLeader(const SosGroups *groups, uint64_t group)
{
    if (groups == NULL || group == 0u)
    {
        return 0u;
    }
    const GroupRecord *record = FindGroup(groups, group);
    return record != NULL ? record->leader : 0u;
}

bool SosGroupsSetLeader(SosGroups *groups, uint64_t group, uint64_t being)
{
    if (groups == NULL || group == 0u || being == 0u)
    {
        return false;
    }
    GroupRecord *record = EditGroup(groups, group);
    Membership *membership = EditMembership(groups, group, being);
    if (record == NULL || membership == NULL)
    {
        return false;
    }
    if (record->leader == being)
    {
        return true;
    }
    if (record->leader != 0u)
    {
        Membership *previous = EditMembership(groups, group, record->leader);
        if (previous != NULL)
        {
            previous->role = SOS_GROUP_ROLE_MEMBER;
        }
    }
    membership->role = SOS_GROUP_ROLE_LEADER;
    record->leader = being;
    return true;
}

uint32_t SosGroupsMembers(const SosGroups *groups, uint64_t group, uint64_t *out, uint32_t capacity)
{
    if (groups == NULL || group == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t found = 0u;
    for (uint64_t being = SosLinksFirst(groups->memberLinks, group); being != 0u;
         being = SosLinksNext(groups->memberLinks, group, being))
    {
        if (found < capacity)
        {
            out[found] = being;
        }
        found += 1u;
    }
    return found;
}

uint32_t SosGroupsOf(const SosGroups *groups, uint64_t being, uint32_t kindFilter, uint64_t *out,
                     uint32_t capacity)
{
    if (groups == NULL || being == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t found = 0u;
    for (uint64_t group = SosLinksFirst(groups->beingLinks, being); group != 0u;
         group = SosLinksNext(groups->beingLinks, being, group))
    {
        const GroupRecord *record = FindGroup(groups, group);
        if (record == NULL)
        {
            continue;
        }
        if (kindFilter == 0u || record->kind == kindFilter)
        {
            if (found < capacity)
            {
                out[found] = group;
            }
            found += 1u;
        }
    }
    return found;
}

bool SosGroupsIsWithin(const SosGroups *groups, uint64_t inner, uint64_t outer)
{
    if (groups == NULL || inner == 0u || outer == 0u)
    {
        return false;
    }
    if (FindGroup(groups, inner) == NULL || FindGroup(groups, outer) == NULL)
    {
        return false;
    }
    /* The chain cannot be longer than the number of groups; the bound also
     * stops a corrupt parent cycle from spinning forever. */
    const uint32_t limit = SosTableCount(groups->groups);
    uint64_t group = inner;
    for (uint32_t step = 0u; group != 0u && step <= limit; ++step)
    {
        if (group == outer)
        {
            return true;
        }
        const GroupRecord *record = FindGroup(groups, group);
        if (record == NULL)
        {
            return false;
        }
        group = record->parent;
    }
    return false;
}

static uint64_t HashGroup(SosTableKey key, const void *record, void *context)
{
    (void)context;
    const GroupRecord *group = record;
    uint64_t value = SosHashCombine(key.first, group->kind);
    value = SosHashCombine(value, group->parent);
    value = SosHashCombine(value, group->leader);
    value = SosHashCombine(value, group->foundedTick);
    value = SosHashCombine(value, group->memberCount);
    return SosHash64(value);
}

static uint64_t HashMembership(SosTableKey key, const void *record, void *context)
{
    (void)context;
    const Membership *membership = record;
    uint64_t value = SosHashCombine(key.first, key.second);
    value = SosHashCombine(value, membership->joinedTick);
    value = SosHashCombine(value, membership->role);
    return SosHash64(value);
}

uint64_t SosGroupsHash(const SosGroups *groups)
{
    if (groups == NULL)
    {
        return 0u;
    }
    const uint64_t sum = SosTableFold(groups->groups, HashGroup, NULL) +
                         SosTableFold(groups->members, HashMembership, NULL);
    return SosHashCombine(groups->nextId, sum);
}
