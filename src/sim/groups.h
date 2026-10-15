#ifndef SOS_SIM_GROUPS_H
#define SOS_SIM_GROUPS_H

#include <stdbool.h>
#include <stdint.h>

/* Groups: unions of beings at every scale.
 *
 * A group's kind says what it is: a household, a band, a clan, a tribe. Kinds
 * are open ended. The named ones below are conveniences; any other 32-bit
 * value is a kind the rest of the world interprets, following the convention
 * that the properties of such an unbounded id are a function of the world
 * seed and the id itself, never of stored state. Groups nest through an
 * optional parent: a band can sit inside a tribe and a tribe inside an
 * alliance, without limit and without a separate tree.
 *
 * The state is sparse tables keyed by the ids the world already uses. One
 * table holds a group record per group; one holds a membership per
 * (group, being) pair; and two shared SosLinks sets thread the memberships
 * into each group's member list and each being's group list, walkable in time
 * proportional to their count. Ids come from a counter and are never reused,
 * so recorded history can name a group forever.
 *
 * Every operation touches only what it needs, allocates only to grow a table,
 * and leaves the object unchanged when memory runs out. Nothing is computed
 * as time passes: a group changes only when a caller changes it. Callers pass
 * the current tick, which is recorded where it matters (founding, joining)
 * so seniority stays meaningful and leadership can pass to the member who has
 * been there longest. */

/* What a group is. 0 stands for "no kind": founding with it fails. Values
 * beyond the named ones are open ended. */
typedef uint32_t SosGroupKind;

#define SOS_GROUP_NONE UINT32_C(0)
#define SOS_GROUP_HOUSEHOLD UINT32_C(1)
#define SOS_GROUP_BAND UINT32_C(2)
#define SOS_GROUP_CLAN UINT32_C(3)
#define SOS_GROUP_TRIBE UINT32_C(4)
#define SOS_GROUP_ALLIANCE UINT32_C(5)
#define SOS_GROUP_FAITH UINT32_C(6)
#define SOS_GROUP_GUILD UINT32_C(7)

/* The part a member plays. A member always carries SOS_GROUP_ROLE_LEADER
 * exactly when it is the group's leader, so the role mirrors SosGroupsLeader
 * for callers that read members as they walk a list. */
typedef uint32_t SosGroupRole;

#define SOS_GROUP_ROLE_MEMBER UINT32_C(0)
#define SOS_GROUP_ROLE_LEADER UINT32_C(1)

/* What is stored about one group, copied out for callers that would otherwise
 * have no way to read it. `parent` is 0 for a root, `leader` is 0 for a group
 * with no members, `foundedTick` is the tick the group was founded at (or
 * split off at), and `memberCount` is exact at the moment of the call. */
typedef struct SosGroupInfo
{
    SosGroupKind kind;
    uint64_t parent;
    uint64_t leader;
    uint64_t foundedTick;
    uint32_t memberCount;
} SosGroupInfo;

typedef struct SosGroups SosGroups;

/* A new, empty collection of groups, or NULL when memory ran out. */
SosGroups *SosGroupsCreate(void);
void SosGroupsDestroy(SosGroups *groups);

/* Founds a group of `kind`, nested under `parent` (0 for a root), led by
 * `founder` (0 for a group founded with no members), and returns its new id,
 * or 0 when the arguments are invalid, `parent` names no group, the counter
 * is exhausted or memory ran out. A nonzero `founder` becomes the first
 * member and the leader. `tick` is recorded as the founding tick, readable
 * through SosGroupsInfo. The id is never reused, even by a failed call. */
uint64_t SosGroupsFound(SosGroups *groups, SosGroupKind kind, uint64_t parent, uint64_t founder,
                        uint64_t tick);

/* Fills `out` with what is known about `group`: its kind, parent, current
 * leader, founding tick and member count. False when `groups` or `out` is
 * NULL, `group` is zero, or the group is unknown, in which case `out` is left
 * unchanged. O(1): it reads the stored record and never walks the members. */
bool SosGroupsInfo(const SosGroups *groups, uint64_t group, SosGroupInfo *out);

/* Adds `being` to `group`, joining at `tick`. False when an argument is zero
 * or unknown, when `being` is already a member, or when memory ran out, in
 * which case nothing changes. A group with no leader gains one. */
bool SosGroupsJoin(SosGroups *groups, uint64_t group, uint64_t being, uint64_t tick);

/* Removes `being` from `group`. False when an argument is zero or the being
 * is not a member; then nothing changes. When the leader leaves, the member
 * that joined earliest becomes leader; equal joining ticks go to the lower
 * serial. The group stays alive, empty if it has to. O(1) when a non-leader
 * leaves, but unlinking walks the group's member list and a leader's
 * departure scans it to elect a successor, so the cost is O(members). */
bool SosGroupsLeave(SosGroups *groups, uint64_t group, uint64_t being, uint64_t tick);

/* Removes `being` from every group it belongs to, as when it dies, running
 * the usual succession in each group it led. Returns how many memberships
 * were removed, or 0 when `groups` is NULL, `being` is zero, or it belongs to
 * no group; then nothing changes. `tick` is accepted to match SosGroupsLeave
 * and, like it, is not recorded. Costs O(total members) across every group
 * the being belonged to, for the same reasons as SosGroupsLeave. */
uint32_t SosGroupsLeaveAll(SosGroups *groups, uint64_t being, uint64_t tick);

/* Dissolves `group`: its members are released and its direct child groups are
 * re-parented to its own parent, so the hierarchy above them is kept. False
 * when the group is unknown; then nothing changes. */
bool SosGroupsDissolve(SosGroups *groups, uint64_t group);

/* Merges `from` into `into`: every member of `from` joins `into` at `tick`,
 * `from`'s child groups are re-parented to `into`, and `from` is dissolved.
 * False when either id is zero or unknown, when they are equal, or when
 * `into` is nested inside `from`, which the re-parenting would turn into a
 * parent cycle; then nothing changes. */
bool SosGroupsMerge(SosGroups *groups, uint64_t into, uint64_t from, uint64_t tick);

/* Splits `group` in two: founds a sibling of the same kind and parent at
 * `tick`, moves the members named in `beings` (a repeated or non-member entry
 * is ignored) into it, and returns the new id, or 0 when the group is unknown,
 * the list is empty or missing, the counter is exhausted or memory ran out.
 * Membership keeps each being's original joining tick. The new half is led by
 * its longest-standing member; the original keeps its leader unless that
 * member moved, when it elects another. */
uint64_t SosGroupsSplit(SosGroups *groups, uint64_t group, const uint64_t *beings, uint32_t count,
                        uint64_t tick);

/* The leader of `group` at the moment, or 0 when it has none or is unknown. */
uint64_t SosGroupsLeader(const SosGroups *groups, uint64_t group);

/* Makes `being`, already a member, the leader of `group`. False when an
 * argument is zero or unknown, or the being is not a member; then nothing
 * changes. */
bool SosGroupsSetLeader(SosGroups *groups, uint64_t group, uint64_t being);

/* Writes up to `capacity` members of `group` into `out` and returns how many
 * there are, which may exceed `capacity`, oldest member first. Zero when
 * `group` is unknown or `out` is NULL with a nonzero capacity. */
uint32_t SosGroupsMembers(const SosGroups *groups, uint64_t group, uint64_t *out,
                          uint32_t capacity);

/* Writes up to `capacity` groups that `being` belongs to into `out` and
 * returns how many there are, which may exceed `capacity`, in the order it
 * joined them. `kindFilter` 0 lists every kind; any other value lists only
 * that kind. Zero when `being` is zero or unknown, or `out` is NULL with a
 * nonzero capacity. */
uint32_t SosGroupsOf(const SosGroups *groups, uint64_t being, uint32_t kindFilter, uint64_t *out,
                     uint32_t capacity);

/* True when `inner` is `outer` or nested inside `outer` through any chain of
 * parents. False when either id is zero, either group is unknown, or they are
 * unrelated. */
bool SosGroupsIsWithin(const SosGroups *groups, uint64_t inner, uint64_t outer);

/* Folds the complete state into one value: equal states give equal hashes
 * wherever the tables happen to keep them, so replays and machines can be
 * compared. Zero for NULL. */
uint64_t SosGroupsHash(const SosGroups *groups);

#endif
