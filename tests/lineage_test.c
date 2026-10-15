#include "sim/lineage.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "lineage test failed: %s\n", message);
        exit(1);
    }
}

/* A birth with a tick derived from the serial, so the same serial always
 * carries the same tick and the tree does not depend on the test's order. */
static void Record(SosLineage *lineage, uint64_t serial, uint64_t mother, uint64_t father)
{
    Expect(SosLineageRecordBirth(lineage, serial, mother, father, serial * 1000u + 1u),
           "a birth is recorded");
}

/* A three-generation family.
 *
 *   founders, generation 0:  1, 2 | 3, 4, 5
 *   generation 1:  10 = child(1, 2), 11 = child(1, 2)
 *   generation 2:  20 = child(10, 3), 21 = child(10, 3), 22 = child(10, 5),
 *                  30 = child(11, 4)
 *   generation 3:  40 = child(20, 30), whose parents are first cousins
 */
static void BuildFamily(SosLineage *lineage)
{
    Record(lineage, 1u, 0u, 0u);
    Record(lineage, 2u, 0u, 0u);
    Record(lineage, 3u, 0u, 0u);
    Record(lineage, 4u, 0u, 0u);
    Record(lineage, 5u, 0u, 0u);
    Record(lineage, 10u, 1u, 2u);
    Record(lineage, 11u, 1u, 2u);
    Record(lineage, 20u, 10u, 3u);
    Record(lineage, 21u, 10u, 3u);
    Record(lineage, 22u, 10u, 5u);
    Record(lineage, 30u, 11u, 4u);
    Record(lineage, 40u, 20u, 30u);
}

/* The same family, recorded in another order within each generation: every
 * parent still precedes its children, but the sibling lists differ. */
static void BuildFamilyReordered(SosLineage *lineage)
{
    Record(lineage, 5u, 0u, 0u);
    Record(lineage, 2u, 0u, 0u);
    Record(lineage, 4u, 0u, 0u);
    Record(lineage, 1u, 0u, 0u);
    Record(lineage, 3u, 0u, 0u);
    Record(lineage, 11u, 1u, 2u);
    Record(lineage, 10u, 1u, 2u);
    Record(lineage, 22u, 10u, 5u);
    Record(lineage, 21u, 10u, 3u);
    Record(lineage, 20u, 10u, 3u);
    Record(lineage, 30u, 11u, 4u);
    Record(lineage, 40u, 20u, 30u);
}

static void TestCreateAndRejects(void)
{
    Expect(SosLineageCreate() != NULL, "a lineage is created");
    SosLineageDestroy(NULL);
    Expect(SosLineageHash(NULL) == 0u, "a null lineage hashes to zero");
    Expect(!SosLineageRecordBirth(NULL, 1u, 0u, 0u, 0u), "no birth without a lineage");

    SosLineage *lineage = SosLineageCreate();
    Expect(lineage != NULL, "a lineage for the rejections is created");
    Expect(SosLineageHash(lineage) == 0u, "an empty lineage hashes to zero");
    Expect(!SosLineageRecordBirth(lineage, 0u, 1u, 2u, 0u), "a serial of zero is rejected");
    Expect(SosLineageRecordBirth(lineage, 1u, 0u, 0u, 7u), "a founder is born");
    Expect(!SosLineageRecordBirth(lineage, 1u, 0u, 0u, 8u), "a serial is never reused");
    Expect(!SosLineageRecordBirth(lineage, 2u, 2u, 0u, 0u), "a being is not its own parent");
    Expect(!SosLineageRecordBirth(lineage, 3u, 0u, 3u, 0u), "a being is not its own parent");
    Expect(!SosLineageRecordBirth(lineage, 4u, 9u, 9u, 0u), "the two parents differ");
    Expect(SosLineageHash(lineage) != 0u, "one founder changes the hash");

    SosLineageRecord record;
    Expect(!SosLineageGet(lineage, 0u, &record), "a serial of zero has no record");
    Expect(!SosLineageGet(lineage, 99u, &record), "an unknown serial has no record");
    Expect(!SosLineageGet(lineage, 1u, NULL), "a null output is rejected");
    Expect(!SosLineageGet(NULL, 1u, &record), "no record without a lineage");
    Expect(SosLineageGet(lineage, 1u, &record), "a founder has a record");
    Expect(record.mother == 0u && record.father == 0u && record.generation == 0u,
           "a founder has no parents and generation zero");
    Expect(record.birthTick == 7u, "the birth tick is kept");

    /* An unrecorded parent still has a generation: the child is one past the
     * founder the parent is assumed to be. */
    Expect(SosLineageRecordBirth(lineage, 5u, 99u, 0u, 0u), "an unrecorded parent is allowed");
    Expect(SosLineageGet(lineage, 5u, &record) && record.generation == 1u,
           "a child of an unrecorded parent is generation one");
    Expect(SosLineageRecordBirth(lineage, 6u, 1u, 0u, UINT64_MAX), "an extreme tick is kept");
    Expect(SosLineageGet(lineage, 6u, &record) && record.birthTick == UINT64_MAX,
           "the extreme tick survives");

    /* A generation far past the depth bound is still recorded and counted. */
    Expect(SosLineageRecordBirth(lineage, 7u, 6u, 0u, 0u), "one more generation is born");
    Expect(SosLineageGet(lineage, 7u, &record) && record.generation == 2u,
           "the generation follows the parent");
    Expect(SosLineageIsAncestor(lineage, 1u, 7u), "a great grandparent is an ancestor");
    Expect(!SosLineageIsAncestor(lineage, 1u, 1u), "a being is not its own ancestor");
    Expect(!SosLineageIsAncestor(lineage, 0u, 7u), "nobody is not an ancestor");
    Expect(!SosLineageIsAncestor(lineage, 1u, 0u), "nobody has no ancestors");
    Expect(!SosLineageIsAncestor(lineage, 7u, 1u), "ancestry is directed");
    Expect(!SosLineageIsAncestor(lineage, 99u, 1u), "an unknown being is no ancestor");
    Expect(!SosLineageIsAncestor(lineage, 1u, 99u), "an unknown being has no ancestors");
    SosLineageDestroy(lineage);
}

static void TestRecords(const SosLineage *lineage)
{
    SosLineageRecord record;
    Expect(SosLineageGet(lineage, 10u, &record) && record.mother == 1u && record.father == 2u &&
               record.generation == 1u && record.birthTick == 10001u,
           "a second-generation being knows its parents");
    Expect(SosLineageGet(lineage, 20u, &record) && record.generation == 2u,
           "the third generation follows");
    Expect(SosLineageGet(lineage, 40u, &record) && record.mother == 20u && record.father == 30u &&
               record.generation == 3u,
           "a child of cousins is the fourth generation");
}

static void TestChildren(const SosLineage *lineage)
{
    uint64_t children[8] = {0u};
    Expect(SosLineageChildren(lineage, 10u, children, 8u) == 3u, "ten has three children");
    Expect(children[0] == 22u && children[1] == 21u && children[2] == 20u,
           "children come newest first");
    Expect(SosLineageChildren(lineage, 1u, children, 8u) == 2u && children[0] == 11u &&
               children[1] == 10u,
           "a founder mother's children are enumerated");
    Expect(SosLineageChildren(lineage, 4u, children, 8u) == 1u && children[0] == 30u,
           "a founder father's children are enumerated");
    Expect(SosLineageChildren(lineage, 5u, children, 8u) == 1u && children[0] == 22u,
           "a father is seen as well as a mother");
    Expect(SosLineageChildren(lineage, 40u, children, 8u) == 0u, "a leaf has no children");

    uint64_t two[2] = {0u};
    Expect(SosLineageChildren(lineage, 10u, two, 2u) == 3u, "the full count exceeds the capacity");
    Expect(two[0] == 22u && two[1] == 21u, "only the capacity is written");
    Expect(SosLineageChildren(lineage, 10u, NULL, 0u) == 3u, "the count alone can be asked");
    Expect(SosLineageChildren(lineage, 10u, NULL, 2u) == 0u, "a null buffer needs zero capacity");
    Expect(SosLineageChildren(lineage, 0u, children, 8u) == 0u, "nobody has no children");
    Expect(SosLineageChildren(lineage, 99u, children, 8u) == 0u, "an unknown being has none");
    Expect(SosLineageChildren(NULL, 10u, children, 8u) == 0u, "no children without a lineage");
}

static void TestKin(const SosLineage *lineage)
{
    Expect(SosLineageKinOf(lineage, 20u, 20u) == SOS_LINEAGE_KIN_SELF, "a being is itself");
    Expect(SosLineageKinOf(lineage, 20u, 10u) == SOS_LINEAGE_KIN_PARENT, "ten is twenty's parent");
    Expect(SosLineageKinOf(lineage, 10u, 20u) == SOS_LINEAGE_KIN_CHILD, "twenty is ten's child");
    Expect(SosLineageKinOf(lineage, 20u, 21u) == SOS_LINEAGE_KIN_FULL_SIBLING,
           "twenty and twenty-one are full siblings");
    Expect(SosLineageKinOf(lineage, 20u, 22u) == SOS_LINEAGE_KIN_HALF_SIBLING,
           "twenty and twenty-two are half siblings");
    Expect(SosLineageKinOf(lineage, 20u, 1u) == SOS_LINEAGE_KIN_GRANDPARENT,
           "one is twenty's grandparent");
    Expect(SosLineageKinOf(lineage, 1u, 20u) == SOS_LINEAGE_KIN_GRANDCHILD,
           "twenty is one's grandchild");
    Expect(SosLineageKinOf(lineage, 20u, 11u) == SOS_LINEAGE_KIN_AUNT_UNCLE,
           "eleven is twenty's aunt or uncle");
    Expect(SosLineageKinOf(lineage, 11u, 20u) == SOS_LINEAGE_KIN_NIECE_NEPHEW,
           "twenty is eleven's niece or nephew");
    Expect(SosLineageKinOf(lineage, 20u, 30u) == SOS_LINEAGE_KIN_FIRST_COUSIN,
           "twenty and thirty are first cousins");
    Expect(SosLineageKinOf(lineage, 40u, 1u) == SOS_LINEAGE_KIN_OTHER_RELATIVE,
           "a great grandparent is another relative");
    Expect(SosLineageKinOf(lineage, 3u, 5u) == SOS_LINEAGE_KIN_UNRELATED,
           "two unrelated founders are strangers");
    Expect(SosLineageKinOf(lineage, 40u, 3u) == SOS_LINEAGE_KIN_GRANDPARENT,
           "three is forty's grandparent through its mother");
    Expect(SosLineageKinOf(lineage, 0u, 0u) == SOS_LINEAGE_KIN_UNRELATED, "nobody is unrelated");
    Expect(SosLineageKinOf(lineage, 20u, 0u) == SOS_LINEAGE_KIN_UNRELATED, "nobody is unrelated");
    Expect(SosLineageKinOf(lineage, 20u, 99u) == SOS_LINEAGE_KIN_UNRELATED,
           "an unknown being is unrelated");
    Expect(SosLineageKinOf(NULL, 20u, 30u) == SOS_LINEAGE_KIN_UNRELATED,
           "no kin without a lineage");
}

static void TestRelatedness(const SosLineage *lineage)
{
    const SosQ16 half = SOS_Q16_ONE / 2;
    const SosQ16 quarter = SOS_Q16_ONE / 4;
    const SosQ16 eighth = SOS_Q16_ONE / 8;
    Expect(SosLineageRelatedness(lineage, 20u, 20u) == SOS_Q16_ONE, "a being is one to itself");
    Expect(SosLineageRelatedness(lineage, 20u, 10u) == half, "a parent is a half");
    Expect(SosLineageRelatedness(lineage, 10u, 20u) == half, "relatedness is symmetric");
    Expect(SosLineageRelatedness(lineage, 20u, 21u) == half, "a full sibling is a half");
    Expect(SosLineageRelatedness(lineage, 20u, 22u) == quarter, "a half sibling is a quarter");
    Expect(SosLineageRelatedness(lineage, 20u, 1u) == quarter, "a grandparent is a quarter");
    Expect(SosLineageRelatedness(lineage, 20u, 30u) == eighth, "a first cousin is an eighth");
    Expect(SosLineageRelatedness(lineage, 40u, 20u) == half, "a parent of a child of cousins");
    Expect(SosLineageRelatedness(lineage, 3u, 5u) == 0, "strangers are zero");
    Expect(SosLineageRelatedness(lineage, 0u, 20u) == 0, "nobody is zero to anyone");
    Expect(SosLineageRelatedness(lineage, 20u, 99u) == 0, "an unknown being is unrelated");
    Expect(SosLineageRelatedness(NULL, 20u, 30u) == 0, "no relatedness without a lineage");
}

/* Equal inputs give equal results, and the recorded order does not change the
 * folded tree. */
static void TestDeterminism(void)
{
    SosLineage *first = SosLineageCreate();
    SosLineage *second = SosLineageCreate();
    SosLineage *reordered = SosLineageCreate();
    Expect(first != NULL && second != NULL && reordered != NULL, "three lineages are created");
    BuildFamily(first);
    BuildFamily(second);
    BuildFamilyReordered(reordered);

    Expect(SosLineageHash(first) == SosLineageHash(second), "equal trees hash equally");
    Expect(SosLineageHash(first) == SosLineageHash(reordered),
           "the folded tree ignores the order of births");
    Expect(SosLineageRelatedness(first, 20u, 30u) == SosLineageRelatedness(second, 20u, 30u),
           "a repeated query gives the same answer");
    Expect(SosLineageKinOf(first, 20u, 30u) == SosLineageKinOf(second, 20u, 30u),
           "a repeated kin query gives the same answer");

    SosLineageRecord record;
    Expect(SosLineageGet(first, 10u, &record) && SosLineageGet(second, 10u, &record),
           "both trees hold the same records");
    SosLineageDestroy(reordered);
    SosLineageDestroy(second);
    SosLineageDestroy(first);
}

/* A fixed point of the tree. Every platform and compiler must fold this
 * hand-built family to exactly this hash. */
static void TestGolden(void)
{
    SosLineage *lineage = SosLineageCreate();
    Expect(lineage != NULL, "a lineage for the golden tree is created");
    BuildFamily(lineage);
    const uint64_t expected = UINT64_C(0x5141243886F2BFEF);
    const uint64_t actual = SosLineageHash(lineage);
    if (actual != expected)
    {
        fprintf(stderr, "lineage hash: 0x%016" PRIX64 "\n", actual);
        Expect(0, "the family tree matches the reference");
    }
    SosLineageDestroy(lineage);
}

int main(void)
{
    SosLineage *lineage = SosLineageCreate();
    Expect(lineage != NULL, "a lineage is created");
    TestCreateAndRejects();
    BuildFamily(lineage);
    TestRecords(lineage);
    TestChildren(lineage);
    TestKin(lineage);
    TestRelatedness(lineage);
    SosLineageDestroy(lineage);
    TestDeterminism();
    TestGolden();
    return 0;
}
