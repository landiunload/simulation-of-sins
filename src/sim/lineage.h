#ifndef SOS_SIM_LINEAGE_H
#define SOS_SIM_LINEAGE_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* The family tree of every being ever born.
 *
 * A being is named by its serial, which is never reused, so its ancestry can
 * be recorded once and kept forever. Every birth adds one compact row to a
 * sparse table keyed by serial: the two parents, the birth tick, the
 * generation and the links that make a parent's children enumerable. Two
 * sibling links are needed because a child belongs to two families, its
 * mother's and its father's; each child is threaded into both lists without
 * allocating a node per child, so children cost the record alone.
 *
 * Kinship is answered by walking up from a pair of beings through their
 * parents, bounded by SOS_LINEAGE_MAX_DEPTH, so the cost of a query never
 * grows with the population and every answer is the same function of the
 * recorded tree on every machine. */

/* How far up the tree a query climbs. Deeper ancestry is still recorded and
 * still hashed; kinship beyond this many generations counts as no relation,
 * which keeps every query bounded and cheap for millions of beings. */
#define SOS_LINEAGE_MAX_DEPTH 8u

/* One being's place in the tree.
 *
 * `mother` and `father` are serials or 0 when unknown; `generation` is 0 for a
 * founder and one more than the parents' maximum otherwise. `firstChild`
 * heads the newest-first list of a being's children, and each child links to
 * its next sibling through `nextSiblingOfMother` or `nextSiblingOfFather`
 * depending on which parent is being followed. The three links are derived
 * from the birth records and are kept out of the hash. */
typedef struct SosLineageRecord
{
    uint64_t mother;
    uint64_t father;
    uint64_t birthTick;
    uint32_t generation;
    uint64_t firstChild;
    uint64_t nextSiblingOfMother;
    uint64_t nextSiblingOfFather;
} SosLineageRecord;

/* How `b` is related to `a`, as `SosLineageKinOf` reports it: a parent, child,
 * sibling, grandparent, grandchild, aunt or uncle, niece or nephew, first
 * cousin, some more distant relative, or nobody. SELF means the same being
 * and UNRELATED is also the answer for a missing or unknown being. */
typedef enum SosLineageKin
{
    SOS_LINEAGE_KIN_UNRELATED = 0,
    SOS_LINEAGE_KIN_SELF,
    SOS_LINEAGE_KIN_PARENT,
    SOS_LINEAGE_KIN_CHILD,
    SOS_LINEAGE_KIN_FULL_SIBLING,
    SOS_LINEAGE_KIN_HALF_SIBLING,
    SOS_LINEAGE_KIN_GRANDPARENT,
    SOS_LINEAGE_KIN_GRANDCHILD,
    SOS_LINEAGE_KIN_AUNT_UNCLE,
    SOS_LINEAGE_KIN_NIECE_NEPHEW,
    SOS_LINEAGE_KIN_FIRST_COUSIN,
    SOS_LINEAGE_KIN_OTHER_RELATIVE,
} SosLineageKin;

typedef struct SosLineage SosLineage;

/* An empty tree, or NULL when memory ran out. */
SosLineage *SosLineageCreate(void);
void SosLineageDestroy(SosLineage *lineage);

/* Records the birth of `serial` to `mother` and `father`, either of which may
 * be 0 for an unknown parent. A being with two unknown parents is a founder
 * of generation 0; otherwise its generation is one more than the highest
 * generation among its recorded parents, an unrecorded parent counting as a
 * founder. False, leaving the tree unchanged, when `lineage` is NULL, `serial`
 * is 0, `serial` is already recorded, a parent is the being itself, the two
 * parents are the same being, or memory ran out. */
bool SosLineageRecordBirth(SosLineage *lineage, uint64_t serial, uint64_t mother, uint64_t father,
                           uint64_t birthTick);

/* Copies the record of `serial` into `outRecord`; false for a NULL lineage or
 * output, a zero serial or a serial that was never born. */
bool SosLineageGet(const SosLineage *lineage, uint64_t serial, SosLineageRecord *outRecord);

/* The children of `serial`, newest first. Writes up to `capacity` serials to
 * `out` and returns how many there are, which may exceed `capacity`. A NULL
 * `out` is allowed only with a zero capacity, when the count alone is
 * returned. Zero for a NULL lineage, a zero serial or an unknown being. */
uint32_t SosLineageChildren(const SosLineage *lineage, uint64_t serial, uint64_t *out,
                            uint32_t capacity);

/* Whether `ancestor` is a strict ancestor of `descendant` within
 * SOS_LINEAGE_MAX_DEPTH generations. False for zeros, equal serials, unknown
 * beings and lineages that are not connected that closely. */
bool SosLineageIsAncestor(const SosLineage *lineage, uint64_t ancestor, uint64_t descendant);

/* Wright's coefficient of relationship: the sum of 2^-(n1 + n2) over the most
 * recent common ancestors, where n1 and n2 count the generations from `a` and
 * `b` to that ancestor. Parents and full siblings give SOS_Q16_ONE / 2, half
 * siblings and grandparents a quarter, first cousins an eighth; a being is 1
 * to itself and unrelated beings are 0. Only ancestry within
 * SOS_LINEAGE_MAX_DEPTH is seen, so a distant tie may read as 0, and the
 * result is clamped to at most SOS_Q16_ONE. Zero for a NULL lineage, a zero
 * serial or an unknown being. */
SosQ16 SosLineageRelatedness(const SosLineage *lineage, uint64_t a, uint64_t b);

/* How `b` is related to `a`, close relations taking precedence over distant
 * ones, from the cases of SosLineageKin. UNRELATED for a NULL lineage, a zero
 * serial or an unknown being. */
SosLineageKin SosLineageKinOf(const SosLineage *lineage, uint64_t a, uint64_t b);

/* Folds the recorded births into one value, order-independently. Two trees
 * with the same parents, ticks and generations hash equally however their
 * records were inserted; the derived child links are not hashed. Zero for a
 * NULL lineage. */
uint64_t SosLineageHash(const SosLineage *lineage);

#endif
