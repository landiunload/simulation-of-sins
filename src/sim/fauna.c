#include "sim/fauna.h"

#include "sim/calendar.h"
#include "sim/cells.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stddef.h>
#include <stdlib.h>

/* A herd keeps half a metre of margin from its waypoint grid, matching the
 * waypoint draw. */
#define HERD_RANGE SOS_FAUNA_HERD_RANGE
#define CHANCE_MASK (SOS_FAUNA_CHANCE - 1u)
/* Streams of a herd's own randomness, so its home and its waypoints are drawn
 * from a stream rather than by masking shared hash bits. */
#define HOME_STREAM UINT64_C(0x686f6d65)     /* "home" */
#define WAYPOINT_STREAM UINT64_C(0x77617970) /* "wayp" */

static const SosFaunaSpeciesInfo speciesTable[SOS_FAUNA_SPECIES_COUNT] = {
    /* Small, common, harmless game: rabbits swarm meadows and come back fast. */
    [SOS_FAUNA_RABBIT] = {.ground = SOS_MATERIAL_GRASS,
                          .density = 8192u,
                          .minimumSize = 3u,
                          .maximumSize = 12u,
                          .meat = 2u,
                          .danger = 0,
                          .fleeDistance = SOS_METRES(16),
                          .regrowth = 10 * SOS_TICKS_PER_DAY},
    /* Deer browse the same meadows, fewer and worth more. */
    [SOS_FAUNA_DEER] = {.ground = SOS_MATERIAL_GRASS,
                        .density = 6144u,
                        .minimumSize = 2u,
                        .maximumSize = 8u,
                        .meat = 20u,
                        .danger = 0,
                        .fleeDistance = SOS_METRES(24),
                        .regrowth = 20 * SOS_TICKS_PER_DAY},
    /* Boar are aggressive when cornered, so a hunt costs health. */
    [SOS_FAUNA_BOAR] = {.ground = SOS_MATERIAL_GRASS,
                        .density = 4096u,
                        .minimumSize = 2u,
                        .maximumSize = 6u,
                        .meat = 30u,
                        .danger = SOS_Q16_ONE / 2,
                        .fleeDistance = SOS_METRES(12),
                        .regrowth = 25 * SOS_TICKS_PER_DAY},
    /* Aurochs are the great prize and take long to return. */
    [SOS_FAUNA_AUROCHS] = {.ground = SOS_MATERIAL_GRASS,
                           .density = 2048u,
                           .minimumSize = 2u,
                           .maximumSize = 5u,
                           .meat = 60u,
                           .danger = 3 * SOS_Q16_ONE / 4,
                           .fleeDistance = SOS_METRES(14),
                           .regrowth = 40 * SOS_TICKS_PER_DAY},
    /* Wolves hunt on the snow line and are the most dangerous game. */
    [SOS_FAUNA_WOLF] = {.ground = SOS_MATERIAL_SNOW,
                        .density = 2048u,
                        .minimumSize = 3u,
                        .maximumSize = 7u,
                        .meat = 5u,
                        .danger = 9 * SOS_Q16_ONE / 10,
                        .fleeDistance = SOS_METRES(30),
                        .regrowth = 30 * SOS_TICKS_PER_DAY},
    /* A bear is a fight nobody wants and a larder nobody forgets. */
    [SOS_FAUNA_BEAR] = {.ground = SOS_MATERIAL_GRASS,
                        .density = 1024u,
                        .minimumSize = 1u,
                        .maximumSize = 2u,
                        .meat = 80u,
                        .danger = SOS_Q16_ONE,
                        .fleeDistance = SOS_METRES(8),
                        .regrowth = 60 * SOS_TICKS_PER_DAY},
};

/* The animals taken from a herd. `span` is the species' regrowth, kept in the
 * record so a loss can be forgotten without the terrain. An empty slot is none
 * of a table's business; the slot only exists while a herd is short. */
typedef struct Loss
{
    uint64_t lostTick;
    uint64_t recoverTick;
    uint32_t lost;
} Loss;

struct SosFauna
{
    uint64_t seed;
    /* One record per hunted cell; absent until a herd is hunted. */
    SosTable *losses;
};

static bool CellInRange(int64_t cellX, int64_t cellY)
{
    /* A cell's corner must not overflow the signed position range. */
    const int64_t limit = INT64_MAX / SOS_FAUNA_CELL_UNITS - 1;
    return cellX >= -limit && cellX <= limit && cellY >= -limit && cellY <= limit;
}

SosFauna *SosFaunaCreate(uint64_t worldSeed)
{
    SosFauna *fauna = calloc(1u, sizeof(*fauna));
    if (fauna == NULL)
    {
        return NULL;
    }
    fauna->seed = SosHashCombine(worldSeed, UINT64_C(0x6661756e61)); /* "fauna" */
    fauna->losses = SosTableCreate(sizeof(Loss));
    if (fauna->losses == NULL)
    {
        free(fauna);
        return NULL;
    }
    return fauna;
}

void SosFaunaDestroy(SosFauna *fauna)
{
    if (fauna != NULL)
    {
        SosTableDestroy(fauna->losses);
        free(fauna);
    }
}

bool SosFaunaSpeciesInfoGet(SosFaunaSpecies species, SosFaunaSpeciesInfo *outInfo)
{
    const int index = (int)species;
    if (outInfo == NULL || index <= (int)SOS_FAUNA_NONE || index >= (int)SOS_FAUNA_SPECIES_COUNT)
    {
        return false;
    }
    *outInfo = speciesTable[index];
    return true;
}

/* The herd the world function puts into a cell, before any hunt. */
static bool Generate(const SosFauna *fauna, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                     SosHerd *outHerd)
{
    const uint64_t hash =
        SosHashCombine(SosHashCombine(fauna->seed, (uint64_t)cellX), (uint64_t)cellY);
    const uint32_t roll = (uint32_t)(hash & CHANCE_MASK);
    uint32_t threshold = 0u;
    SosFaunaSpecies species = SOS_FAUNA_NONE;
    for (uint32_t candidate = 1u; candidate < (uint32_t)SOS_FAUNA_SPECIES_COUNT; ++candidate)
    {
        threshold += speciesTable[candidate].density;
        if (roll < threshold)
        {
            species = (SosFaunaSpecies)candidate;
            break;
        }
    }
    if (species == SOS_FAUNA_NONE)
    {
        return false;
    }
    const int64_t span = SOS_FAUNA_CELL_UNITS - 2 * HERD_RANGE;
    /* Draw each offset over the whole span the herd may roam, so homes cover
     * the cell instead of a masked sliver of it. */
    SosRandom random = SosRandomStream(hash, HOME_STREAM);
    const int64_t homeX =
        cellX * SOS_FAUNA_CELL_UNITS + HERD_RANGE + SosRandomRange(&random, 0, span - 1);
    const int64_t homeY =
        cellY * SOS_FAUNA_CELL_UNITS + HERD_RANGE + SosRandomRange(&random, 0, span - 1);
    const SosTerrainSurface surface = SosTerrainSurfaceAt(terrain, homeX, homeY);
    if (surface.material != speciesTable[species].ground)
    {
        return false;
    }
    const uint32_t minimum = speciesTable[species].minimumSize;
    const uint32_t maximum = speciesTable[species].maximumSize;
    const uint32_t size = minimum + (uint32_t)((hash >> 48u) % (uint64_t)(maximum - minimum + 1u));
    *outHerd = (SosHerd){.cellX = cellX,
                         .cellY = cellY,
                         .home = {homeX, homeY, surface.z},
                         .species = species,
                         .size = size,
                         .alive = size};
    return true;
}

static const Loss *FindLoss(const SosFauna *fauna, int64_t cellX, int64_t cellY)
{
    const SosTableKey key = {.first = (uint64_t)cellX, .second = (uint64_t)cellY};
    return (const Loss *)SosTableFind(fauna->losses, key);
}

/* How many animals the record still owes the herd at `tick`. Before the hunt
 * the herd was whole and after it healed it is whole again; in between they
 * come back at an even pace, so the count never jumps. */
static uint32_t LossMissing(const Loss *loss, uint64_t tick)
{
    if (tick < loss->lostTick || tick >= loss->recoverTick)
    {
        return 0u;
    }
    if (tick == loss->lostTick)
    {
        return loss->lost;
    }
    const uint64_t elapsed = tick - loss->lostTick;
    const uint64_t span = loss->recoverTick - loss->lostTick;
    return (uint32_t)((uint64_t)loss->lost - (uint64_t)loss->lost * elapsed / span);
}

static void HerdInCell(const SosFauna *fauna, const SosTerrain *terrain, int64_t cellX,
                       int64_t cellY, uint64_t tick, SosHerd *outHerd)
{
    if (!CellInRange(cellX, cellY) || !Generate(fauna, terrain, cellX, cellY, outHerd))
    {
        outHerd->species = SOS_FAUNA_NONE;
        return;
    }
    const Loss *loss = FindLoss(fauna, cellX, cellY);
    if (loss != NULL)
    {
        const uint32_t missing = LossMissing(loss, tick);
        outHerd->alive = missing < outHerd->size ? outHerd->size - missing : 0u;
    }
}

bool SosFaunaInCell(const SosFauna *fauna, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                    uint64_t tick, SosHerd *outHerd)
{
    if (fauna == NULL || terrain == NULL || outHerd == NULL)
    {
        return false;
    }
    HerdInCell(fauna, terrain, cellX, cellY, tick, outHerd);
    return outHerd->species != SOS_FAUNA_NONE;
}

/* A daily waypoint of a herd: a hashed offset from its home. The home already
 * follows from the seed, so equal seeds roam equally without storing the
 * path. */
static SosPosition Waypoint(const SosHerd *herd, uint64_t day)
{
    const uint64_t seed = SosHashCombine(
        SosHash64(day), SosHashCombine((uint64_t)herd->home.x, (uint64_t)herd->home.y));
    /* A symmetric draw over the whole range, so herds roam in every direction
     * and their average offset is home. */
    SosRandom random = SosRandomStream(seed, WAYPOINT_STREAM);
    const int64_t offsetX = SosRandomRange(&random, -HERD_RANGE, HERD_RANGE);
    const int64_t offsetY = SosRandomRange(&random, -HERD_RANGE, HERD_RANGE);
    return (SosPosition){herd->home.x + offsetX, herd->home.y + offsetY, herd->home.z};
}

SosPosition SosFaunaHerdPosition(const SosHerd *herd, const SosTerrain *terrain, uint64_t tick)
{
    if (herd == NULL || terrain == NULL || !CellInRange(herd->cellX, herd->cellY))
    {
        return (SosPosition){0, 0, 0};
    }
    const uint64_t day = tick / (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t inDay = tick % (uint64_t)SOS_TICKS_PER_DAY;
    const SosPosition from = Waypoint(herd, day);
    const SosPosition to = Waypoint(herd, day + 1u);
    const int64_t fraction = (int64_t)(inDay * (uint64_t)SOS_Q16_ONE / (uint64_t)SOS_TICKS_PER_DAY);
    const int64_t x = from.x + (to.x - from.x) * fraction / SOS_Q16_ONE;
    const int64_t y = from.y + (to.y - from.y) * fraction / SOS_Q16_ONE;
    return (SosPosition){x, y, SosTerrainSurfaceZ(terrain, x, y)};
}

/* State of one nearest-herd search, shared with its visitor. */
typedef struct NearestHerd
{
    const SosFauna *fauna;
    const SosTerrain *terrain;
    SosPosition around;
    uint64_t radiusSquared;
    uint64_t tick;
    int filter;
    /* The squared distance of the best herd found, UINT64_MAX for none. */
    uint64_t best;
    SosHerd herd;
} NearestHerd;

static void VisitHerd(int64_t cellX, int64_t cellY, void *context)
{
    NearestHerd *search = context;
    SosHerd herd;
    HerdInCell(search->fauna, search->terrain, cellX, cellY, search->tick, &herd);
    if (herd.species == SOS_FAUNA_NONE || herd.alive == 0u ||
        (search->filter != (int)SOS_FAUNA_NONE && (int)herd.species != search->filter))
    {
        return;
    }
    const SosPosition position = SosFaunaHerdPosition(&herd, search->terrain, search->tick);
    const uint64_t distance = SosCellsGroundDistanceSquared(search->around, position);
    if (distance <= search->radiusSquared && distance < search->best)
    {
        search->best = distance;
        search->herd = herd;
    }
}

bool SosFaunaFindNearest(const SosFauna *fauna, const SosTerrain *terrain, SosPosition around,
                         int64_t radius, SosFaunaSpecies speciesFilter, uint64_t tick,
                         SosHerd *outHerd)
{
    const int filter = (int)speciesFilter;
    if (fauna == NULL || terrain == NULL || outHerd == NULL || radius < 0 ||
        filter < (int)SOS_FAUNA_NONE || filter >= (int)SOS_FAUNA_SPECIES_COUNT)
    {
        return false;
    }
    NearestHerd search = {.fauna = fauna,
                          .terrain = terrain,
                          .around = around,
                          .radiusSquared = SosCellsSquared((uint64_t)radius),
                          .tick = tick,
                          .filter = filter,
                          .best = UINT64_MAX};
    if (!SosCellsVisitRings(around, radius, SOS_FAUNA_CELL_SHIFT, SOS_FAUNA_QUERY_MAX_CELLS,
                            &search.best, VisitHerd, &search) ||
        search.best == UINT64_MAX)
    {
        return false;
    }
    *outHerd = search.herd;
    return true;
}

/* State of one box query, shared with its visitor. */
typedef struct HerdCollect
{
    const SosFauna *fauna;
    const SosTerrain *terrain;
    uint64_t tick;
    SosHerd *outHerds;
    uint32_t capacity;
    uint32_t found;
} HerdCollect;

static void CollectHerd(int64_t cellX, int64_t cellY, void *context)
{
    HerdCollect *collect = context;
    SosHerd herd;
    HerdInCell(collect->fauna, collect->terrain, cellX, cellY, collect->tick, &herd);
    if (herd.species != SOS_FAUNA_NONE)
    {
        if (collect->found < collect->capacity)
        {
            collect->outHerds[collect->found] = herd;
        }
        collect->found += 1u;
    }
}

uint32_t SosFaunaQuery(const SosFauna *fauna, const SosTerrain *terrain, int64_t minX, int64_t minY,
                       int64_t maxX, int64_t maxY, uint64_t tick, SosHerd *outHerds,
                       uint32_t capacity)
{
    if (fauna == NULL || terrain == NULL || (outHerds == NULL && capacity != 0u))
    {
        return 0u;
    }
    HerdCollect collect = {.fauna = fauna,
                           .terrain = terrain,
                           .tick = tick,
                           .outHerds = outHerds,
                           .capacity = capacity,
                           .found = 0u};
    SosCellsVisitBox(minX, minY, maxX, maxY, SOS_FAUNA_CELL_SHIFT, SOS_FAUNA_QUERY_MAX_CELLS,
                     CollectHerd, &collect);
    return collect.found;
}

/* True when the loss is whole again, so the herd looks untouched and the
 * record has nothing left to say. */
static bool RecoveredLoss(SosTableKey key, const void *record, void *context)
{
    (void)key;
    const Loss *loss = record;
    const uint64_t tick = *(const uint64_t *)context;
    return tick >= loss->recoverTick;
}

static void Remember(SosFauna *fauna, int64_t cellX, int64_t cellY, uint64_t tick, uint32_t lost,
                     SosFaunaSpecies species)
{
    const SosTableKey key = {.first = (uint64_t)cellX, .second = (uint64_t)cellY};
    Loss *loss = SosTableInsert(fauna->losses, key, NULL);
    if (loss == NULL)
    {
        /* Out of memory: the animals are taken but not remembered, so the
         * world loses detail, never animals. */
        return;
    }
    const uint64_t span = (uint64_t)speciesTable[species].regrowth;
    loss->lost = lost;
    loss->lostTick = tick;
    loss->recoverTick = tick > UINT64_MAX - span ? UINT64_MAX : tick + span;
}

uint32_t SosFaunaHunt(SosFauna *fauna, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                      uint64_t tick, uint32_t count)
{
    if (fauna == NULL || terrain == NULL || count == 0u)
    {
        return 0u;
    }
    /* Hunting is the only thing that grows the layer, so the herds that have
     * healed are forgotten here, keeping it proportional to what is hunted. */
    (void)SosTableRemoveIf(fauna->losses, RecoveredLoss, &tick);
    SosHerd herd;
    HerdInCell(fauna, terrain, cellX, cellY, tick, &herd);
    if (herd.species == SOS_FAUNA_NONE || herd.alive == 0u)
    {
        return 0u;
    }
    const uint32_t taken = count < herd.alive ? count : herd.alive;
    const uint32_t missing = herd.size - herd.alive + taken;
    Remember(fauna, cellX, cellY, tick, missing, herd.species);
    return taken;
}

static uint64_t HashLoss(SosTableKey key, const void *record, void *context)
{
    const uint64_t tick = *(const uint64_t *)context;
    const uint32_t missing = LossMissing((const Loss *)record, tick);
    if (missing == 0u)
    {
        return 0u;
    }
    return SosHash64(SosHashCombine(SosHashCombine(SosHash64(key.first), key.second), missing));
}

uint64_t SosFaunaHash(const SosFauna *fauna, uint64_t tick)
{
    if (fauna == NULL)
    {
        return 0u;
    }
    return SosHashCombine(fauna->seed, SosTableFold(fauna->losses, HashLoss, &tick));
}
