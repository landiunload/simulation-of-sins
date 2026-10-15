#include "sim/bonds.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "bonds test failed: %s\n", message);
        exit(1);
    }
}

static void TestInvalidInput(void)
{
    SosBond bond;
    Expect(SosBondsHash(NULL) == 0u && SosBondsHistoryCount(NULL, 1u) == 0u, "NULL is empty");
    Expect(SosBondsEndAllOf(NULL, 1u, SOS_BOND_END_DEATH, 0u) == 0u, "NULL ends nothing");
    Expect(!SosBondsForm(NULL, 1u, 2u, SOS_BOND_UNION, 0u, 1u), "NULL forms nothing");
    Expect(!SosBondsEnd(NULL, 1u, 2u, SOS_BOND_END_SEPARATION, 0u), "NULL ends nothing");
    Expect(!SosBondsGet(NULL, 1u, 2u, &bond), "NULL has no bond");
    Expect(SosBondsPartners(NULL, 1u, false, NULL, 0u) == 0u, "NULL has no partners");
    SosBondsDestroy(NULL);

    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    Expect(!SosBondsForm(bonds, 0u, 2u, SOS_BOND_UNION, 0u, 1u), "a zero serial cannot bond");
    Expect(!SosBondsForm(bonds, 2u, 2u, SOS_BOND_UNION, 0u, 1u), "a being cannot bond itself");
    Expect(!SosBondsForm(bonds, 1u, 2u, SOS_BOND_KIND_COUNT, 0u, 1u),
           "an unknown kind cannot bond");
    Expect(!SosBondsForm(bonds, 1u, 2u, SOS_BOND_UNION, 0u, 0u), "no partner is allowed");
    Expect(!SosBondsGet(bonds, 1u, 2u, &bond), "an unknown pair has no bond");
    Expect(!SosBondsGet(bonds, 1u, 2u, NULL), "a missing output is rejected");
    Expect(!SosBondsEnd(bonds, 1u, 2u, SOS_BOND_END_NONE, 0u), "a bond cannot end with no reason");
    Expect(!SosBondsEnd(bonds, 1u, 2u, SOS_BOND_END_DEATH, 0u), "an absent bond cannot end");
    Expect(SosBondsPartners(bonds, 1u, false, NULL, 1u) == 0u, "a missing output is rejected");
    Expect(SosBondsHistoryCount(bonds, 0u) == 0u, "nobody has no history");
    SosBonds *other = SosBondsCreate();
    Expect(other != NULL && SosBondsHash(bonds) == SosBondsHash(other),
           "equal empty sets hash equally");
    SosBondsDestroy(other);
    SosBondsDestroy(bonds);
}

static void TestFormAndEnd(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    const uint64_t a = 10u;
    const uint64_t b = 20u;
    Expect(SosBondsForm(bonds, b, a, SOS_BOND_COURTSHIP, 100u, 1u),
           "a bond forms whatever the order");
    Expect(!SosBondsForm(bonds, a, b, SOS_BOND_UNION, 200u, 1u),
           "an active pair cannot bond again");
    SosBond bond;
    Expect(SosBondsGet(bonds, a, b, &bond), "the bond is found");
    Expect(bond.first == a && bond.second == b && bond.kind == SOS_BOND_COURTSHIP &&
               bond.endReason == SOS_BOND_END_NONE && bond.formedTick == 100u &&
               bond.endedTick == 0u && bond.reunions == 0u,
           "the bond keeps what was formed");
    Expect(!SosBondsGet(bonds, a, a, &bond), "a being has no bond with itself");
    uint64_t partners[4];
    Expect(SosBondsPartners(bonds, a, true, partners, 4u) == 1u && partners[0] == b,
           "an active partner is listed");
    Expect(SosBondsHistoryCount(bonds, a) == 1u && SosBondsHistoryCount(bonds, b) == 1u,
           "history counts the bond once");
    Expect(!SosBondsEnd(bonds, a, b, SOS_BOND_END_SEPARATION, 99u),
           "an end before forming is rejected");
    Expect(SosBondsEnd(bonds, a, b, SOS_BOND_END_SEPARATION, 300u), "a bond ends");
    Expect(SosBondsGet(bonds, a, b, &bond) && bond.endReason == SOS_BOND_END_SEPARATION &&
               bond.endedTick == 300u,
           "the end is remembered");
    Expect(SosBondsPartners(bonds, a, true, partners, 4u) == 0u,
           "an ended bond is no longer active");
    Expect(SosBondsPartners(bonds, a, false, partners, 4u) == 1u && partners[0] == b,
           "an ended partner stays in history");
    Expect(SosBondsHistoryCount(bonds, a) == 1u, "history keeps the ended bond");
    Expect(!SosBondsEnd(bonds, a, b, SOS_BOND_END_DEATH, 400u), "an ended bond cannot end again");
    SosBondsDestroy(bonds);
}

static void TestLimits(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    Expect(SosBondsForm(bonds, 1u, 2u, SOS_BOND_UNION, 10u, 1u), "the first bond forms");
    Expect(!SosBondsForm(bonds, 1u, 3u, SOS_BOND_UNION, 11u, 1u),
           "a monogamous being cannot take a second");
    Expect(!SosBondsForm(bonds, 3u, 2u, SOS_BOND_UNION, 11u, 1u),
           "and neither can the other's partner");
    Expect(SosBondsEnd(bonds, 1u, 2u, SOS_BOND_END_SEPARATION, 12u), "the bond ends");
    Expect(SosBondsForm(bonds, 1u, 3u, SOS_BOND_UNION, 13u, 1u), "an ended bond frees the being");
    Expect(!SosBondsForm(bonds, 2u, 3u, SOS_BOND_UNION, 14u, 1u), "the other being is still full");

    Expect(SosBondsForm(bonds, 1u, 4u, SOS_BOND_UNION, 15u, 2u),
           "a polygamous being takes a second bond");
    Expect(!SosBondsForm(bonds, 1u, 5u, SOS_BOND_UNION, 16u, 2u), "but not a third");
    uint64_t partners[4];
    Expect(SosBondsPartners(bonds, 1u, true, partners, 4u) == 2u,
           "the polygamous being has two active bonds");
    SosBondsDestroy(bonds);
}

static void TestDeath(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    Expect(SosBondsForm(bonds, 7u, 8u, SOS_BOND_MARRIAGE, 1000u, 1u), "a marriage forms");
    uint64_t partners[2];
    Expect(SosBondsPartners(bonds, 8u, true, partners, 2u) == 1u && partners[0] == 7u,
           "the spouse is listed");
    Expect(SosBondsEndAllOf(bonds, 7u, SOS_BOND_END_DEATH, 5000u) == 1u, "death ends the marriage");
    Expect(SosBondsPartners(bonds, 8u, true, partners, 2u) == 0u, "the widow has no active spouse");
    Expect(SosBondsPartners(bonds, 8u, false, partners, 2u) == 1u, "the widow remembers it");
    SosBond bond;
    Expect(SosBondsGet(bonds, 7u, 8u, &bond) && bond.endReason == SOS_BOND_END_DEATH &&
               bond.endedTick == 5000u,
           "death is the reason");
    Expect(SosBondsEndAllOf(bonds, 7u, SOS_BOND_END_DEATH, 6000u) == 0u, "the dead have no bonds");
    Expect(SosBondsForm(bonds, 8u, 9u, SOS_BOND_MARRIAGE, 7000u, 1u), "a widow may marry again");
    Expect(SosBondsHistoryCount(bonds, 8u) == 2u, "history keeps both marriages");

    Expect(SosBondsForm(bonds, 20u, 21u, SOS_BOND_UNION, 100u, 2u) &&
               SosBondsForm(bonds, 20u, 22u, SOS_BOND_UNION, 101u, 2u),
           "a being holds two bonds");
    Expect(SosBondsEndAllOf(bonds, 20u, SOS_BOND_END_DEATH, 200u) == 2u, "death ends every bond");
    Expect(SosBondsPartners(bonds, 21u, true, partners, 2u) == 0u &&
               SosBondsPartners(bonds, 22u, true, partners, 2u) == 0u,
           "both partners are widowed");
    SosBondsDestroy(bonds);
}

static void TestReunion(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    Expect(SosBondsForm(bonds, 1u, 2u, SOS_BOND_COURTSHIP, 10u, 1u), "a courtship forms");
    Expect(SosBondsEnd(bonds, 1u, 2u, SOS_BOND_END_SEPARATION, 20u), "the courtship ends");
    Expect(SosBondsForm(bonds, 2u, 1u, SOS_BOND_UNION, 30u, 1u), "the pair bonds again");
    SosBond bond;
    Expect(SosBondsGet(bonds, 1u, 2u, &bond) && bond.kind == SOS_BOND_UNION &&
               bond.endReason == SOS_BOND_END_NONE && bond.formedTick == 30u &&
               bond.endedTick == 0u && bond.reunions == 1u,
           "a reunion reuses the record");
    Expect(SosBondsHistoryCount(bonds, 1u) == 1u, "a reunion is not a second partner");
    uint64_t partners[4];
    Expect(SosBondsPartners(bonds, 1u, false, partners, 4u) == 1u,
           "a reunion does not duplicate the partner");
    Expect(SosBondsEnd(bonds, 1u, 2u, SOS_BOND_END_SEPARATION, 40u), "the reunion ends");
    Expect(SosBondsForm(bonds, 1u, 2u, SOS_BOND_MARRIAGE, 50u, 1u), "the pair reunites again");
    Expect(SosBondsGet(bonds, 1u, 2u, &bond) && bond.reunions == 2u, "reunions accumulate");
    SosBondsDestroy(bonds);
}

static void TestPartnersCapacity(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    for (uint64_t partner = 2u; partner <= 5u; ++partner)
    {
        Expect(SosBondsForm(bonds, 1u, partner, SOS_BOND_UNION, partner, 10u), "a bond forms");
    }
    uint64_t one[1];
    Expect(SosBondsPartners(bonds, 1u, false, one, 1u) == 4u, "the count exceeds the capacity");
    Expect(one[0] == 2u, "the oldest partner fits first");
    uint64_t all[8];
    Expect(SosBondsPartners(bonds, 1u, false, all, 8u) == 4u, "every partner is listed");
    Expect(all[0] == 2u && all[1] == 3u && all[2] == 4u && all[3] == 5u,
           "partners come back in the order the bonds formed");
    Expect(SosBondsPartners(bonds, 1u, false, NULL, 0u) == 4u, "a null output only counts");
    Expect(SosBondsPartners(bonds, 99u, false, all, 8u) == 0u, "a stranger has no partners");
    SosBondsDestroy(bonds);
}

static void TestDeterminism(void)
{
    SosBonds *first = SosBondsCreate();
    SosBonds *second = SosBondsCreate();
    Expect(first != NULL && second != NULL, "bonds are created");
    for (uint64_t partner = 2u; partner <= 20u; ++partner)
    {
        (void)SosBondsForm(first, 1u, partner, SOS_BOND_UNION, partner, 30u);
        (void)SosBondsForm(second, 1u, partner, SOS_BOND_UNION, partner, 30u);
    }
    (void)SosBondsEnd(first, 1u, 2u, SOS_BOND_END_SEPARATION, 100u);
    (void)SosBondsEnd(second, 1u, 2u, SOS_BOND_END_SEPARATION, 100u);
    (void)SosBondsForm(first, 1u, 2u, SOS_BOND_MARRIAGE, 200u, 30u);
    (void)SosBondsForm(second, 1u, 2u, SOS_BOND_MARRIAGE, 200u, 30u);
    Expect(SosBondsHash(first) == SosBondsHash(second), "equal histories hash equally");
    (void)SosBondsEnd(second, 3u, 1u, SOS_BOND_END_DEATH, 300u);
    Expect(SosBondsHash(first) != SosBondsHash(second), "a different history hashes differently");
    SosBondsDestroy(second);
    SosBondsDestroy(first);
}

/* Fixed point of the module. Every platform and compiler must fold this exact
 * history to the same value. */
static void TestGolden(void)
{
    SosBonds *bonds = SosBondsCreate();
    Expect(bonds != NULL, "bonds are created");
    for (uint64_t partner = 2u; partner <= 12u; ++partner)
    {
        (void)SosBondsForm(bonds, 1u, partner, SOS_BOND_UNION, partner * 100u, 20u);
    }
    (void)SosBondsEnd(bonds, 1u, 5u, SOS_BOND_END_SEPARATION, 5000u);
    (void)SosBondsEndAllOf(bonds, 1u, SOS_BOND_END_DEATH, 9000u);
    (void)SosBondsForm(bonds, 2u, 3u, SOS_BOND_COURTSHIP, 10000u, 1u);
    const uint64_t fingerprint = SosBondsHash(bonds);
    const uint64_t expected = UINT64_C(0x05C1134B33501A21);
    if (fingerprint != expected)
    {
        fprintf(stderr, "bonds fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the bond history matches the reference");
    }
    SosBondsDestroy(bonds);
}

int main(void)
{
    TestInvalidInput();
    TestFormAndEnd();
    TestLimits();
    TestDeath();
    TestReunion();
    TestPartnersCapacity();
    TestDeterminism();
    TestGolden();
    return 0;
}
