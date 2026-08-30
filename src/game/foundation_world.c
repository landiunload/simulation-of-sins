#include "game/foundation_world.h"

#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define FOUNDATION_MIN_HORIZONTAL (-12)
#define FOUNDATION_MAX_HORIZONTAL 12
#define FOUNDATION_MAX_MUTATIONS 700U

typedef struct FoundationBlockPosition
{
    int64_t x;
    int64_t y;
    int64_t z;
} FoundationBlockPosition;

typedef struct FoundationPillar
{
    FoundationBlockPosition position;
    int64_t height;
    BlockType material;
} FoundationPillar;

static bool AppendMutation(WorldBlockMutation mutations[FOUNDATION_MAX_MUTATIONS], uint32_t *count,
                           FoundationBlockPosition position, BlockType material)
{
    if (*count >= FOUNDATION_MAX_MUTATIONS)
    {
        return false;
    }

    WorldBlockMutation *mutation = &mutations[(*count)++];
    mutation->block[0] = position.x;
    mutation->block[1] = position.y;
    mutation->block[2] = position.z;
    mutation->expected = BLOCK_AIR;
    mutation->replacement = material;
    return true;
}

static bool AppendPillar(WorldBlockMutation mutations[FOUNDATION_MAX_MUTATIONS], uint32_t *count,
                         FoundationPillar pillar)
{
    for (int64_t layer = 0; layer < pillar.height; ++layer)
    {
        pillar.position.z = layer + 1;
        if (!AppendMutation(mutations, count, pillar.position, pillar.material))
        {
            return false;
        }
    }
    return true;
}

bool SimulationFoundationWorldPopulate(World *world)
{
    if (world == NULL)
    {
        return false;
    }

    WorldBlockMutation mutations[FOUNDATION_MAX_MUTATIONS];
    uint32_t count = 0;
    for (int64_t x = FOUNDATION_MIN_HORIZONTAL; x <= FOUNDATION_MAX_HORIZONTAL; ++x)
    {
        for (int64_t y = FOUNDATION_MIN_HORIZONTAL; y <= FOUNDATION_MAX_HORIZONTAL; ++y)
        {
            FoundationBlockPosition position = {x, y, 0};
            if (!AppendMutation(mutations, &count, position, SIMULATION_MATERIAL_FOUNDATION))
            {
                return false;
            }
        }
    }

    return AppendPillar(mutations, &count,
                        (FoundationPillar){{0, 7, 0}, 6, SIMULATION_MATERIAL_ACCENT}) &&
           AppendPillar(mutations, &count,
                        (FoundationPillar){{-7, 0, 0}, 4, SIMULATION_MATERIAL_MARKER}) &&
           AppendPillar(mutations, &count,
                        (FoundationPillar){{7, 0, 0}, 4, SIMULATION_MATERIAL_MARKER}) &&
           AppendPillar(mutations, &count,
                        (FoundationPillar){{0, -7, 0}, 3, SIMULATION_MATERIAL_MARKER}) &&
           WorldApplyBlockBatch(world, mutations, count);
}

bool SimulationFoundationWorldValidate(World *world)
{
    return world != NULL && WorldGetBlock(world, -12, -12, 0) == SIMULATION_MATERIAL_FOUNDATION &&
           WorldGetBlock(world, 12, 12, 0) == SIMULATION_MATERIAL_FOUNDATION &&
           WorldGetBlock(world, 13, 12, 0) == BLOCK_AIR &&
           WorldGetBlock(world, 0, 7, 6) == SIMULATION_MATERIAL_ACCENT &&
           WorldGetBlock(world, 0, 7, 7) == BLOCK_AIR &&
           WorldGetBlock(world, -7, 0, 4) == SIMULATION_MATERIAL_MARKER &&
           WorldGetBlock(world, 7, 0, 4) == SIMULATION_MATERIAL_MARKER &&
           WorldGetBlock(world, 0, -7, 3) == SIMULATION_MATERIAL_MARKER;
}
