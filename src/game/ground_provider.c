#include "game/ground_provider.h"

#include "game/foundation_world.h"

#include <stddef.h>
#include <string.h>

// Локальная координата плюс абсолютная координата локального нуля. Ни то
// ни другое движок не ограничивает, поэтому сумма проверяется: молчаливое
// переполнение вернуло бы пол не там, где он есть.
static bool AddWithoutOverflow(int64_t left, int64_t right, int64_t *outValue)
{
    if (right > 0 && left > INT64_MAX - right)
    {
        return false;
    }
    if (right < 0 && left < INT64_MIN - right)
    {
        return false;
    }
    *outValue = left + right;
    return true;
}

static BlockType GroundBlockAtLocalZ(const SimulationGroundProvider *ground, int64_t z)
{
    int64_t absoluteZ = 0;
    if (!AddWithoutOverflow(z, ground->originBlock[2], &absoluteZ))
    {
        // За пределами представимых координат пола нет: воздух — это
        // отказ, который ничего не ломает.
        return BLOCK_AIR;
    }
    return absoluteZ == SIMULATION_GROUND_LEVEL ? (BlockType)SIMULATION_MATERIAL_FOUNDATION
                                                : (BlockType)BLOCK_AIR;
}

static BlockType GroundGetBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    // Пол ровный, поэтому от X и Y не зависит ничего.
    (void)x;
    (void)y;
    return GroundBlockAtLocalZ((const SimulationGroundProvider *)context, z);
}

static WorldRegionContents GroundFillRegion(void *context, int64_t minBlockX, int64_t minBlockY,
                                            int64_t minBlockZ, int32_t sizeX, int32_t sizeY,
                                            int32_t sizeZ, BlockType *outBlocks)
{
    const SimulationGroundProvider *ground = (const SimulationGroundProvider *)context;
    (void)minBlockX;
    (void)minBlockY;
    if (outBlocks == NULL || sizeX <= 0 || sizeY <= 0 || sizeZ <= 0)
    {
        return WORLD_REGION_ALL_AIR;
    }

    // Раскладка ((y * sizeX) + x) * sizeZ + z: Z идёт подряд. Пол зависит
    // только от Z, поэтому колонка считается один раз и копируется —
    // регион чанка это одна и та же колонка, повторённая 64x64 раза.
    BlockType *column = outBlocks;
    bool sawSolid = false;
    bool sawAir = false;
    for (int32_t z = 0; z < sizeZ; ++z)
    {
        BlockType block = GroundBlockAtLocalZ(ground, minBlockZ + z);
        column[z] = block;
        sawSolid = sawSolid || block != BLOCK_AIR;
        sawAir = sawAir || block == BLOCK_AIR;
    }

    size_t columnBytes = (size_t)sizeZ * sizeof(BlockType);
    size_t columnCount = (size_t)sizeX * (size_t)sizeY;
    for (size_t index = 1; index < columnCount; ++index)
    {
        memcpy(outBlocks + index * (size_t)sizeZ, column, columnBytes);
    }

    // Движок заполненный буфер использует всегда, а сводку — как подсказку.
    if (!sawSolid)
    {
        return WORLD_REGION_ALL_AIR;
    }
    return sawAir ? WORLD_REGION_MIXED : WORLD_REGION_ALL_SOLID;
}

static bool GroundRebase(void *context, int64_t blockShiftX, int64_t blockShiftY,
                         int64_t blockShiftZ)
{
    SimulationGroundProvider *ground = (SimulationGroundProvider *)context;
    const int64_t shift[3] = {blockShiftX, blockShiftY, blockShiftZ};

    // Локальные координаты уменьшаются на shift, значит абсолютная
    // координата локального нуля на столько же растёт.
    int64_t shifted[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!AddWithoutOverflow(ground->originBlock[axis], shift[axis], &shifted[axis]))
        {
            // Отказ обязан оставить контекст нетронутым: движок отменит
            // весь rebase целиком.
            return false;
        }
    }
    memcpy(ground->originBlock, shifted, sizeof(shifted));
    return true;
}

void SimulationGroundProviderInit(SimulationGroundProvider *ground)
{
    if (ground == NULL)
    {
        return;
    }
    memset(ground, 0, sizeof(*ground));
}

void SimulationGroundProviderBind(SimulationGroundProvider *ground, WorldBaseProvider *outProvider)
{
    if (ground == NULL || outProvider == NULL)
    {
        return;
    }
    outProvider->context = ground;
    outProvider->getBlock = GroundGetBlock;
    outProvider->fillRegion = GroundFillRegion;
    outProvider->rebase = GroundRebase;
}

int64_t SimulationGroundLocalLevel(const SimulationGroundProvider *ground)
{
    if (ground == NULL)
    {
        return SIMULATION_GROUND_LEVEL;
    }
    return SIMULATION_GROUND_LEVEL - ground->originBlock[2];
}
