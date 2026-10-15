#include "sim/calendar.h"
#include "sim/cells.h"
#include "sim/deposits.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define BOX_CELLS 64
#define HILL_CELLS 128

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "deposits test failed: %s\n", message);
        exit(1);
    }
}

static SosDeposit box[BOX_CELLS * BOX_CELLS];
static SosDeposit hill[HILL_CELLS * HILL_CELLS];

/* Deposits in `cells` cells per side starting at a cell. */
static uint32_t Query(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                      int64_t cellY, uint32_t cells, uint64_t tick, SosDeposit *outDeposits,
                      uint32_t capacity)
{
    const int64_t minX = cellX * SOS_DEPOSIT_CELL_UNITS;
    const int64_t minY = cellY * SOS_DEPOSIT_CELL_UNITS;
    const int64_t span = (int64_t)cells * SOS_DEPOSIT_CELL_UNITS - 1;
    const uint32_t count = SosDepositsQuery(deposits, terrain, minX, minY, minX + span, minY + span,
                                            tick, outDeposits, capacity);
    Expect(count <= capacity, "a box holds at most one deposit per cell");
    return count;
}

static uint32_t QueryBox(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                         int64_t cellY, uint64_t tick)
{
    return Query(deposits, terrain, cellX, cellY, BOX_CELLS, tick, box, BOX_CELLS * BOX_CELLS);
}

static void TestPlacement(const SosDeposits *deposits, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(deposits, terrain, 0, 0, 0u);
    /* Near the origin the seed gives a meadow between the sand and hill
     * lines: two kinds, and about one cell in seven. */
    const uint32_t cells = BOX_CELLS * BOX_CELLS;
    Expect(count > cells / 12u && count < cells / 6u, "deposits cover a meadow at their density");
    uint32_t timber = 0u;
    uint32_t ochre = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosDeposit *deposit = &box[index];
        Expect(deposit->kind == SOS_DEPOSIT_TIMBER || deposit->kind == SOS_DEPOSIT_OCHRE,
               "an inland meadow holds only timber and ochre");
        timber += deposit->kind == SOS_DEPOSIT_TIMBER ? 1u : 0u;
        ochre += deposit->kind == SOS_DEPOSIT_OCHRE ? 1u : 0u;
        Expect(deposit->richness > 0 && deposit->richness <= SOS_Q16_ONE, "richness is a fraction");
        Expect(deposit->amount > 0u, "an untouched deposit holds material");
        const int64_t inX = deposit->position.x - deposit->cellX * SOS_DEPOSIT_CELL_UNITS;
        const int64_t inY = deposit->position.y - deposit->cellY * SOS_DEPOSIT_CELL_UNITS;
        Expect(inX >= SOS_METRES(2) && inX < SOS_DEPOSIT_CELL_UNITS - SOS_METRES(2) &&
                   inY >= SOS_METRES(2) && inY < SOS_DEPOSIT_CELL_UNITS - SOS_METRES(2),
               "a deposit keeps away from its cell's edges");
        const SosTerrainSurface surface =
            SosTerrainSurfaceAt(terrain, deposit->position.x, deposit->position.y);
        Expect(surface.z == deposit->position.z && surface.material == SOS_MATERIAL_GRASS,
               "a deposit stands on the grass surface");
        SosDeposit single;
        Expect(SosDepositsInCell(deposits, terrain, deposit->cellX, deposit->cellY, 0u, &single) &&
                   single.position.x == deposit->position.x &&
                   single.position.y == deposit->position.y && single.kind == deposit->kind,
               "a cell and a query see the same deposit");
    }
    Expect(timber > 0u && ochre > 0u, "the meadow holds both of its kinds");
}

/* The kind a cell gets follows its surface material and height. */
static void TestKinds(const SosDeposits *deposits, const SosTerrain *terrain)
{
    const uint32_t sand = QueryBox(deposits, terrain, 400, 0, 0u);
    uint32_t clay = 0u;
    uint32_t salt = 0u;
    uint32_t reed = 0u;
    for (uint32_t index = 0u; index < sand; ++index)
    {
        Expect(box[index].kind == SOS_DEPOSIT_CLAY || box[index].kind == SOS_DEPOSIT_SALT ||
                   box[index].kind == SOS_DEPOSIT_REED,
               "a shore of sand holds clay, salt and reeds");
        clay += box[index].kind == SOS_DEPOSIT_CLAY ? 1u : 0u;
        salt += box[index].kind == SOS_DEPOSIT_SALT ? 1u : 0u;
        reed += box[index].kind == SOS_DEPOSIT_REED ? 1u : 0u;
    }
    Expect(sand > 0u && clay > 0u && salt > 0u && reed > 0u, "the shore holds all three");

    const uint32_t peak =
        Query(deposits, terrain, -1000, -800, HILL_CELLS, 0u, hill, HILL_CELLS * HILL_CELLS);
    uint32_t flint = 0u;
    uint32_t stone = 0u;
    uint32_t obsidian = 0u;
    for (uint32_t index = 0u; index < peak; ++index)
    {
        Expect(hill[index].kind == SOS_DEPOSIT_FLINT || hill[index].kind == SOS_DEPOSIT_STONE ||
                   hill[index].kind == SOS_DEPOSIT_OBSIDIAN ||
                   hill[index].kind == SOS_DEPOSIT_TIMBER,
               "hill country holds rock and timber");
        flint += hill[index].kind == SOS_DEPOSIT_FLINT ? 1u : 0u;
        stone += hill[index].kind == SOS_DEPOSIT_STONE ? 1u : 0u;
        obsidian += hill[index].kind == SOS_DEPOSIT_OBSIDIAN ? 1u : 0u;
    }
    Expect(flint > 0u && stone > 0u && obsidian > 0u && obsidian < stone,
           "hills hold flint, stone and rare obsidian");
}

static void FindEmptyCell(const SosDeposits *deposits, const SosTerrain *terrain, int64_t *outX,
                          int64_t *outY)
{
    for (int64_t cellX = 0;; ++cellX)
    {
        SosDeposit deposit;
        if (!SosDepositsInCell(deposits, terrain, cellX, 3, 0u, &deposit))
        {
            *outX = cellX;
            *outY = 3;
            return;
        }
    }
}

static void TestExtraction(SosDeposits *deposits, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(deposits, terrain, 0, 0, 0u);
    Expect(count > 0u, "the meadow has deposits");
    SosDeposit mineral = box[0];
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (box[index].kind == SOS_DEPOSIT_OCHRE)
        {
            mineral = box[index];
            break;
        }
    }
    const uint64_t tick = 1000u;
    const uint32_t full = mineral.amount;
    Expect(full > 10u, "a mineral deposit holds a useful amount");
    Expect(SosDepositsExtract(deposits, terrain, mineral.cellX, mineral.cellY, tick, 0u) == 0u,
           "a zero request takes nothing");
    Expect(SosDepositsExtract(deposits, terrain, mineral.cellX, mineral.cellY, tick, 10u) == 10u,
           "an extraction takes what was asked");
    Expect(SosDepositsExtract(deposits, terrain, mineral.cellX, mineral.cellY, tick, full * 2u) ==
               full - 10u,
           "an extraction takes no more than is left");
    SosDeposit after;
    Expect(SosDepositsInCell(deposits, terrain, mineral.cellX, mineral.cellY, tick, &after) &&
               after.amount == 0u,
           "a worked-out mineral is empty");
    Expect(SosDepositsExtract(deposits, terrain, mineral.cellX, mineral.cellY, tick, 5u) == 0u,
           "a worked-out mineral gives nothing");
    Expect(SosDepositsInCell(deposits, terrain, mineral.cellX, mineral.cellY,
                             (uint64_t)(10 * SOS_TICKS_PER_YEAR), &after) &&
               after.amount == 0u,
           "a mineral never renews");

    int64_t emptyX = 0;
    int64_t emptyY = 0;
    FindEmptyCell(deposits, terrain, &emptyX, &emptyY);
    Expect(SosDepositsExtract(deposits, terrain, emptyX, emptyY, tick, 5u) == 0u,
           "an empty cell gives nothing");
}

/* Timber and reeds grow back over their renewal time; a grown-back deposit is
 * remembered as never worked. */
static void TestRenewal(const SosTerrain *terrain)
{
    SosDeposits *worked = SosDepositsCreate(SEED);
    SosDeposits *untouched = SosDepositsCreate(SEED);
    Expect(worked != NULL && untouched != NULL, "deposits are created");
    const uint32_t count = QueryBox(worked, terrain, 0, 0, 0u);
    SosDeposit tree = {0};
    bool found = false;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (box[index].kind == SOS_DEPOSIT_TIMBER)
        {
            tree = box[index];
            found = true;
            break;
        }
    }
    Expect(found, "the meadow has timber");
    const uint32_t full = tree.amount;
    const uint64_t tick = 1000u;
    const uint64_t renewal = SOS_DEPOSIT_TIMBER_RENEWAL;
    Expect(SosDepositsExtract(worked, terrain, tree.cellX, tree.cellY, tick, full) == full,
           "a tree can be felled completely");
    SosDeposit middle;
    Expect(SosDepositsInCell(worked, terrain, tree.cellX, tree.cellY, tick, &middle) &&
               middle.amount == 0u,
           "a felled tree is empty");
    Expect(
        SosDepositsInCell(worked, terrain, tree.cellX, tree.cellY, tick + renewal / 2u, &middle) &&
            middle.amount > 0u && middle.amount < full,
        "a tree grows back slowly");
    Expect(SosDepositsInCell(worked, terrain, tree.cellX, tree.cellY, tick + renewal, &middle) &&
               middle.amount == full,
           "a tree is whole again after its renewal time");
    Expect(SosDepositsHash(worked, tick + renewal) == SosDepositsHash(untouched, tick + renewal),
           "a grown-back deposit counts as never worked");
    Expect(SosDepositsHash(worked, tick) != SosDepositsHash(untouched, tick),
           "a felled tree changes the hash");
    Expect(SosDepositsExtract(worked, terrain, tree.cellX, tree.cellY, tick + renewal, 5u) == 5u,
           "a grown-back tree can be felled again");
    SosDepositsDestroy(untouched);
    SosDepositsDestroy(worked);
}

static void TestFindNearest(SosDeposits *deposits, const SosTerrain *terrain)
{
    const uint64_t tick = 5000u;
    const int64_t radius = SOS_METRES(64);
    SosRandom random = SosRandomStream(SEED, 7u);
    for (int trial = 0; trial < 40; ++trial)
    {
        const SosPosition around = {
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            0,
        };
        /* Every deposit within the radius, by brute force. */
        const int64_t first = SosFloorShift(around.x - radius, SOS_DEPOSIT_CELL_SHIFT);
        const int64_t firstY = SosFloorShift(around.y - radius, SOS_DEPOSIT_CELL_SHIFT);
        const uint32_t count = QueryBox(deposits, terrain, first, firstY, tick);
        uint64_t nearest = UINT64_MAX;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const uint64_t distance = SosCellsGroundDistanceSquared(around, box[index].position);
            if (box[index].amount > 0u && distance <= (uint64_t)(radius * radius) &&
                distance < nearest)
            {
                nearest = distance;
            }
        }
        SosDeposit found;
        const bool any = SosDepositsFindNearest(deposits, terrain, around, radius, tick,
                                                SOS_DEPOSIT_KIND_ALL, &found);
        Expect(any == (nearest != UINT64_MAX),
               "a search finds a deposit exactly when one is in reach");
        if (!any)
        {
            continue;
        }
        Expect(found.amount > 0u &&
                   SosCellsGroundDistanceSquared(around, found.position) == nearest,
               "a search finds the nearest deposit with material");
        /* Work it out: the next search finds another one no nearer. */
        Expect(SosDepositsExtract(deposits, terrain, found.cellX, found.cellY, tick,
                                  found.amount) == found.amount,
               "the found deposit is worked out");
        SosDeposit next;
        if (SosDepositsFindNearest(deposits, terrain, around, radius, tick, SOS_DEPOSIT_KIND_ALL,
                                   &next))
        {
            Expect(next.cellX != found.cellX || next.cellY != found.cellY,
                   "a worked-out deposit is not found again");
            Expect(SosCellsGroundDistanceSquared(around, next.position) >= nearest,
                   "the next deposit is no nearer");
        }
        /* A kind filter keeps only that kind. */
        SosDeposit timber;
        if (SosDepositsFindNearest(deposits, terrain, around, radius, tick,
                                   SOS_DEPOSIT_KIND_MASK(SOS_DEPOSIT_TIMBER), &timber))
        {
            Expect(timber.kind == SOS_DEPOSIT_TIMBER, "the kind filter holds");
        }
    }
    SosDeposit unused;
    Expect(!SosDepositsFindNearest(deposits, terrain, (SosPosition){0, 0, 0}, radius, tick, 0u,
                                   &unused),
           "an empty kind filter finds nothing");
    Expect(!SosDepositsFindNearest(deposits, terrain, (SosPosition){0, 0, 0},
                                   SOS_DEPOSIT_QUERY_MAX_CELLS * SOS_DEPOSIT_CELL_UNITS, tick,
                                   SOS_DEPOSIT_KIND_ALL, &unused),
           "an oversized search is rejected");
}

/* Same seed and same work give the same state on every machine. */
static void TestDeterminism(const SosTerrain *terrain)
{
    SosDeposits *first = SosDepositsCreate(SEED);
    SosDeposits *second = SosDepositsCreate(SEED);
    SosDeposits *virgin = SosDepositsCreate(SEED);
    SosDeposits *otherSeed = SosDepositsCreate(SEED + 1u);
    Expect(first != NULL && second != NULL && virgin != NULL && otherSeed != NULL,
           "deposits are created");
    Expect(SosDepositsHash(first, 0u) == SosDepositsHash(virgin, 0u),
           "equal deposits hash equally");
    const uint32_t count = QueryBox(first, terrain, 300, 300, 0u);
    Expect(count > 0u, "the worked region has deposits");
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t amount = 3u;
        Expect(SosDepositsExtract(first, terrain, box[index].cellX, box[index].cellY, 10u,
                                  amount) == SosDepositsExtract(second, terrain, box[index].cellX,
                                                                box[index].cellY, 10u, amount),
               "the same work takes the same material");
    }
    Expect(SosDepositsHash(first, 10u) == SosDepositsHash(second, 10u),
           "the same work gives the same hash");
    Expect(SosDepositsHash(first, 10u) != SosDepositsHash(virgin, 10u),
           "working deposits changes the hash");
    Expect(SosDepositsHash(first, 10u) != SosDepositsHash(otherSeed, 10u),
           "different seeds give different worlds");
    Expect(SosDepositsHash(NULL, 0u) == 0u, "a missing layer hashes to zero");
    SosDepositsDestroy(otherSeed);
    SosDepositsDestroy(virgin);
    SosDepositsDestroy(second);
    SosDepositsDestroy(first);
}

/* Fixed points of the deposit cover. Every platform and compiler must place
 * exactly these deposits. */
static void TestGoldenValues(const SosTerrain *terrain)
{
    SosDeposits *deposits = SosDepositsCreate(SEED);
    Expect(deposits != NULL, "deposits are created");
    const uint32_t count = QueryBox(deposits, terrain, -1000, 2500, 0u);
    uint64_t fingerprint = count;
    for (uint32_t index = 0u; index < count; ++index)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].cellX);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].cellY);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].kind);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.x);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.y);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.z);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].richness);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].amount);
    }
    const uint64_t expected = UINT64_C(0xA737D708B1653AD0);
    if (fingerprint != expected)
    {
        fprintf(stderr, "deposit fingerprint: 0x%016" PRIX64 " (%u deposits)\n", fingerprint,
                count);
        Expect(0, "the deposit cover matches the reference");
    }
    SosDepositsDestroy(deposits);
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    SosDeposits *deposits = SosDepositsCreate(SEED);
    Expect(deposits != NULL, "deposits are created");
    TestPlacement(deposits, &terrain);
    TestKinds(deposits, &terrain);
    TestExtraction(deposits, &terrain);
    TestRenewal(&terrain);
    TestFindNearest(deposits, &terrain);
    TestDeterminism(&terrain);
    TestGoldenValues(&terrain);
    SosDepositsDestroy(deposits);
    return 0;
}
