#include "sim/deposits.h"

#include "sim/cells.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stddef.h>
#include <stdlib.h>

/* Chances are out of 65536 cells. */
#define CHANCE_BITS 16u
#define CHANCE_MASK ((UINT64_C(1) << CHANCE_BITS) - 1u)
/* A deposit keeps two metres from its cell's edges, so its cell names it. */
#define CELL_MARGIN SOS_METRES(2)
#define CELL_SPAN (SOS_DEPOSIT_CELL_UNITS - 2 * CELL_MARGIN)
/* Cell coordinates within this stay inside the 64-bit unit range after being
 * scaled by the cell size. */
#define CELL_LIMIT (INT64_C(1) << 47)
/* Fully regrown records are dropped when the table reaches this size, which
 * doubles after every sweep, so pruning costs O(1) per extraction. */
#define PRUNE_START 64u

/* One bit per material, for a kind's list of surfaces. */
#define MATERIAL_BIT(material) (UINT32_C(1) << (uint32_t)(material))

/* The terrain's sand line: below it the ground is beach sand. */
#define SAND_LINE_Z SOS_METRES(-20)
/* Low ground within this of the sand line holds clay, reeds and salt. */
#define NEAR_SAND_MAX (SAND_LINE_Z + SOS_METRES(60))
/* Above this the ground is hill country, where flint and stone crop out. */
#define HILL_LINE SOS_METRES(150)
/* Above this only rock and the rare volcanic glass remain. */
#define PEAK_LINE SOS_METRES(250)

typedef struct Kind
{
    /* Chance per cell of this kind, out of 65536. */
    uint32_t chance;
    /* Surface materials it appears on, one bit per SosMaterial. */
    uint32_t materials;
    /* Surface height band, in world units, both ends inclusive. */
    int64_t minZ;
    int64_t maxZ;
    /* Amount the deposit holds at full richness. */
    uint32_t baseAmount;
    /* Ticks from empty to full; zero for a mineral that never renews. */
    uint64_t renewal;
} Kind;

static const Kind kindTable[SOS_DEPOSIT_KIND_COUNT] = {
    [SOS_DEPOSIT_FLINT] = {.chance = 3000u,
                           .materials =
                               MATERIAL_BIT(SOS_MATERIAL_GRASS) | MATERIAL_BIT(SOS_MATERIAL_SNOW),
                           .minZ = HILL_LINE,
                           .maxZ = INT64_MAX,
                           .baseAmount = 300u,
                           .renewal = 0u},
    [SOS_DEPOSIT_STONE] = {.chance = 2500u,
                           .materials =
                               MATERIAL_BIT(SOS_MATERIAL_GRASS) | MATERIAL_BIT(SOS_MATERIAL_SNOW),
                           .minZ = PEAK_LINE,
                           .maxZ = INT64_MAX,
                           .baseAmount = 1500u,
                           .renewal = 0u},
    [SOS_DEPOSIT_CLAY] = {.chance = 2500u,
                          .materials =
                              MATERIAL_BIT(SOS_MATERIAL_GRASS) | MATERIAL_BIT(SOS_MATERIAL_SAND),
                          .minZ = INT64_MIN,
                          .maxZ = NEAR_SAND_MAX,
                          .baseAmount = 600u,
                          .renewal = 0u},
    [SOS_DEPOSIT_OCHRE] = {.chance = 1500u,
                           .materials = MATERIAL_BIT(SOS_MATERIAL_GRASS),
                           .minZ = SAND_LINE_Z,
                           .maxZ = HILL_LINE,
                           .baseAmount = 200u,
                           .renewal = 0u},
    [SOS_DEPOSIT_SALT] = {.chance = 1200u,
                          .materials = MATERIAL_BIT(SOS_MATERIAL_SAND),
                          .minZ = INT64_MIN,
                          .maxZ = SAND_LINE_Z,
                          .baseAmount = 800u,
                          .renewal = 0u},
    [SOS_DEPOSIT_OBSIDIAN] = {.chance = 120u,
                              .materials = MATERIAL_BIT(SOS_MATERIAL_GRASS) |
                                           MATERIAL_BIT(SOS_MATERIAL_SNOW),
                              .minZ = PEAK_LINE,
                              .maxZ = INT64_MAX,
                              .baseAmount = 40u,
                              .renewal = 0u},
    [SOS_DEPOSIT_TIMBER] = {.chance = 8000u,
                            .materials = MATERIAL_BIT(SOS_MATERIAL_GRASS),
                            .minZ = INT64_MIN,
                            .maxZ = INT64_MAX,
                            .baseAmount = 500u,
                            .renewal = SOS_DEPOSIT_TIMBER_RENEWAL},
    [SOS_DEPOSIT_REED] = {.chance = 4000u,
                          .materials =
                              MATERIAL_BIT(SOS_MATERIAL_SAND) | MATERIAL_BIT(SOS_MATERIAL_GRASS),
                          .minZ = INT64_MIN,
                          .maxZ = NEAR_SAND_MAX,
                          .baseAmount = 300u,
                          .renewal = SOS_DEPOSIT_REED_RENEWAL},
};

/* The deposit the world function puts into a cell, before any extraction. */
typedef struct Generated
{
    SosPosition position;
    SosDepositKind kind;
    SosQ16 richness;
    uint32_t initial;
} Generated;

/* What beings took from a deposit: the amount left at a tick. A record is
 * dropped once it has grown back, so only worked deposits are remembered. */
typedef struct Extraction
{
    /* Amount the deposit held before it was first worked, for regrowth. */
    uint32_t initial;
    /* Amount left at `asOfTick`. */
    uint32_t remaining;
    uint64_t asOfTick;
    /* The deposit kind, which fixes its renewal time. */
    uint8_t kind;
} Extraction;

struct SosDeposits
{
    uint64_t seed;
    SosTable *extractions;
    /* The table size at which the next sweep drops regrown records. */
    uint32_t pruneAt;
};

/* Amount a deposit of `baseAmount` at `richness` holds when untouched. */
static uint32_t ScaledAmount(uint32_t baseAmount, SosQ16 richness)
{
    const uint32_t amount = (uint32_t)SosQ16Mul((SosQ16)baseAmount, richness);
    return amount == 0u ? 1u : amount;
}

/* The kind's properties, or NULL when the id is not a kind. */
static const Kind *KindEntry(uint32_t kind)
{
    return kind < (uint32_t)SOS_DEPOSIT_KIND_COUNT ? &kindTable[kind] : NULL;
}

static uint32_t InitialAmount(SosDepositKind kind, SosQ16 richness)
{
    const Kind *entry = KindEntry((uint32_t)kind);
    return entry != NULL ? ScaledAmount(entry->baseAmount, richness) : 0u;
}

static uint64_t RenewalOf(uint8_t kind)
{
    const Kind *entry = KindEntry(kind);
    return entry != NULL ? entry->renewal : 0u;
}

/* The amount left in a deposit at `tick`, from what was taken and the time
 * since: the plants pattern, state computed lazily from the elapsed time. */
static uint32_t CurrentAmount(uint32_t initial, uint64_t renewal, const Extraction *extraction,
                              uint64_t tick)
{
    if (extraction == NULL)
    {
        return initial;
    }
    uint32_t current = extraction->remaining > initial ? initial : extraction->remaining;
    if (renewal == 0u || tick <= extraction->asOfTick || current >= initial)
    {
        return current;
    }
    const uint64_t elapsed = tick - extraction->asOfTick;
    if (elapsed >= renewal)
    {
        return initial;
    }
    const uint64_t regrown = (uint64_t)initial * elapsed / renewal;
    const uint64_t total = (uint64_t)current + regrown;
    return total > initial ? initial : (uint32_t)total;
}

SosDeposits *SosDepositsCreate(uint64_t worldSeed)
{
    SosDeposits *deposits = calloc(1u, sizeof(*deposits));
    if (deposits == NULL)
    {
        return NULL;
    }
    deposits->seed = SosHashCombine(worldSeed, UINT64_C(0x6465706f73697473)); /* "deposits" */
    deposits->extractions = SosTableCreate(sizeof(Extraction));
    if (deposits->extractions == NULL)
    {
        free(deposits);
        return NULL;
    }
    deposits->pruneAt = PRUNE_START;
    return deposits;
}

void SosDepositsDestroy(SosDeposits *deposits)
{
    if (deposits != NULL)
    {
        SosTableDestroy(deposits->extractions);
        free(deposits);
    }
}

static const Extraction *FindExtraction(const SosDeposits *deposits, int64_t cellX, int64_t cellY)
{
    if (deposits->extractions == NULL)
    {
        return NULL;
    }
    const SosTableKey key = {.first = (uint64_t)cellX, .second = (uint64_t)cellY};
    return (const Extraction *)SosTableFind(deposits->extractions, key);
}

static bool Generate(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                     int64_t cellY, Generated *outGenerated)
{
    if (cellX <= -CELL_LIMIT || cellX >= CELL_LIMIT || cellY <= -CELL_LIMIT || cellY >= CELL_LIMIT)
    {
        return false;
    }
    const uint64_t hash =
        SosHashCombine(SosHashCombine(deposits->seed, (uint64_t)cellX), (uint64_t)cellY);
    const int64_t x = cellX * SOS_DEPOSIT_CELL_UNITS + CELL_MARGIN +
                      (int64_t)((hash >> 16) % (uint64_t)CELL_SPAN);
    const int64_t y = cellY * SOS_DEPOSIT_CELL_UNITS + CELL_MARGIN +
                      (int64_t)((hash >> 40) % (uint64_t)CELL_SPAN);
    const SosTerrainSurface surface = SosTerrainSurfaceAt(terrain, x, y);
    const uint32_t roll = (uint32_t)(hash & CHANCE_MASK);
    const uint32_t material = MATERIAL_BIT(surface.material);
    uint32_t threshold = 0u;
    SosDepositKind kind = SOS_DEPOSIT_NONE;
    for (uint32_t candidate = 1u; candidate < SOS_DEPOSIT_KIND_COUNT; ++candidate)
    {
        const Kind *entry = &kindTable[candidate];
        if ((entry->materials & material) == 0u || surface.z < entry->minZ ||
            surface.z > entry->maxZ)
        {
            continue;
        }
        threshold += entry->chance;
        if (roll < threshold)
        {
            kind = (SosDepositKind)candidate;
            break;
        }
    }
    if (kind == SOS_DEPOSIT_NONE)
    {
        return false;
    }
    /* A second hash picks the quality, so richness does not follow the place
     * in the cell or the kind. Half to full, so every deposit is worth it. */
    const uint64_t bonus = SosHashCombine(hash, UINT64_C(0x726963686e657373)); /* "richness" */
    const SosQ16 richness =
        (SosQ16)(SOS_Q16_ONE / 2 + (uint32_t)(bonus % (uint64_t)(SOS_Q16_ONE / 2 + 1)));
    *outGenerated = (Generated){
        .position = {x, y, surface.z},
        .kind = kind,
        .richness = richness,
        .initial = InitialAmount(kind, richness),
    };
    return true;
}

static bool DepositInCell(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                          int64_t cellY, uint64_t tick, SosDeposit *outDeposit)
{
    Generated generated;
    if (!Generate(deposits, terrain, cellX, cellY, &generated))
    {
        return false;
    }
    const Extraction *extraction = FindExtraction(deposits, cellX, cellY);
    const uint64_t renewal = RenewalOf((uint8_t)generated.kind);
    *outDeposit = (SosDeposit){
        .cellX = cellX,
        .cellY = cellY,
        .position = generated.position,
        .kind = generated.kind,
        .richness = generated.richness,
        .amount = CurrentAmount(generated.initial, renewal, extraction, tick),
    };
    return true;
}

bool SosDepositsInCell(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                       int64_t cellY, uint64_t tick, SosDeposit *outDeposit)
{
    if (deposits == NULL || terrain == NULL || outDeposit == NULL)
    {
        return false;
    }
    return DepositInCell(deposits, terrain, cellX, cellY, tick, outDeposit);
}

/* State of one nearest-deposit search, shared with its visitor. */
typedef struct NearestDeposit
{
    const SosDeposits *deposits;
    const SosTerrain *terrain;
    SosPosition around;
    uint64_t radiusSquared;
    uint64_t tick;
    uint32_t kindMask;
    /* The squared distance of the best deposit found, UINT64_MAX for none. */
    uint64_t best;
    SosDeposit deposit;
} NearestDeposit;

static void VisitDeposit(int64_t cellX, int64_t cellY, void *context)
{
    NearestDeposit *search = context;
    SosDeposit deposit;
    if (!DepositInCell(search->deposits, search->terrain, cellX, cellY, search->tick, &deposit) ||
        deposit.amount == 0u || (search->kindMask & SOS_DEPOSIT_KIND_MASK(deposit.kind)) == 0u)
    {
        return;
    }
    const uint64_t distance = SosCellsGroundDistanceSquared(search->around, deposit.position);
    if (distance <= search->radiusSquared && distance < search->best)
    {
        search->best = distance;
        search->deposit = deposit;
    }
}

bool SosDepositsFindNearest(const SosDeposits *deposits, const SosTerrain *terrain,
                            SosPosition around, int64_t radius, uint64_t tick, uint32_t kindMask,
                            SosDeposit *outDeposit)
{
    if (deposits == NULL || terrain == NULL || outDeposit == NULL || kindMask == 0u || radius < 0)
    {
        return false;
    }
    NearestDeposit search = {.deposits = deposits,
                             .terrain = terrain,
                             .around = around,
                             .radiusSquared = SosCellsSquared((uint64_t)radius),
                             .tick = tick,
                             .kindMask = kindMask,
                             .best = UINT64_MAX};
    if (!SosCellsVisitRings(around, radius, SOS_DEPOSIT_CELL_SHIFT, SOS_DEPOSIT_QUERY_MAX_CELLS,
                            &search.best, VisitDeposit, &search) ||
        search.best == UINT64_MAX)
    {
        return false;
    }
    *outDeposit = search.deposit;
    return true;
}

/* True for a record that has grown back to full at the sweep's tick. */
static bool DropRegrown(SosTableKey key, const void *record, void *context)
{
    (void)key;
    const Extraction *extraction = (const Extraction *)record;
    const uint64_t tick = *(const uint64_t *)context;
    const uint64_t renewal = RenewalOf(extraction->kind);
    return CurrentAmount(extraction->initial, renewal, extraction, tick) >= extraction->initial;
}

/* Drops regrown records and raises the watermark, so the table stays about
 * as large as the number of deposits actually being worked. */
static void PruneRegrown(SosDeposits *deposits, uint64_t tick)
{
    (void)SosTableRemoveIf(deposits->extractions, DropRegrown, &tick);
    const uint64_t next = (uint64_t)SosTableCount(deposits->extractions) * 2u;
    deposits->pruneAt = next < PRUNE_START
                            ? PRUNE_START
                            : (next > SOS_TABLE_MAX_COUNT ? SOS_TABLE_MAX_COUNT : (uint32_t)next);
}

uint32_t SosDepositsExtract(SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                            int64_t cellY, uint64_t tick, uint32_t amount)
{
    if (deposits == NULL || terrain == NULL || deposits->extractions == NULL || amount == 0u)
    {
        return 0u;
    }
    SosDeposit deposit;
    if (!DepositInCell(deposits, terrain, cellX, cellY, tick, &deposit) || deposit.amount == 0u)
    {
        return 0u;
    }
    const uint32_t taken = amount < deposit.amount ? amount : deposit.amount;
    if (SosTableCount(deposits->extractions) >= deposits->pruneAt)
    {
        PruneRegrown(deposits, tick);
    }
    const SosTableKey key = {.first = (uint64_t)cellX, .second = (uint64_t)cellY};
    Extraction *extraction = (Extraction *)SosTableInsert(deposits->extractions, key, NULL);
    if (extraction != NULL)
    {
        extraction->initial = InitialAmount(deposit.kind, deposit.richness);
        extraction->remaining = deposit.amount - taken;
        extraction->asOfTick = tick;
        extraction->kind = (uint8_t)deposit.kind;
    }
    return taken;
}

/* State of one box query, shared with its visitor. */
typedef struct DepositCollect
{
    const SosDeposits *deposits;
    const SosTerrain *terrain;
    uint64_t tick;
    SosDeposit *outDeposits;
    uint32_t capacity;
    uint32_t found;
} DepositCollect;

static void CollectDeposit(int64_t cellX, int64_t cellY, void *context)
{
    DepositCollect *collect = context;
    SosDeposit deposit;
    if (DepositInCell(collect->deposits, collect->terrain, cellX, cellY, collect->tick, &deposit))
    {
        if (collect->found < collect->capacity)
        {
            collect->outDeposits[collect->found] = deposit;
        }
        collect->found += 1u;
    }
}

uint32_t SosDepositsQuery(const SosDeposits *deposits, const SosTerrain *terrain, int64_t minX,
                          int64_t minY, int64_t maxX, int64_t maxY, uint64_t tick,
                          SosDeposit *outDeposits, uint32_t capacity)
{
    if (deposits == NULL || terrain == NULL || (outDeposits == NULL && capacity != 0u))
    {
        return 0u;
    }
    DepositCollect collect = {.deposits = deposits,
                              .terrain = terrain,
                              .tick = tick,
                              .outDeposits = outDeposits,
                              .capacity = capacity,
                              .found = 0u};
    SosCellsVisitBox(minX, minY, maxX, maxY, SOS_DEPOSIT_CELL_SHIFT, SOS_DEPOSIT_QUERY_MAX_CELLS,
                     CollectDeposit, &collect);
    return collect.found;
}

static uint64_t HashExtraction(SosTableKey key, const void *record, void *context)
{
    const uint64_t tick = *(const uint64_t *)context;
    const Extraction *extraction = record;
    const uint64_t renewal = RenewalOf(extraction->kind);
    const uint32_t current = CurrentAmount(extraction->initial, renewal, extraction, tick);
    if (current >= extraction->initial)
    {
        /* A record that has grown back counts as never worked. */
        return 0u;
    }
    return SosHash64(SosHashCombine(SosHashCombine(key.first, key.second), (uint64_t)current));
}

uint64_t SosDepositsHash(const SosDeposits *deposits, uint64_t tick)
{
    if (deposits == NULL || deposits->extractions == NULL)
    {
        return 0u;
    }
    return SosHashCombine(deposits->seed,
                          SosTableFold(deposits->extractions, HashExtraction, &tick));
}
