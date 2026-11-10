#include "sim/territory.h"

#include "sim/random.h"
#include "sim/table.h"

#include <stddef.h>
#include <stdlib.h>

/* One group's hold on one cell. An empty slot has `group` zero. */
typedef struct Claim
{
    uint64_t group;
    /* The strength at `lastTick`, before any decay since. */
    SosQ16 strength;
    uint64_t lastTick;
} Claim;

/* What the claims table stores for one cell. */
typedef struct CellClaims
{
    Claim claim[SOS_TERRITORY_CLAIMS];
} CellClaims;

struct SosTerritory
{
    SosTable *cells;
};

SosTerritory *SosTerritoryCreate(void)
{
    SosTerritory *territory = calloc(1u, sizeof(*territory));
    if (territory != NULL)
    {
        territory->cells = SosTableCreate(sizeof(CellClaims));
        if (territory->cells == NULL)
        {
            free(territory);
            return NULL;
        }
    }
    return territory;
}

void SosTerritoryDestroy(SosTerritory *territory)
{
    if (territory != NULL)
    {
        SosTableDestroy(territory->cells);
        free(territory);
    }
}

/* The claim's strength at `tick`; nothing once it has faded away, and never
 * more than the saturating maximum. */
static SosQ16 Effective(const Claim *claim, uint64_t tick)
{
    const int64_t decayed =
        SosDecaySince(claim->strength, claim->lastTick, tick, SOS_TERRITORY_HALF_LIFE);
    if (decayed <= 0)
    {
        return 0;
    }
    return decayed > (int64_t)SOS_TERRITORY_MAX_STRENGTH ? SOS_TERRITORY_MAX_STRENGTH
                                                         : (SosQ16)decayed;
}

static SosQ16 Raised(SosQ16 current, SosQ16 amount)
{
    const int64_t raised = (int64_t)current + amount;
    return raised > (int64_t)SOS_TERRITORY_MAX_STRENGTH ? SOS_TERRITORY_MAX_STRENGTH
                                                        : (SosQ16)raised;
}

static SosTableKey CellKey(SosPosition position)
{
    return (SosTableKey){
        .first = (uint64_t)SosFloorShift(position.x, SOS_TERRITORY_CELL_SHIFT),
        .second = (uint64_t)SosFloorShift(position.y, SOS_TERRITORY_CELL_SHIFT),
    };
}

static const CellClaims *FindCell(const SosTerritory *territory, SosPosition position)
{
    return SosTableFind(territory->cells, CellKey(position));
}

/* Slides the held claims to the front, keeping their order, so a cell that
 * lost a slot to prune or abandon has no hole a duplicate claim could fill. */
static void Compact(CellClaims *cells)
{
    uint32_t next = 0u;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        if (cells->claim[index].group != 0u)
        {
            cells->claim[next] = cells->claim[index];
            next += 1u;
        }
    }
    for (uint32_t index = next; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        cells->claim[index] = (Claim){0};
    }
}

/* Adds to `group`'s claim in a cell. The whole cell is searched for the
 * group's own claim before any free slot is used, so a hole left by prune or
 * abandon can never grow a second claim of the same group; failing both, the
 * weakest slot is taken, and only when the newcomer is stronger. False when
 * nothing changed. */
static bool Strengthen(CellClaims *cells, uint64_t group, uint64_t tick, SosQ16 amount)
{
    uint32_t freeSlot = SOS_TERRITORY_CLAIMS;
    uint32_t weakest = 0u;
    SosQ16 weakestStrength = SOS_TERRITORY_MAX_STRENGTH;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        Claim *claim = &cells->claim[index];
        if (claim->group == group)
        {
            claim->strength = Raised(Effective(claim, tick), amount);
            claim->lastTick = tick;
            return true;
        }
        if (claim->group == 0u)
        {
            if (freeSlot == SOS_TERRITORY_CLAIMS)
            {
                freeSlot = index;
            }
            continue;
        }
        const SosQ16 strength = Effective(claim, tick);
        if (strength < weakestStrength)
        {
            weakestStrength = strength;
            weakest = index;
        }
    }
    if (freeSlot != SOS_TERRITORY_CLAIMS)
    {
        cells->claim[freeSlot] =
            (Claim){.group = group, .strength = Raised(0, amount), .lastTick = tick};
        Compact(cells);
        return true;
    }
    if (weakestStrength < amount)
    {
        cells->claim[weakest] =
            (Claim){.group = group, .strength = Raised(0, amount), .lastTick = tick};
        return true;
    }
    return false;
}

uint32_t SosTerritoryMark(SosTerritory *territory, uint64_t group, SosPosition position,
                          int64_t radius, uint64_t tick, SosQ16 amount)
{
    if (territory == NULL || group == 0u || radius < 0 || amount <= 0)
    {
        return 0u;
    }
    /* Keep the radius from running off the end of the coordinate range. */
    if (position.x < INT64_MIN + radius || position.x > INT64_MAX - radius ||
        position.y < INT64_MIN + radius || position.y > INT64_MAX - radius)
    {
        return 0u;
    }
    const int64_t firstX = SosFloorShift(position.x - radius, SOS_TERRITORY_CELL_SHIFT);
    const int64_t firstY = SosFloorShift(position.y - radius, SOS_TERRITORY_CELL_SHIFT);
    const int64_t lastX = SosFloorShift(position.x + radius, SOS_TERRITORY_CELL_SHIFT);
    const int64_t lastY = SosFloorShift(position.y + radius, SOS_TERRITORY_CELL_SHIFT);
    const int64_t cellsX = lastX - firstX + 1;
    const int64_t cellsY = lastY - firstY + 1;
    if (cellsX > SOS_TERRITORY_MARK_MAX_CELLS || cellsY > SOS_TERRITORY_MARK_MAX_CELLS ||
        cellsX * cellsY > SOS_TERRITORY_MARK_MAX_CELLS)
    {
        return 0u;
    }
    uint32_t changed = 0u;
    for (int64_t cellY = firstY; cellY <= lastY; ++cellY)
    {
        for (int64_t cellX = firstX; cellX <= lastX; ++cellX)
        {
            const SosTableKey key = {.first = (uint64_t)cellX, .second = (uint64_t)cellY};
            CellClaims *cells = SosTableInsert(territory->cells, key, NULL);
            if (cells == NULL)
            {
                return changed;
            }
            if (Strengthen(cells, group, tick, amount))
            {
                changed += 1u;
            }
        }
    }
    return changed;
}

bool SosTerritoryOwner(const SosTerritory *territory, SosPosition position, uint64_t tick,
                       uint64_t *outGroup, SosQ16 *outStrength)
{
    if (territory == NULL)
    {
        return false;
    }
    const CellClaims *cells = FindCell(territory, position);
    if (cells == NULL)
    {
        return false;
    }
    bool found = false;
    uint64_t bestGroup = 0u;
    SosQ16 bestStrength = 0;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        const Claim *claim = &cells->claim[index];
        if (claim->group == 0u)
        {
            continue;
        }
        const SosQ16 strength = Effective(claim, tick);
        if (!found || strength > bestStrength ||
            (strength == bestStrength && claim->group < bestGroup))
        {
            found = true;
            bestStrength = strength;
            bestGroup = claim->group;
        }
    }
    if (!found || bestStrength <= SOS_TERRITORY_OWNER_THRESHOLD)
    {
        return false;
    }
    if (outGroup != NULL)
    {
        *outGroup = bestGroup;
    }
    if (outStrength != NULL)
    {
        *outStrength = bestStrength;
    }
    return true;
}

bool SosTerritoryContested(const SosTerritory *territory, SosPosition position, uint64_t tick)
{
    if (territory == NULL)
    {
        return false;
    }
    const CellClaims *cells = FindCell(territory, position);
    if (cells == NULL)
    {
        return false;
    }
    SosQ16 first = 0;
    SosQ16 second = 0;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        const Claim *claim = &cells->claim[index];
        if (claim->group == 0u)
        {
            continue;
        }
        const SosQ16 strength = Effective(claim, tick);
        if (strength > first)
        {
            second = first;
            first = strength;
        }
        else if (strength > second)
        {
            second = strength;
        }
    }
    if (first <= 0 || second <= 0)
    {
        return false;
    }
    return (int64_t)second * SOS_Q16_ONE >= (int64_t)first * SOS_TERRITORY_CONTEST_RATIO;
}

/* True for a cell whose every claim slot is empty. */
static bool CellEmpty(SosTableKey key, const void *record, void *context)
{
    (void)key;
    (void)context;
    const CellClaims *cells = record;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        if (cells->claim[index].group != 0u)
        {
            return false;
        }
    }
    return true;
}

uint32_t SosTerritoryAbandon(SosTerritory *territory, uint64_t group)
{
    if (territory == NULL || group == 0u)
    {
        return 0u;
    }
    uint32_t removed = 0u;
    const uint32_t slots = SosTableSlotCount(territory->cells);
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        SosTableKey key;
        if (!SosTableSlot(territory->cells, slot, &key, NULL))
        {
            continue;
        }
        CellClaims *cells = SosTableEdit(territory->cells, key);
        if (cells == NULL)
        {
            continue;
        }
        bool changed = false;
        for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
        {
            if (cells->claim[index].group == group)
            {
                cells->claim[index] = (Claim){0};
                removed += 1u;
                changed = true;
            }
        }
        if (changed)
        {
            Compact(cells);
        }
    }
    if (removed != 0u)
    {
        (void)SosTableRemoveIf(territory->cells, CellEmpty, NULL);
    }
    return removed;
}

uint32_t SosTerritoryPrune(SosTerritory *territory, uint64_t tick)
{
    if (territory == NULL)
    {
        return 0u;
    }
    uint32_t removed = 0u;
    const uint32_t slots = SosTableSlotCount(territory->cells);
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        SosTableKey key;
        if (!SosTableSlot(territory->cells, slot, &key, NULL))
        {
            continue;
        }
        CellClaims *cells = SosTableEdit(territory->cells, key);
        if (cells == NULL)
        {
            continue;
        }
        bool changed = false;
        for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
        {
            Claim *claim = &cells->claim[index];
            if (claim->group != 0u && Effective(claim, tick) < SOS_TERRITORY_MIN_STRENGTH)
            {
                *claim = (Claim){0};
                removed += 1u;
                changed = true;
            }
        }
        if (changed)
        {
            Compact(cells);
        }
    }
    if (removed != 0u)
    {
        (void)SosTableRemoveIf(territory->cells, CellEmpty, NULL);
    }
    return removed;
}

static uint64_t HashClaims(SosTableKey key, const void *record, void *context)
{
    const uint64_t tick = *(const uint64_t *)context;
    const CellClaims *cells = record;
    uint64_t sum = 0u;
    for (uint32_t index = 0u; index < SOS_TERRITORY_CLAIMS; ++index)
    {
        const Claim *claim = &cells->claim[index];
        if (claim->group == 0u)
        {
            continue;
        }
        const SosQ16 strength = Effective(claim, tick);
        if (strength <= 0)
        {
            continue;
        }
        uint64_t claimHash = SosHashCombine(SosHash64(key.first), key.second);
        claimHash = SosHashCombine(claimHash, claim->group);
        sum += SosHashCombine(claimHash, (uint64_t)(uint32_t)strength);
    }
    return sum;
}

uint64_t SosTerritoryHash(const SosTerritory *territory, uint64_t tick)
{
    if (territory == NULL)
    {
        return 0u;
    }
    return SosTableFold(territory->cells, HashClaims, &tick);
}
