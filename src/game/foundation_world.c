#include "game/foundation_world.h"

#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

// Пол больше не выкладывается блоками: он бесконечен и приходит базовым
// слоем мира (game/ground_provider.h). Здесь остаётся только то, что
// вычислением не выражается — авторские ориентиры поверх пола.
#define FOUNDATION_MAX_MUTATIONS 32U

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
    // Пол проверяется там, куда никакой batch мутаций не дотягивался: его
    // конечность была бы видна именно здесь. Над полом и под ним — воздух,
    // иначе слой был бы толще одного блока.
    return world != NULL && WorldGetBlock(world, -12, -12, 0) == SIMULATION_MATERIAL_FOUNDATION &&
           WorldGetBlock(world, 12, 12, 0) == SIMULATION_MATERIAL_FOUNDATION &&
           WorldGetBlock(world, 1000000, -1000000, 0) == SIMULATION_MATERIAL_FOUNDATION &&
           WorldGetBlock(world, 1000000, -1000000, 1) == BLOCK_AIR &&
           WorldGetBlock(world, 1000000, -1000000, -1) == BLOCK_AIR &&
           WorldGetBlock(world, 0, 7, 6) == SIMULATION_MATERIAL_ACCENT &&
           WorldGetBlock(world, 0, 7, 7) == BLOCK_AIR &&
           WorldGetBlock(world, -7, 0, 4) == SIMULATION_MATERIAL_MARKER &&
           WorldGetBlock(world, 7, 0, 4) == SIMULATION_MATERIAL_MARKER &&
           WorldGetBlock(world, 0, -7, 3) == SIMULATION_MATERIAL_MARKER;
}
