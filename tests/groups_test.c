#include "sim/groups.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "groups test failed: %s\n", message);
        exit(1);
    }
}

static void TestLifecycle(void)
{
    SosGroups *groups = SosGroupsCreate();
    Expect(groups != NULL, "groups are created");
    Expect(SosGroupsFound(groups, SOS_GROUP_NONE, 0u, 1u, 0u) == 0u, "a kind is required");

    const uint64_t house = SosGroupsFound(groups, SOS_GROUP_HOUSEHOLD, 0u, 1u, 100u);
    Expect(house != 0u, "a household is founded");
    Expect(SosGroupsLeader(groups, house) == 1u, "the founder leads");
    Expect(SosGroupsJoin(groups, house, 1u, 150u) == false, "a founder does not join twice");
    Expect(SosGroupsJoin(groups, house, 2u, 200u), "a second member joins");
    Expect(SosGroupsJoin(groups, house, 3u, 300u), "a third member joins");
    Expect(SosGroupsLeader(groups, house) == 1u, "joining does not unseat the leader");

    uint64_t members[8];
    Expect(SosGroupsMembers(groups, house, members, 8u) == 3u, "three members belong");
    Expect(SosGroupsMembers(groups, house, members, 2u) == 3u,
           "capacity bounds the writing, not the count");
    Expect(members[0] == 1u && members[1] == 2u, "members come back in joining order");
    Expect(SosGroupsMembers(groups, house, NULL, 0u) == 3u, "a NULL list with no room only counts");

    uint64_t of[4];
    Expect(SosGroupsOf(groups, 2u, 0u, of, 4u) == 1u && of[0] == house, "a being knows its group");
    Expect(SosGroupsOf(groups, 3u, SOS_GROUP_HOUSEHOLD, of, 4u) == 1u, "a kind filter keeps it");
    Expect(SosGroupsOf(groups, 3u, SOS_GROUP_TRIBE, of, 4u) == 0u, "a kind filter drops it");

    Expect(SosGroupsLeave(groups, house, 3u, 400u), "a member leaves");
    Expect(SosGroupsLeave(groups, house, 3u, 400u) == false, "leaving twice fails");
    Expect(SosGroupsMembers(groups, house, members, 8u) == 2u, "two members remain");
    Expect(SosGroupsOf(groups, 3u, 0u, of, 4u) == 0u, "the left being has no groups");
    Expect(SosGroupsJoin(groups, house, 3u, 500u), "a being may rejoin");

    /* An empty group stays alive, without a leader, and gains one on joining. */
    const uint64_t empty = SosGroupsFound(groups, SOS_GROUP_BAND, 0u, 0u, 0u);
    Expect(empty != 0u, "a group may be founded with no members");
    Expect(SosGroupsLeader(groups, empty) == 0u, "an empty group has no leader");
    Expect(SosGroupsJoin(groups, empty, 9u, 10u), "a member joins an empty group");
    Expect(SosGroupsLeader(groups, empty) == 9u, "the first joiner leads an empty group");
    Expect(SosGroupsLeave(groups, empty, 9u, 20u), "the only member leaves");
    Expect(SosGroupsLeader(groups, empty) == 0u, "an emptied group loses its leader");

    SosGroupsDestroy(groups);
}

static void TestInfo(void)
{
    SosGroups *groups = SosGroupsCreate();
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 10u, 100u);
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 20u, 200u);
    Expect(SosGroupsJoin(groups, band, 30u, 300u), "info: a member joins");

    SosGroupInfo info;
    Expect(SosGroupsInfo(groups, tribe, &info), "a group is read");
    Expect(info.kind == SOS_GROUP_TRIBE, "the kind is read back");
    Expect(info.parent == 0u, "a root has no parent");
    Expect(info.leader == 10u, "the founder is the leader");
    Expect(info.foundedTick == 100u, "the founding tick is stored");
    Expect(info.memberCount == 1u, "the founder is counted");

    Expect(SosGroupsInfo(groups, band, &info), "a nested group is read");
    Expect(info.kind == SOS_GROUP_BAND && info.parent == tribe, "the parent is read back");
    Expect(info.foundedTick == 200u, "a nested group records its own founding tick");
    Expect(info.leader == 20u, "the nested founder leads");
    Expect(info.memberCount == 2u, "joins increase the count");

    Expect(SosGroupsLeave(groups, band, 20u, 400u), "the leader leaves");
    Expect(SosGroupsInfo(groups, band, &info), "the group is read after a departure");
    Expect(info.leader == 30u, "the successor is visible through info");
    Expect(info.memberCount == 1u, "the count drops");

    /* A split-off group records the tick it split at. */
    const uint64_t mover[1] = {30u};
    const uint64_t half = SosGroupsSplit(groups, band, mover, 1u, 500u);
    Expect(half != 0u, "info: a half is split off");
    Expect(SosGroupsInfo(groups, half, &info), "the half is read");
    Expect(info.kind == SOS_GROUP_BAND && info.parent == tribe, "the half keeps kind and parent");
    Expect(info.foundedTick == 500u, "the half records the split tick");
    SosGroupsDestroy(groups);
}

static void TestNesting(void)
{
    SosGroups *groups = SosGroupsCreate();
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 10u, 0u);
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 20u, 0u);
    const uint64_t clan = SosGroupsFound(groups, SOS_GROUP_CLAN, band, 30u, 0u);
    Expect(tribe != 0u && band != 0u && clan != 0u, "a hierarchy is founded");
    Expect(SosGroupsIsWithin(groups, band, tribe), "a band is within its tribe");
    Expect(SosGroupsIsWithin(groups, clan, tribe), "a clan is within its tribe");
    Expect(SosGroupsIsWithin(groups, tribe, tribe), "a group is within itself");
    Expect(SosGroupsIsWithin(groups, tribe, band) == false, "a tribe is not within its band");
    Expect(SosGroupsIsWithin(groups, band, 0u) == false, "nothing is within nobody");
    Expect(SosGroupsFound(groups, SOS_GROUP_BAND, 999u, 1u, 0u) == 0u,
           "founding under an unknown parent fails");

    /* Dissolving the band leaves its clan with the tribe and releases the
     * band's members. */
    Expect(SosGroupsJoin(groups, band, 5u, 0u), "the band has a member");
    Expect(SosGroupsDissolve(groups, band), "the band dissolves");
    Expect(SosGroupsDissolve(groups, band) == false, "dissolving twice fails");
    Expect(SosGroupsIsWithin(groups, clan, tribe), "the orphaned clan is re-parented");
    Expect(SosGroupsLeader(groups, band) == 0u, "a dissolved group has no leader");
    Expect(SosGroupsOf(groups, 5u, 0u, NULL, 0u) == 0u, "an orphan has no groups");
    SosGroupsDestroy(groups);
}

static void TestSuccession(void)
{
    SosGroups *groups = SosGroupsCreate();
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, 0u, 5u, 100u);
    Expect(SosGroupsJoin(groups, band, 9u, 150u), "the earliest joiner joins");
    Expect(SosGroupsJoin(groups, band, 7u, 200u), "the first of a tie joins");
    Expect(SosGroupsJoin(groups, band, 3u, 200u), "the second of a tie joins");
    Expect(SosGroupsLeave(groups, band, 5u, 300u), "the founder leaves");
    Expect(SosGroupsLeader(groups, band) == 9u, "the longest-standing member succeeds");
    Expect(SosGroupsLeave(groups, band, 9u, 400u), "the leader leaves");
    Expect(SosGroupsLeader(groups, band) == 3u, "a tie goes to the lower serial");

    Expect(SosGroupsSetLeader(groups, band, 7u), "leadership is set directly");
    Expect(SosGroupsLeader(groups, band) == 7u, "the new leader is kept");
    Expect(SosGroupsSetLeader(groups, band, 7u), "setting the same leader succeeds");
    Expect(SosGroupsSetLeader(groups, band, 42u) == false, "a non-member cannot lead");
    Expect(SosGroupsSetLeader(groups, band, 0u) == false, "nobody cannot lead");
    Expect(SosGroupsLeave(groups, band, 7u, 500u), "the appointed leader leaves");
    Expect(SosGroupsLeader(groups, band) == 3u, "the remaining member succeeds");
    SosGroupsDestroy(groups);
}

static void TestLeaveAll(void)
{
    SosGroups *groups = SosGroupsCreate();
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 1u, 0u);
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 2u, 0u);
    const uint64_t guild = SosGroupsFound(groups, SOS_GROUP_GUILD, 0u, 2u, 0u);
    Expect(SosGroupsJoin(groups, band, 3u, 100u), "leave-all: a being joins the band");
    Expect(SosGroupsJoin(groups, guild, 4u, 50u), "leave-all: a guild member joins");

    /* Being 2 leads both the band and the guild; its death hands each to the
     * longest-standing survivor. */
    Expect(SosGroupsLeaveAll(groups, 2u, 500u) == 2u, "a death removes every membership");
    Expect(SosGroupsLeader(groups, band) == 3u, "the band elects a successor");
    Expect(SosGroupsLeader(groups, guild) == 4u, "the guild elects a successor");
    Expect(SosGroupsOf(groups, 2u, 0u, NULL, 0u) == 0u, "the dead being has no groups");
    Expect(SosGroupsMembers(groups, band, NULL, 0u) == 1u, "the band keeps its other member");
    Expect(SosGroupsMembers(groups, guild, NULL, 0u) == 1u, "the guild keeps its other member");

    Expect(SosGroupsLeaveAll(groups, 2u, 600u) == 0u, "leaving all again removes nothing");
    Expect(SosGroupsLeaveAll(groups, 999u, 600u) == 0u, "an unknown being removes nothing");
    Expect(SosGroupsLeaveAll(NULL, 1u, 600u) == 0u, "NULL cannot leave all");
    Expect(SosGroupsLeaveAll(groups, 0u, 600u) == 0u, "nobody cannot leave all");
    SosGroupsDestroy(groups);
}

static void TestMergeAndSplit(void)
{
    SosGroups *groups = SosGroupsCreate();
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 1u, 0u);
    const uint64_t left = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 10u, 0u);
    const uint64_t right = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 0u, 0u);
    Expect(SosGroupsJoin(groups, left, 2u, 10u), "left has a member");
    Expect(SosGroupsJoin(groups, left, 3u, 20u), "left has another");
    Expect(SosGroupsJoin(groups, right, 4u, 5u), "right has a member");
    Expect(SosGroupsJoin(groups, right, 2u, 30u), "a being may belong to both bands");

    Expect(SosGroupsMerge(groups, left, right, 100u), "right merges into left");
    Expect(SosGroupsLeader(groups, right) == 0u, "the absorbed group is gone");
    uint64_t members[8];
    Expect(SosGroupsMembers(groups, left, members, 8u) == 4u, "the members are combined once");
    Expect(SosGroupsOf(groups, 4u, 0u, members, 8u) == 1u,
           "an absorbed member follows the survivor");
    Expect(SosGroupsIsWithin(groups, left, tribe), "the survivor keeps its parent");
    Expect(SosGroupsMerge(groups, left, right, 100u) == false, "merging a missing group fails");
    Expect(SosGroupsMerge(groups, left, left, 100u) == false, "a group cannot merge into itself");
    Expect(SosGroupsMerge(groups, left, tribe, 100u) == false,
           "a merge that would make a parent cycle fails");

    /* Splitting founds a sibling of the same kind and parent. */
    Expect(SosGroupsJoin(groups, left, 5u, 40u), "a fifth member joins");
    const uint64_t moving[2] = {2u, 5u};
    const uint64_t half = SosGroupsSplit(groups, left, moving, 2u, 200u);
    Expect(half != 0u && half != left, "a split founds a new group");
    Expect(SosGroupsIsWithin(groups, half, tribe), "the new group keeps the parent");
    Expect(SosGroupsLeader(groups, half) == 2u, "the longest-standing moved member leads");
    Expect(SosGroupsMembers(groups, left, members, 8u) == 3u, "the rest stay behind");
    Expect(SosGroupsMembers(groups, half, members, 8u) == 2u, "the named members move");
    Expect(SosGroupsOf(groups, 5u, 0u, members, 8u) == 1u && members[0] == half,
           "a moved member now belongs to the new group");
    Expect(SosGroupsSplit(groups, half, NULL, 1u, 0u) == 0u, "a split needs a list");
    Expect(SosGroupsSplit(groups, half, moving, 0u, 0u) == 0u, "a split needs members");
    SosGroupsDestroy(groups);
}

static void TestInvalidIds(void)
{
    SosGroups *groups = SosGroupsCreate();
    Expect(groups != NULL, "groups are created");
    uint64_t out[2];
    Expect(SosGroupsFound(NULL, SOS_GROUP_BAND, 0u, 1u, 0u) == 0u, "NULL cannot found");
    Expect(SosGroupsJoin(NULL, 1u, 1u, 0u) == false, "NULL cannot join");
    Expect(SosGroupsLeave(NULL, 1u, 1u, 0u) == false, "NULL cannot leave");
    Expect(SosGroupsDissolve(NULL, 1u) == false, "NULL cannot dissolve");
    Expect(SosGroupsMerge(NULL, 1u, 2u, 0u) == false, "NULL cannot merge");
    Expect(SosGroupsSplit(NULL, 1u, out, 1u, 0u) == 0u, "NULL cannot split");
    Expect(SosGroupsLeader(NULL, 1u) == 0u, "NULL has no leaders");
    Expect(SosGroupsSetLeader(NULL, 1u, 1u) == false, "NULL cannot appoint");
    Expect(SosGroupsMembers(NULL, 1u, out, 2u) == 0u, "NULL has no members");
    Expect(SosGroupsOf(NULL, 1u, 0u, out, 2u) == 0u, "NULL has no groups");
    Expect(SosGroupsIsWithin(NULL, 1u, 1u) == false, "NULL nests nothing");
    Expect(SosGroupsHash(NULL) == 0u, "NULL hashes zero");

    Expect(SosGroupsJoin(groups, 0u, 1u, 0u) == false, "group zero cannot be joined");
    Expect(SosGroupsJoin(groups, 1u, 0u, 0u) == false, "nobody cannot join");
    Expect(SosGroupsLeader(groups, 0u) == 0u, "group zero has no leader");
    Expect(SosGroupsMembers(groups, 0u, out, 2u) == 0u, "group zero has no members");
    Expect(SosGroupsOf(groups, 0u, 0u, out, 2u) == 0u, "nobody has no groups");
    Expect(SosGroupsLeave(groups, 0u, 1u, 0u) == false, "group zero cannot be left");
    Expect(SosGroupsDissolve(groups, 0u) == false, "group zero cannot dissolve");
    Expect(SosGroupsMerge(groups, 0u, 0u, 0u) == false, "group zero cannot merge");

    /* Unknown but nonzero ids fail the same way. */
    Expect(SosGroupsJoin(groups, 77u, 1u, 0u) == false, "an unknown group cannot be joined");
    Expect(SosGroupsLeave(groups, 77u, 1u, 0u) == false, "an unknown group cannot be left");
    Expect(SosGroupsDissolve(groups, 77u) == false, "an unknown group cannot dissolve");
    Expect(SosGroupsSetLeader(groups, 77u, 1u) == false, "an unknown group has no leader to set");
    Expect(SosGroupsMembers(groups, 77u, out, 2u) == 0u, "an unknown group has no members");
    Expect(SosGroupsIsWithin(groups, 77u, 88u) == false, "unknown groups are unrelated");
    Expect(SosGroupsMembers(groups, 1u, NULL, 2u) == 0u, "a NULL list with room is rejected");
    Expect(SosGroupsOf(groups, 1u, 0u, NULL, 2u) == 0u, "a NULL group list is rejected");

    SosGroupInfo info;
    Expect(SosGroupsInfo(NULL, 1u, &info) == false, "NULL has no info");
    Expect(SosGroupsInfo(groups, 0u, &info) == false, "group zero has no info");
    Expect(SosGroupsInfo(groups, 77u, &info) == false, "an unknown group has no info");
    Expect(SosGroupsInfo(groups, 1u, NULL) == false, "a NULL out has no info");
    Expect(SosGroupsLeaveAll(groups, 1u, 0u) == 0u, "a being in no group leaves none");

    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, 0u, 1u, 0u);
    Expect(SosGroupsFound(groups, SOS_GROUP_CLAN, band, 0u, 0u) != 0u, "an empty clan is founded");
    Expect(SosGroupsHash(groups) != 0u, "a real world hashes nonzero");
    SosGroupsDestroy(groups);
}

/* A refused move must leave the whole world as it was, which the hash proves,
 * and a move must cope with a target that already holds one of the movers. */
static void TestAtomicity(void)
{
    SosGroups *groups = SosGroupsCreate();
    Expect(groups != NULL, "groups are created");
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 1u, 0u);
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 2u, 0u);
    const uint64_t squad = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 3u, 0u);
    Expect(SosGroupsJoin(groups, band, 4u, 10u), "atomicity: a member joins the band");
    Expect(SosGroupsJoin(groups, squad, 4u, 20u), "atomicity: the member is in both");

    const uint64_t before = SosGroupsHash(groups);
    const uint64_t movers[1] = {4u};
    /* Every rejected move returns before it reserves or touches a table. */
    Expect(SosGroupsMerge(groups, band, tribe, 30u) == false, "a cycle is refused");
    Expect(SosGroupsSplit(groups, 999u, movers, 1u, 30u) == 0u, "an unknown split is refused");
    Expect(SosGroupsSplit(groups, band, NULL, 1u, 30u) == 0u, "a listless split is refused");
    Expect(SosGroupsHash(groups) == before, "a refused move leaves the groups intact");

    /* The reserve-then-move path runs with a member already present in the
     * target, so the move must drop the duplicate and still succeed. */
    Expect(SosGroupsMerge(groups, band, squad, 40u), "the duplicate-holding group merges");
    Expect(SosGroupsOf(groups, 4u, 0u, NULL, 0u) == 1u, "the duplicate is counted once");
    Expect(SosGroupsMerge(groups, band, squad, 40u) == false, "the absorbed group is gone");
    Expect(SosGroupsHash(groups) != before, "a real merge changes the world");
    SosGroupsDestroy(groups);
}

/* A fixed history of foundings, joins and a split, used for both the
 * determinism check and the golden value. */
static void BuildScenario(SosGroups *groups)
{
    const uint64_t tribe = SosGroupsFound(groups, SOS_GROUP_TRIBE, 0u, 200u, 1u);
    const uint64_t band = SosGroupsFound(groups, SOS_GROUP_BAND, tribe, 100u, 10u);
    const uint64_t house = SosGroupsFound(groups, SOS_GROUP_HOUSEHOLD, band, 1u, 100u);
    Expect(SosGroupsJoin(groups, house, 2u, 200u), "scenario: a spouse joins");
    Expect(SosGroupsJoin(groups, house, 3u, 300u), "scenario: a child joins");
    Expect(SosGroupsJoin(groups, band, 4u, 150u), "scenario: a wanderer joins the band");
    Expect(SosGroupsJoin(groups, band, 5u, 250u), "scenario: a second wanderer joins");
    Expect(SosGroupsJoin(groups, tribe, 6u, 120u), "scenario: a family joins the tribe");
    Expect(SosGroupsLeave(groups, house, 3u, 400u), "scenario: the child leaves home");
    Expect(SosGroupsJoin(groups, house, 3u, 450u), "scenario: the child returns");
    const uint64_t guild = SosGroupsFound(groups, SOS_GROUP_GUILD, 0u, 7u, 500u);
    Expect(SosGroupsJoin(groups, guild, 2u, 510u), "scenario: a guild forms");
    Expect(SosGroupsJoin(groups, guild, 8u, 520u), "scenario: the guild grows");
    const uint64_t moved[2] = {4u, 5u};
    Expect(SosGroupsSplit(groups, band, moved, 2u, 600u) != 0u, "scenario: the band splits");
    Expect(SosGroupsMerge(groups, house, guild, 700u), "scenario: the guild merges into the house");
    Expect(SosGroupsSetLeader(groups, tribe, 6u), "scenario: the tribe chooses a leader");
}

static void TestDeterminism(void)
{
    SosGroups *first = SosGroupsCreate();
    SosGroups *second = SosGroupsCreate();
    Expect(first != NULL && second != NULL, "two worlds are created");
    BuildScenario(first);
    BuildScenario(second);
    Expect(SosGroupsHash(first) == SosGroupsHash(second), "the same history hashes equally");
    Expect(SosGroupsHash(first) != 0u, "a real history hashes nonzero");
    SosGroupsDestroy(first);
    SosGroupsDestroy(second);
}

/* A fixed point of the whole state. Every platform and compiler must fold the
 * reference history to exactly this value. */
static void TestGoldenValue(void)
{
    SosGroups *groups = SosGroupsCreate();
    Expect(groups != NULL, "groups are created");
    BuildScenario(groups);
    const uint64_t actual = SosGroupsHash(groups);
    const uint64_t expected = UINT64_C(0x992F5C586C1F0A32);
    if (actual != expected)
    {
        fprintf(stderr, "groups fingerprint: 0x%016" PRIX64 "\n", actual);
        Expect(0, "the group history matches the reference");
    }
    SosGroupsDestroy(groups);
}

int main(void)
{
    TestLifecycle();
    TestInfo();
    TestNesting();
    TestSuccession();
    TestLeaveAll();
    TestMergeAndSplit();
    TestInvalidIds();
    TestAtomicity();
    TestDeterminism();
    TestGoldenValue();
    return 0;
}
