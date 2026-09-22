#include "game/construct.h"
#include "game/falling_cubes.h"
#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/ground_provider.h"
#include "game/physics_defaults.h"
#include "game/rebase_policy.h"

#include "world/world.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define EXPECT(condition)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fputs(__FILE__ ": expectation failed: " #condition "\n", stderr);                      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static World *CreateGroundWorld(SimulationGroundProvider *ground)
{
    SimulationGroundProviderInit(ground);
    WorldBaseProvider provider;
    SimulationGroundProviderBind(ground, &provider);
    return WorldCreate(&provider);
}

static void TestFoundationWorld(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    uint64_t initialRevision = WorldGetRevision(world);
    EXPECT(SimulationFoundationWorldPopulate(world));
    EXPECT(SimulationFoundationWorldValidate(world));
    uint64_t populatedRevision = WorldGetRevision(world);
    EXPECT(populatedRevision > initialRevision);
    EXPECT(!SimulationFoundationWorldPopulate(world));
    EXPECT(WorldGetRevision(world) == populatedRevision);
    WorldDestroy(world);
}

static void TestInfiniteGround(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    // Пол есть там, куда никакой batch мутаций не дотянулся бы.
    EXPECT(WorldGetBlock(world, INT64_C(4000000000), INT64_C(-4000000000), 0) ==
           SIMULATION_MATERIAL_FOUNDATION);
    EXPECT(WorldGetBlock(world, INT64_C(4000000000), INT64_C(-4000000000), 1) == BLOCK_AIR);
    EXPECT(WorldGetBlock(world, INT64_C(4000000000), INT64_C(-4000000000), -1) == BLOCK_AIR);

    // Два пути провайдера обязаны отвечать одинаково: fillRegion — это
    // отдельная реализация, и разойтись с getBlock она может молча.
    BlockType region[2 * 2 * 4];
    WorldRegionContents contents = WorldFillRegion(world, -1, -1, -2, 2, 2, 4, region);
    EXPECT(contents == WORLD_REGION_MIXED);
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 2; ++x)
        {
            for (int32_t z = 0; z < 4; ++z)
            {
                size_t index = (((size_t)y * 2u) + (size_t)x) * 4u + (size_t)z;
                EXPECT(region[index] == WorldGetBlock(world, -1 + x, -1 + y, -2 + z));
            }
        }
    }

    // Регион целиком выше пола — воздух, и буфер обязан быть заполнен.
    region[0] = SIMULATION_MATERIAL_MARKER;
    EXPECT(WorldFillRegion(world, 0, 0, 40, 2, 2, 4, region) == WORLD_REGION_ALL_AIR);
    EXPECT(region[0] == BLOCK_AIR);

    // Свой блок поверх базового слоя главнее пола.
    EXPECT(WorldTrySetBlock(world, 5, 5, 0, SIMULATION_MATERIAL_MARKER));
    EXPECT(WorldGetBlock(world, 5, 5, 0) == SIMULATION_MATERIAL_MARKER);

    // Rebasing уводит локальную сетку, пол остаётся на своей абсолютной
    // высоте: иначе он ездил бы вместе с камерой.
    EXPECT(WorldRebase(world, 0, 0, 64));
    EXPECT(ground.originBlock[2] == 64);
    EXPECT(WorldGetBlock(world, 0, 0, -64) == SIMULATION_MATERIAL_FOUNDATION);
    EXPECT(WorldGetBlock(world, 0, 0, 0) == BLOCK_AIR);
    EXPECT(SimulationGroundLocalLevel(&ground) == -64);

    WorldDestroy(world);
}

// Побайтовое сравнение региона с поячеечным getBlock. Регион читают мешер
// и физика, а getBlock — точечные запросы; оптимизация заполнения может
// разойтись с ними молча, поэтому каждый регион сверяется целиком, а не
// только по сводке.
static void ExpectRegionMatchesCells(World *world, int64_t minX, int64_t minY, int64_t minZ,
                                     int32_t sizeX, int32_t sizeY, int32_t sizeZ)
{
    BlockType region[8 * 7 * 9];
    // Длина берётся из sizeof самого массива, выйти за него нечем;
    // Annex K (memset_s) в этом окружении недоступен.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(region, 0xAB, sizeof(region));
    WorldRegionContents contents =
        WorldFillRegion(world, minX, minY, minZ, sizeX, sizeY, sizeZ, region);
    bool sawSolid = false;
    bool sawAir = false;
    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                size_t index =
                    (((size_t)y * (size_t)sizeX) + (size_t)x) * (size_t)sizeZ + (size_t)z;
                BlockType expected = WorldGetBlock(world, minX + x, minY + y, minZ + z);
                EXPECT(region[index] == expected);
                sawSolid = sawSolid || expected != BLOCK_AIR;
                sawAir = sawAir || expected == BLOCK_AIR;
            }
        }
    }
    WorldRegionContents expectedContents =
        !sawSolid ? WORLD_REGION_ALL_AIR : (sawAir ? WORLD_REGION_MIXED : WORLD_REGION_ALL_SOLID);
    EXPECT(contents == expectedContents);
}

// Регионы вокруг пола: полностью воздушные, ровно слой пола, пол по краю
// и нечётные размеры, гоняющие все ветки удвоения копирования.
static void TestGroundFillRegionEquivalence(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    for (int64_t z = -6; z <= 6; ++z)
    {
        for (int32_t sizeZ = 1; sizeZ <= 5; ++sizeZ)
        {
            ExpectRegionMatchesCells(world, -3, -2, z, 3, 5, sizeZ);
        }
    }
    ExpectRegionMatchesCells(world, 0, 0, -2, 5, 3, 7);
    ExpectRegionMatchesCells(world, 0, 0, 0, 7, 3, 1);
    ExpectRegionMatchesCells(world, 0, 0, 1, 1, 1, 9);
    ExpectRegionMatchesCells(world, 0, 0, -1, 2, 2, 2);

    // Случайные регионы до и после сдвига начала координат: локальный пол
    // переезжает, а содержимое региона меняться не должно.
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    for (uint32_t round = 0; round < 400; ++round)
    {
        const int64_t level = SimulationGroundLocalLevel(&ground);
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int32_t sizeX = 1 + (int32_t)((state >> 33) % 8u);
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int32_t sizeY = 1 + (int32_t)((state >> 33) % 7u);
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int32_t sizeZ = 1 + (int32_t)((state >> 33) % 9u);
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int64_t minX = (int64_t)((state >> 33) % 41u) - 20;
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int64_t minY = (int64_t)((state >> 33) % 41u) - 20;
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        int64_t minZ = level + (int64_t)((state >> 33) % 25u) - 12;
        ExpectRegionMatchesCells(world, minX, minY, minZ, sizeX, sizeY, sizeZ);
        if (round == 200u)
        {
            EXPECT(WorldRebase(world, 0, 0, 64));
        }
    }

    WorldDestroy(world);
}

// Кубы теперь настоящие твёрдые тела: они кувыркаются, ложатся на грань
// или на ребро и разгоняются без потолка. Проверяется поведение, а не
// возвращаемые коды.

static double LowestCubeZ(const SimulationCubeField *field)
{
    double lowest = INFINITY;
    for (uint32_t index = 0; index < SimulationCubeFieldCount(field); ++index)
    {
        double origin[3];
        float rotation[4];
        if (!SimulationCubeFieldPlacement(field, index, origin, rotation))
        {
            continue;
        }
        if (origin[2] < lowest)
        {
            lowest = origin[2];
        }
    }
    return lowest;
}

// Авторитетные tick: шаг спавна квантуется до 1/128 секунды.
static void AdvanceCubes(SimulationCubeField *field, World *world, uint32_t spawns)
{
    uint32_t target = field->count + spawns;
    while (field->count < target)
    {
        double spawn[3] = {0.5, 0.5, 19.0};
        bool advanced = SimulationCubeFieldAdvanceTick(field, world, spawn);
        EXPECT(advanced);
        if (!advanced)
        {
            return;
        }
    }
}

static void ExpectCubeStatesEqual(const SimulationCubeField *first,
                                  const SimulationCubeField *second)
{
    EXPECT(first->tickCount == second->tickCount);
    EXPECT(first->count == second->count);
    EXPECT(first->spawnCounter == second->spawnCounter);
    EXPECT(first->spawnPhase == second->spawnPhase);
    EXPECT(first->randomState == second->randomState);
    EXPECT(first->nextStableId == second->nextStableId);
    EXPECT(first->failed == second->failed);
    EXPECT(first->contactCache.contactCount == second->contactCache.contactCount);
    EXPECT(first->contactCache.matchedContactCount == second->contactCache.matchedContactCount);
    // Replay compares every floating-point bit, including signed zero.
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    EXPECT(memcmp(first->spawnDirection, second->spawnDirection, sizeof(first->spawnDirection)) ==
           0);
    if (first->count != second->count)
    {
        return;
    }
    for (uint32_t index = 0u; index < first->count; ++index)
    {
        const VoxelRigidBody *left = &first->bodies[index];
        const VoxelRigidBody *right = &second->bodies[index];
        EXPECT(left->stableId == right->stableId);
        EXPECT(left->active == right->active);
        EXPECT(left->sleeping == right->sleeping);
        EXPECT(left->sleepCounter == right->sleepCounter);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        EXPECT(memcmp(left->orientation, right->orientation, sizeof(left->orientation)) == 0);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            EXPECT(InfiniteCoordCompare(&left->position[axis], &right->position[axis]) == 0);
            EXPECT(InfiniteCoordCompare(&left->linearVelocity[axis],
                                        &right->linearVelocity[axis]) == 0);
            EXPECT(InfiniteCoordCompare(&left->angularVelocity[axis],
                                        &right->angularVelocity[axis]) == 0);
        }
    }
}

// A reference allocation deliberately bypasses the game's growth path. Only
// transient buffers move; the impulse history must stay live across the move.
static bool ReserveReplayTransientBuffers(SimulationCubeField *field, uint32_t capacity)
{
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(capacity);
    VoxelRigidBody *bodies = realloc(field->bodies, (size_t)capacity * sizeof(*bodies));
    if (bodies == NULL)
    {
        return false;
    }
    field->bodies = bodies;
    VoxelRigidCompoundShape *shapes = realloc(field->shapes, (size_t)capacity * sizeof(*shapes));
    if (shapes == NULL)
        return false;
    field->shapes = shapes;
    if (capacity > field->capacity)
        // Exact allocated tail in the reference fixture.
        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(field->shapes + field->capacity, 0,
               (size_t)(capacity - field->capacity) * sizeof(*shapes));
    void *scratch = realloc(field->scratch, scratchBytes);
    if (scratch == NULL)
    {
        return false;
    }
    field->scratch = scratch;
    field->scratchBytes = scratchBytes;
    field->capacity = capacity;
    return true;
}

static void TestCubeTickReplay(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }
    SimulationCubeField reference = {0};
    SimulationCubeField frames = {0};
    SimulationCubeField backlog = {0};
    bool initialized = SimulationCubeFieldInitWithSeed(&reference, 42u) &&
                       SimulationCubeFieldInitWithSeed(&frames, 42u) &&
                       SimulationCubeFieldInitWithSeed(&backlog, 42u);
    EXPECT(initialized);
    if (initialized)
    {
        // Same physical state must also survive changing broadphase algorithms.
        reference.useSpatialIndex = false;
        frames.useSpatialIndex = true;
        backlog.useSpatialIndex = true;
        // Низкий спавнер включает столкновения с полом и между телами.
        const double spawn[3] = {0.5, 0.5, 3.0};
        // Cross both persistent-cache resets (257, 513) and the scratch-only
        // growth at 385 bodies. These allocations must not change replay.
        for (uint32_t batch = 0u; batch < 48u; ++batch)
        {
            for (uint32_t tick = 0u; tick < 16u; ++tick)
            {
                if (reference.count == 384u && reference.capacity == 384u &&
                    reference.spawnPhase + SIMULATION_CUBE_SPAWNS_PER_SECOND >=
                        SIMULATION_CUBE_TICKS_PER_SECOND)
                {
                    EXPECT(ReserveReplayTransientBuffers(&reference, 512u));
                }
                EXPECT(SimulationCubeFieldAdvanceTick(&reference, world, spawn));
                uint32_t expectedCapacity = reference.count <= 256u   ? 256u
                                            : reference.count <= 384u ? 384u
                                            : reference.count <= 512u ? 512u
                                                                      : 768u;
                EXPECT(reference.capacity == expectedCapacity);
                uint32_t persistentCapacity = SimulationGrownCapacity(256u, reference.count);
                EXPECT(reference.contactCache.bodyCapacity == persistentCapacity);
                EXPECT(reference.broadphase.bodyCapacity == persistentCapacity);
                EXPECT(reference.scratchBytes ==
                       VoxelRigidBodyStepScratchBytes(reference.capacity));
            }
            // Разные длины render-кадров, включая кадр без physics tick.
            SimulationCubeFieldUpdate(&frames, world, spawn, SIMULATION_CUBE_STEP_SECONDS * 0.25);
            SimulationCubeFieldUpdate(&frames, world, spawn, SIMULATION_CUBE_STEP_SECONDS * 15.75);
            ExpectCubeStatesEqual(&reference, &frames);
        }
        EXPECT(reference.tickCount == 768u);
        EXPECT(reference.count == 600u);

        // Six seconds of debt survive the 16-step render-frame budget.
        SimulationCubeFieldUpdate(&backlog, world, spawn, 6.0);
        EXPECT(backlog.tickCount == 16u);
        EXPECT(backlog.stepAccumulator == 6.0 - 16.0 * SIMULATION_CUBE_STEP_SECONDS);
        for (uint32_t batch = 1u; batch < 48u; ++batch)
        {
            SimulationCubeFieldUpdate(&backlog, world, spawn, 0.0);
        }
        EXPECT(backlog.stepAccumulator == 0.0);
        ExpectCubeStatesEqual(&reference, &backlog);

        SimulationCubeFieldUpdate(&frames, world, spawn, -1.0);
        SimulationCubeFieldUpdate(&frames, world, spawn, INFINITY);
        EXPECT(!SimulationCubeFieldAdvanceTick(&frames, world, (double[3]){NAN, 0.0, 0.0}));
        ExpectCubeStatesEqual(&reference, &frames);

        // Другой seed действительно меняет последовательность, а не
        // служит неиспользуемым параметром API.
        SimulationCubeField different = {0};
        EXPECT(SimulationCubeFieldInitWithSeed(&different, 43u));
        EXPECT(SimulationCubeFieldAdvanceTick(&different, world, spawn));
        EXPECT(SimulationCubeFieldAdvanceTick(&different, world, spawn));
        EXPECT(different.count == 1u);
        uint64_t expectedRandom = 43u;
        for (uint32_t sample = 0u; sample < 4u; ++sample)
        {
            expectedRandom =
                expectedRandom * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        }
        EXPECT(different.randomState == expectedRandom);
        different.settings.solverIterations = 0u;
        EXPECT(!SimulationCubeFieldAdvanceTick(&different, world, spawn));
        EXPECT(different.failed);
        EXPECT(different.tickCount == 2u);
        uint64_t failedRandom = different.randomState;
        EXPECT(!SimulationCubeFieldAdvanceTick(&different, world, spawn));
        EXPECT(different.randomState == failedRandom);
        SimulationCubeFieldRelease(&different);
    }
    SimulationCubeFieldRelease(&reference);
    SimulationCubeFieldRelease(&frames);
    SimulationCubeFieldRelease(&backlog);
    WorldDestroy(world);
}

static void TestFallingCubes(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    EXPECT(SIMULATION_CUBE_HALF_EXTENT * 2.0 == SIMULATION_CUBE_EXTENT);

    static SimulationCubeField field;
    EXPECT(SimulationCubeFieldInit(&field));
    EXPECT(SimulationCubeFieldCount(&field) == 0);

    // В среднем 100 кубов в секунду, каждый на определённом physics tick.
    AdvanceCubes(&field, world, 5u);
    EXPECT(SimulationCubeFieldCount(&field) == 5u);

    // Падение с восемнадцати блоков занимает около 1.2 с. К четырём
    // секундам первые кубы обязаны лежать на полу.
    AdvanceCubes(&field, world, 400u);
    double lowest = LowestCubeZ(&field);
    EXPECT(lowest > 0.5);
    // Пол занимает [0, 1]. Куб лежит на грани при 1.0 и стоит на ребре
    // чуть выше; выше диагонали ему взяться неоткуда.
    EXPECT(lowest < 1.0 + SIMULATION_CUBE_EXTENT);

    // Вращение обязано появиться: тела кувыркаются, а не падают плашмя.
    bool sawRotation = false;
    for (uint32_t index = 0; index < SimulationCubeFieldCount(&field); ++index)
    {
        double origin[3];
        float rotation[4];
        if (!SimulationCubeFieldPlacement(&field, index, origin, rotation))
        {
            continue;
        }
        double vector =
            fabs((double)rotation[0]) + fabs((double)rotation[1]) + fabs((double)rotation[2]);
        sawRotation = sawRotation || vector > 0.05;
    }
    EXPECT(sawRotation);

    // Смена начала координат уносит кубы вместе с сеткой.
    const int64_t shift[3] = {0, 0, 64};
    double beforeRebase = LowestCubeZ(&field);
    SimulationCubeFieldRebase(&field, shift);
    EXPECT(fabs(LowestCubeZ(&field) - (beforeRebase - 64.0)) < 1e-6);
    SimulationCubeFieldRebase(&field, (const int64_t[3]){0, 0, -64});

    // Деспавна и потолка нет: сколько появилось, столько и осталось,
    // а массив растёт сам.
    uint32_t before = SimulationCubeFieldCount(&field);
    AdvanceCubes(&field, world, 200u);
    EXPECT(SimulationCubeFieldCount(&field) == before + 200u);
    EXPECT(field.capacity >= field.count);

    // Отказы обязаны быть безвредными.
    SimulationCubeFieldUpdate(&field, world, (double[3]){0.5, 0.5, 19.0}, 0.0);
    SimulationCubeFieldUpdate(&field, world, (double[3]){0.5, 0.5, NAN}, 0.01);
    SimulationCubeFieldUpdate(NULL, world, (double[3]){0.0, 0.0, 0.0}, 0.01);
    EXPECT(SimulationCubeFieldCount(&field) == before + 200u);

    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

static void TestCubeCapacityLimit(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    bool initialized = world != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        // No multi-gigabyte allocation: a full descriptor must reject the next
        // spawn before reading or writing any body slot or incrementing count.
        field.count = VOXEL_RIGID_MAX_BODIES;
        field.capacity = VOXEL_RIGID_MAX_BODIES;
        field.spawnPhase = SIMULATION_CUBE_TICKS_PER_SECOND - SIMULATION_CUBE_SPAWNS_PER_SECOND;
        EXPECT(!SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){0.5, 0.5, 3.0}));
        EXPECT(field.failed);
        EXPECT(field.count == VOXEL_RIGID_MAX_BODIES);
        EXPECT(field.tickCount == 0u);
        field.count = 0u;
    }
    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

// A game-only cap must not silently stop the original stress-test spawner.
static void TestCubeSpawnBeyondOneThousand(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    bool initialized = world != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        for (uint32_t index = 0u; index < 1001u; ++index)
        {
            field.spawnPhase = SIMULATION_CUBE_TICKS_PER_SECOND - SIMULATION_CUBE_SPAWNS_PER_SECOND;
            const double spawn[3] = {(double)index * 3.0, 0.5, 10.0};
            EXPECT(SimulationCubeFieldAdvanceTick(&field, world, spawn));
            if (field.failed)
                break;
            // Keep this a spawner regression, not a dense-pile benchmark.
            field.bodies[field.count - 1u].active = false;
        }
        EXPECT(field.spawnCounter == 1001u);
        EXPECT(!field.failed);
    }
    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

// Мир, который правит игрок: сломанный блок бесконечного пола становится
// воздухом поверх базового слоя, а поставленный материал — обычной правкой.
// Это контракт, на который опирается ЛКМ/ПКМ клиента.
static void TestWorldBlockEdit(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    EXPECT(WorldGetBlock(world, 3, -2, 0) == SIMULATION_MATERIAL_FOUNDATION);
    EXPECT(WorldTrySetBlock(world, 3, -2, 0, BLOCK_AIR));
    EXPECT(WorldGetBlock(world, 3, -2, 0) == BLOCK_AIR);
    // Соседняя клетка пола не тронута: правка точечная.
    EXPECT(WorldGetBlock(world, 4, -2, 0) == SIMULATION_MATERIAL_FOUNDATION);

    EXPECT(WorldGetBlock(world, 3, -2, 1) == BLOCK_AIR);
    EXPECT(WorldTrySetBlock(world, 3, -2, 1, SIMULATION_MATERIAL_MARKER));
    EXPECT(WorldGetBlock(world, 3, -2, 1) == SIMULATION_MATERIAL_MARKER);

    // Возврат клетки к значению базового слоя снимает правку, а не кладёт
    // поверх неё ещё один слой.
    EXPECT(WorldTrySetBlock(world, 3, -2, 0, SIMULATION_MATERIAL_FOUNDATION));
    EXPECT(WorldGetBlock(world, 3, -2, 0) == SIMULATION_MATERIAL_FOUNDATION);

    // Правка обязана пережить смену начала координат: пол уезжает, а дыра
    // в нём — нет, иначе разрушение откатывалось бы само.
    EXPECT(WorldTrySetBlock(world, 3, -2, 0, BLOCK_AIR));
    EXPECT(WorldRebase(world, 0, 0, 64));
    EXPECT(WorldGetBlock(world, 3, -2, -64) == BLOCK_AIR);
    EXPECT(WorldGetBlock(world, 4, -2, -64) == SIMULATION_MATERIAL_FOUNDATION);

    WorldDestroy(world);
}

// Постройка — это набор блоков, который держится связностью по граням.
// Проверяются три вещи: постановка блока, удаление без распада и распад на
// две постройки, когда удалённый блок был единственной связью.
static void TestConstructTopology(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    ConstructSystem *system = (ConstructSystem *)malloc(sizeof(*system));
    EXPECT(world != NULL && system != NULL);
    if (world == NULL || system == NULL)
    {
        free(system);
        WorldDestroy(world);
        return;
    }
    SimulationCubeField field = {0};
    if (!SimulationCubeFieldInit(&field))
    {
        EXPECT(false);
        free(system);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);

    // Цепочка [0,0,0],[1,0,0] плюс ветка [0,1,0]: связность держат грани.
    const ConstructBlock blocks[3] = {
        {{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},
        {{1, 0, 0}, SIMULATION_MATERIAL_ACCENT},
        {{0, 1, 0}, SIMULATION_MATERIAL_MARKER},
    };
    uint64_t bodyId = 0u;
    EXPECT(ConstructSpawn(system, blocks, 3u, (const double[3]){10.0, 10.0, 10.0}, &bodyId));
    EXPECT(bodyId != 0u);
    EXPECT(ConstructActiveBodyCount(system) == 1u);

    // Постановка свободного оторванного блока запрещена: он бы сразу упал.
    EXPECT(!ConstructPlaceBlock(system, bodyId, (const int32_t[3]){5, 5, 5},
                                SIMULATION_MATERIAL_ACCENT));
    // А касающийся гранью — разрешена.
    EXPECT(ConstructPlaceBlock(system, bodyId, (const int32_t[3]){2, 0, 0},
                               SIMULATION_MATERIAL_ACCENT));
    // Повторная постановка в занятую клетку запрещена.
    EXPECT(!ConstructPlaceBlock(system, bodyId, (const int32_t[3]){2, 0, 0},
                                SIMULATION_MATERIAL_ACCENT));

    // Удаление концевого блока [2,0,0] не рвёт связность.
    uint32_t splitCount = 0u;
    uint64_t splitIds[8] = {0};
    EXPECT(ConstructBreakBlock(system, bodyId, (const int32_t[3]){2, 0, 0}, splitIds, 8u,
                               &splitCount, NULL));
    EXPECT(splitCount == 0u);
    EXPECT(ConstructActiveBodyCount(system) == 1u);

    // Тело: [0,0,0],[1,0,0],[0,1,0]. Удаление [1,0,0] оставляет связные
    // [0,0,0] и [0,1,0] — распада нет.
    EXPECT(ConstructBreakBlock(system, bodyId, (const int32_t[3]){1, 0, 0}, splitIds, 8u,
                               &splitCount, NULL));
    EXPECT(splitCount == 0u);
    EXPECT(ConstructActiveBodyCount(system) == 1u);

    // Достраиваем обратно в цепочку [0,0,0],[1,0,0],[2,0,0] и ломаем
    // середину: [2,0,0] отсоединяется от [0,0,0], это распад на два тела.
    EXPECT(ConstructPlaceBlock(system, bodyId, (const int32_t[3]){1, 0, 0},
                               SIMULATION_MATERIAL_ACCENT));
    EXPECT(ConstructPlaceBlock(system, bodyId, (const int32_t[3]){2, 0, 0},
                               SIMULATION_MATERIAL_ACCENT));
    EXPECT(ConstructBreakBlock(system, bodyId, (const int32_t[3]){1, 0, 0}, splitIds, 8u,
                               &splitCount, NULL));
    EXPECT(splitCount == 1u);
    EXPECT(ConstructActiveBodyCount(system) == 2u);
    EXPECT(splitIds[0] != 0u && splitIds[0] != bodyId);

    // Удаление несуществующего блока — отказ без изменений.
    EXPECT(!ConstructBreakBlock(system, bodyId, (const int32_t[3]){9, 9, 9}, NULL, 0u, NULL, NULL));
    // Разрушаем оба тела по частям: сначала оставшийся [2,0,0] нового тела.
    EXPECT(ConstructBreakBlock(system, splitIds[0], (const int32_t[3]){2, 0, 0}, NULL, 0u, NULL,
                               NULL));
    EXPECT(ConstructActiveBodyCount(system) == 1u);
    // Затем исходное тело до конца.
    EXPECT(ConstructBreakBlock(system, bodyId, (const int32_t[3]){0, 1, 0}, NULL, 0u, NULL, NULL));
    EXPECT(ConstructBreakBlock(system, bodyId, (const int32_t[3]){0, 0, 0}, NULL, 0u, NULL, NULL));
    EXPECT(ConstructActiveBodyCount(system) == 0u);

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructRaycast(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    ConstructSystem *system = (ConstructSystem *)malloc(sizeof(*system));
    EXPECT(world != NULL && system != NULL);
    if (world == NULL || system == NULL)
    {
        free(system);
        WorldDestroy(world);
        return;
    }
    SimulationCubeField field = {0};
    if (!SimulationCubeFieldInit(&field))
    {
        EXPECT(false);
        free(system);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);

    const ConstructBlock blocks[1] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT}};
    uint64_t bodyId = 0u;
    EXPECT(ConstructSpawn(system, blocks, 1u, (const double[3]){0.0, 0.0, 5.0}, &bodyId));

    // Луч сверху вниз входит в верхнюю грань блока на z = 6.
    ConstructRaycastHit hit;
    EXPECT(ConstructRaycast(system, (const double[3]){0.5, 0.5, 10.0},
                            (const double[3]){0.0, 0.0, -1.0}, 100.0, &hit));
    EXPECT(hit.bodyId == bodyId);
    EXPECT(fabs(hit.distance - 4.0) < 1e-9);
    EXPECT(hit.normal[2] == 1);

    // Мимо — отказ.
    EXPECT(!ConstructRaycast(system, (const double[3]){5.5, 5.5, 10.0},
                             (const double[3]){0.0, 0.0, -1.0}, 2.0, &hit));

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// Падение: тело обязано остановиться на полу, а не провалиться сквозь него.
// Пол занимает клетку z = 0, значит низ блока ложится на z = 1.
static void TestConstructFalls(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    ConstructSystem *system = (ConstructSystem *)malloc(sizeof(*system));
    EXPECT(world != NULL && system != NULL);
    if (world == NULL || system == NULL)
    {
        free(system);
        WorldDestroy(world);
        return;
    }
    SimulationCubeField field = {0};
    if (!SimulationCubeFieldInit(&field))
    {
        EXPECT(false);
        free(system);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);

    const ConstructBlock blocks[1] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT}};
    uint64_t bodyId = 0u;
    EXPECT(ConstructSpawn(system, blocks, 1u, (const double[3]){0.5, 0.5, 6.0}, &bodyId));

    for (uint32_t frame = 0u; frame < 240u; ++frame)
    {
        // Suppress this test's spawner; every body uses the same authoritative tick.
        field.spawnPhase = 0u;
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){50.0, 50.0, 10.0}));
    }
    ConstructBody *body = NULL;
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        if (system->bodies[index].active)
        {
            body = &system->bodies[index];
            break;
        }
    }
    EXPECT(body != NULL);
    if (body != NULL)
    {
        double origin[3];
        float rotation[4];
        uint32_t slot = (uint32_t)(body - system->bodies);
        EXPECT(ConstructBlockPlacement(system, slot, 0u, origin, rotation));
        EXPECT(origin[2] > 0.98);
        EXPECT(origin[2] < 1.02);
    }

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static VoxelRigidBody *FindConstructRigid(SimulationCubeField *field, uint64_t id)
{
    for (uint32_t index = 0u; index < field->count; ++index)
        if (field->bodies[index].active && field->bodies[index].stableId == id)
            return &field->bodies[index];
    return NULL;
}

static uint32_t FindConstructSlot(const ConstructSystem *system, uint64_t id)
{
    for (uint32_t index = 0u; index < system->capacity; ++index)
        if (system->bodies[index].active && system->bodies[index].id == id)
            return index;
    return system->capacity;
}

// Editing changes COM, not the world-space pose of the surviving blocks.
// Fragments inherit v + omega x r at their own COM, not just the old v.
static void TestConstructEditPoseAndVelocity(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        SimulationCubeFieldRelease(&field);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    const ConstructBlock chain[3] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                     {{1, 0, 0}, SIMULATION_MATERIAL_MARKER},
                                     {{2, 0, 0}, SIMULATION_MATERIAL_ACCENT}};
    uint64_t id = 0u;
    EXPECT(ConstructSpawn(system, chain, 3u, (const double[3]){10.0, 10.0, 8.0}, &id));
    VoxelRigidBody *rigid = FindConstructRigid(&field, id);
    EXPECT(rigid != NULL);
    if (rigid != NULL)
    {
        rigid->orientation[2] = 1.0;
        rigid->orientation[3] = 0.0;
        EXPECT(VoxelRigidBodyAddLinearVelocity(rigid, (const double[3]){1.0, 2.0, 3.0}));
        EXPECT(VoxelRigidBodyAddAngularVelocity(rigid, (const double[3]){0.0, 0.0, 2.0}));
        uint32_t slot = FindConstructSlot(system, id);
        double origins[2][3];
        double expectedVelocity[2][3];
        float rotation[4];
        EXPECT(ConstructBlockPlacement(system, slot, 0u, origins[0], rotation));
        EXPECT(ConstructBlockPlacement(system, slot, 2u, origins[1], rotation));
        for (uint32_t part = 0u; part < 2u; ++part)
        {
            // The exact 180-degree Z rotation sends the local cube centre to (-.5,-.5,+.5).
            const double center[3] = {origins[part][0] - 0.5, origins[part][1] - 0.5,
                                      origins[part][2] + 0.5};
            EXPECT(VoxelRigidBodyPointVelocity(rigid, center, expectedVelocity[part]));
        }
        uint64_t splitId = 0u;
        uint32_t splitCount = 0u;
        EXPECT(ConstructBreakBlock(system, id, (const int32_t[3]){1, 0, 0}, &splitId, 1u,
                                   &splitCount, NULL));
        EXPECT(splitCount == 1u);
        for (uint32_t part = 0u; part < 2u; ++part)
        {
            uint64_t partId = part == 0u ? id : splitId;
            VoxelRigidBody *fragment = FindConstructRigid(&field, partId);
            EXPECT(fragment != NULL);
            if (fragment == NULL)
                continue;
            double origin[3];
            double velocity[3];
            EXPECT(ConstructBlockPlacement(system, FindConstructSlot(system, partId), 0u, origin,
                                           rotation));
            EXPECT(VoxelRigidBodyLinearVelocity(fragment, velocity));
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                EXPECT(fabs(origin[axis] - origins[part][axis]) < 1e-8);
                EXPECT(fabs(velocity[axis] - expectedVelocity[part][axis]) < 1e-8);
            }
        }
        ConstructRaycastHit hit;
        const double ray[3] = {origins[1][0] - 0.5, origins[1][1] - 0.5, 20.0};
        EXPECT(ConstructRaycast(system, ray, (const double[3]){0.0, 0.0, -1.0}, 30.0, &hit));
        EXPECT(hit.bodyId == splitId && hit.normal[2] == 1);
        EXPECT(!ConstructRaycast(system, (const double[3]){NAN, 0.0, 0.0},
                                 (const double[3]){0.0, 0.0, -1.0}, 30.0, &hit));
    }
    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// Дописывает в blocks сплошной параллелепипед со стартовой клеткой
// (baseX, baseY, baseZ), пропуская одну клетку skip (может быть NULL).
static uint32_t AppendCuboid(ConstructBlock *blocks, uint32_t write, int32_t baseX, int32_t baseY,
                             int32_t baseZ, int32_t sizeX, int32_t sizeY, int32_t sizeZ,
                             uint8_t material, const int32_t skip[3])
{
    for (int32_t z = 0; z < sizeZ; ++z)
    {
        for (int32_t y = 0; y < sizeY; ++y)
        {
            for (int32_t x = 0; x < sizeX; ++x)
            {
                int32_t lx = baseX + x;
                int32_t ly = baseY + y;
                int32_t lz = baseZ + z;
                if (skip != NULL && skip[0] == lx && skip[1] == ly && skip[2] == lz)
                {
                    continue;
                }
                blocks[write].local[0] = lx;
                blocks[write].local[1] = ly;
                blocks[write].local[2] = lz;
                blocks[write].material = material;
                ++write;
            }
        }
    }
    return write;
}

// Прежний фиксированный пул 1024 заменён ростом в куче: система обязана
// принять больше 1024 тел и переиспользовать наименьший освободившийся слот,
// не теряя тела и не заводя игрового потолка.
static void TestConstructGrowthBeyondOneThousand(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        SimulationCubeFieldRelease(&field);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    const ConstructBlock block = {{0, 0, 0}, SIMULATION_MATERIAL_ACCENT};
    for (uint32_t index = 0u; index < 1100u; ++index)
    {
        EXPECT(ConstructSpawn(system, &block, 1u, (const double[3]){(double)index, 0.0, 10.0},
                              NULL));
    }
    EXPECT(ConstructActiveBodyCount(system) == 1100u);
    EXPECT(field.count == 1100u);
    EXPECT(system->capacity >= 1100u);
    EXPECT(system->capacity > 1024u);
    for (uint32_t index = 0u; index < 1100u; ++index)
    {
        EXPECT(system->bodies[index].active);
        EXPECT(system->bodies[index].blocks != NULL);
        EXPECT(system->bodies[index].shapeBoxes != NULL);
    }
    // Освобождение одного тела не сжимает ёмкость, а следующий спавн занимает
    // наименьший свободный слот.
    uint64_t removedId = system->bodies[500].id;
    EXPECT(ConstructBreakBlock(system, removedId, (const int32_t[3]){0, 0, 0}, NULL, 0u, NULL,
                               NULL));
    EXPECT(!system->bodies[500].active);
    EXPECT(ConstructActiveBodyCount(system) == 1099u);
    uint64_t reAddedId = 0u;
    EXPECT(ConstructSpawn(system, &block, 1u, (const double[3]){0.0, 0.0, 10.0}, &reAddedId));
    EXPECT(system->bodies[500].active && system->bodies[500].id == reAddedId);
    EXPECT(ConstructActiveBodyCount(system) == 1100u);
    EXPECT(system->capacity > 1024u);
    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// 4096 блоков в одном теле: форма не имеет лимита на число блоков, а блоки и
// коробки владеются отдельными массивами. Правка меняет оба массива, но поле
// всегда видит shapeBoxes тела, а не освобождённый прежний массив.
static void TestConstructLargeShapeOwnership(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        SimulationCubeFieldRelease(&field);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    const uint32_t total = 64u * 64u;
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)total * sizeof(*blocks));
    EXPECT(blocks != NULL);
    if (blocks != NULL)
    {
        uint32_t count = AppendCuboid(blocks, 0u, 0, 0, 0, 64, 64, 1, SIMULATION_MATERIAL_ACCENT,
                                      NULL);
        EXPECT(count == total);
        uint64_t id = 0u;
        EXPECT(ConstructSpawn(system, blocks, count, (const double[3]){0.0, 0.0, 10.0}, &id));
        free(blocks);
        uint32_t slot = FindConstructSlot(system, id);
        EXPECT(slot < system->capacity);
        if (slot < system->capacity)
        {
            ConstructBody *body = &system->bodies[slot];
            EXPECT(body->blockCount == total);
            EXPECT(body->blocks != NULL && body->shapeBoxes != NULL);
            EXPECT(field.shapes[body->bodyIndex].boxes == body->shapeBoxes);
            // Сплошная плита стряпается в одну точную коробку.
            EXPECT(field.shapes[body->bodyIndex].boxCount == 1u);
            ConstructBlock *beforeBlocks = body->blocks;
            VoxelRigidCompoundBox *beforeBoxes = body->shapeBoxes;
            EXPECT(ConstructPlaceBlock(system, id, (const int32_t[3]){0, 0, 1},
                                       SIMULATION_MATERIAL_MARKER));
            EXPECT(body->blockCount == total + 1u);
            EXPECT(body->blocks != beforeBlocks);
            EXPECT(body->shapeBoxes != beforeBoxes);
            EXPECT(field.shapes[body->bodyIndex].boxes == body->shapeBoxes);
            EXPECT(body->blocks[total].local[0] == 0 && body->blocks[total].local[1] == 0 &&
                   body->blocks[total].local[2] == 1);
            EXPECT(body->blocks[total].material == SIMULATION_MATERIAL_MARKER);
        }
    }
    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// Распад большого тела: два кубоида по ~2048 блоков, соединённые одним
// блоком, после разрыва становятся двумя телами. Каждое владеет своими
// блоками и формой, а поле ссылается на shapeBoxes именно этого тела.
static void TestConstructLargeSplitOwnership(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        SimulationCubeFieldRelease(&field);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    const uint32_t total = 4096u;
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)total * sizeof(*blocks));
    EXPECT(blocks != NULL);
    if (blocks != NULL)
    {
        uint32_t count = AppendCuboid(blocks, 0u, 0, 0, 0, 16, 16, 8, SIMULATION_MATERIAL_ACCENT,
                                      NULL);
        blocks[count] = (ConstructBlock){{16, 0, 0}, SIMULATION_MATERIAL_MARKER};
        ++count;
        const int32_t skip[3] = {32, 15, 7};
        count = AppendCuboid(blocks, count, 17, 0, 0, 16, 16, 8, SIMULATION_MATERIAL_ACCENT, skip);
        EXPECT(count == total);
        uint64_t parentId = 0u;
        EXPECT(ConstructSpawn(system, blocks, count, (const double[3]){0.0, 0.0, 10.0}, &parentId));
        free(blocks);
        EXPECT(ConstructActiveBodyCount(system) == 1u);
        uint64_t childIds[4] = {0};
        uint32_t childCount = 0u;
        EXPECT(ConstructBreakBlock(system, parentId, (const int32_t[3]){16, 0, 0}, childIds, 4u,
                                   &childCount, NULL));
        EXPECT(childCount == 1u);
        EXPECT(ConstructActiveBodyCount(system) == 2u);
        EXPECT(field.count == 2u);
        uint32_t parentSlot = FindConstructSlot(system, parentId);
        uint32_t childSlot = FindConstructSlot(system, childIds[0]);
        EXPECT(parentSlot < system->capacity && childSlot < system->capacity);
        if (parentSlot < system->capacity && childSlot < system->capacity)
        {
            ConstructBody *parent = &system->bodies[parentSlot];
            ConstructBody *child = &system->bodies[childSlot];
            EXPECT(parent->id != child->id);
            EXPECT(parent->blockCount == 2048u);
            EXPECT(child->blockCount == 2047u);
            EXPECT(parent->blocks != child->blocks);
            EXPECT(parent->shapeBoxes != child->shapeBoxes);
            EXPECT(field.shapes[parent->bodyIndex].boxes == parent->shapeBoxes);
            EXPECT(field.shapes[child->bodyIndex].boxes == child->shapeBoxes);
            // Каждая половина сливается в считанные точные коробки, а не в
            // 2048 дочерних.
            EXPECT(field.shapes[parent->bodyIndex].boxCount >= 1u &&
                   field.shapes[parent->bodyIndex].boxCount <= 8u);
            EXPECT(field.shapes[child->bodyIndex].boxCount >= 1u &&
                   field.shapes[child->bodyIndex].boxCount <= 8u);
        }
    }
    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructSixWaySplit(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        ConstructSystemInit(system, world, &field);
        const ConstructBlock blocks[7] = {
            {{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},  {{1, 0, 0}, SIMULATION_MATERIAL_ACCENT},
            {{-1, 0, 0}, SIMULATION_MATERIAL_ACCENT}, {{0, 1, 0}, SIMULATION_MATERIAL_ACCENT},
            {{0, -1, 0}, SIMULATION_MATERIAL_ACCENT}, {{0, 0, 1}, SIMULATION_MATERIAL_ACCENT},
            {{0, 0, -1}, SIMULATION_MATERIAL_ACCENT},
        };
        uint64_t parentId = 0u;
        EXPECT(ConstructSpawn(system, blocks, 7u, (const double[3]){10.0, 10.0, 8.0}, &parentId));
        uint64_t childIds[5] = {0};
        uint32_t childCount = 0u;
        EXPECT(ConstructBreakBlock(system, parentId, (const int32_t[3]){0, 0, 0}, childIds, 5u,
                                   &childCount, NULL));
        EXPECT(childCount == 5u);
        EXPECT(field.count == 6u);
        EXPECT(ConstructActiveBodyCount(system) == 6u);
        for (uint32_t index = 0u; index < 5u; ++index)
        {
            uint32_t slot = FindConstructSlot(system, childIds[index]);
            EXPECT(slot < system->capacity);
            if (slot < system->capacity)
            {
                const ConstructBody *part = &system->bodies[slot];
                EXPECT(part->blockCount == 1u);
                EXPECT(part->bodyIndex < field.count);
                if (part->bodyIndex < field.count)
                {
                    EXPECT(field.bodies[part->bodyIndex].stableId == childIds[index]);
                    EXPECT(field.shapes[part->bodyIndex].boxes == part->shapeBoxes);
                }
            }
            for (uint32_t previous = 0u; previous < index; ++previous)
                EXPECT(childIds[previous] != childIds[index]);
        }
        field.spawnPhase = 0u;
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){50.0, 50.0, 10.0}));
        ConstructSystemReset(system);
        EXPECT(field.count == 0u);
    }
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructFrameReplay(VoxelRigidSolverOrder solverOrder)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField fields[2] = {0};
    ConstructSystem *systems = calloc(2u, sizeof(*systems));
    LaiueTaskPool *pool = LaiueTaskPoolCreate(2u);
    LaiueTaskExecutor executor = {0};
    executor.structSize = sizeof(executor);
    bool haveExecutor = pool != NULL && LaiueTaskPoolGetExecutor(pool, &executor);
    EXPECT(haveExecutor);
    bool initialized = world != NULL && systems != NULL && SimulationCubeFieldInit(&fields[0]) &&
                       SimulationCubeFieldInit(&fields[1]);
    EXPECT(initialized);
    if (initialized)
    {
        const ConstructBlock shape[3] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                         {{1, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                         {{0, 0, 1}, SIMULATION_MATERIAL_MARKER}};
        for (uint32_t index = 0u; index < 2u; ++index)
        {
            ConstructSystemInit(&systems[index], world, &fields[index]);
            fields[index].stepOptions.solverOrder = solverOrder;
            fields[index].stepOptions.executor = index == 0u && haveExecutor ? &executor : NULL;
            EXPECT(ConstructSpawn(&systems[index], shape, 3u, (const double[3]){10.0, 10.0, 6.0},
                                  NULL));
        }
        const double spawn[3] = {50.0, 50.0, 10.0};
        for (uint32_t tick = 0u; tick < 384u; ++tick)
        {
            fields[0].spawnPhase = fields[1].spawnPhase = 0u;
            SimulationCubeFieldUpdate(&fields[0], world, spawn, 1.0 / 256.0);
            SimulationCubeFieldUpdate(&fields[0], world, spawn, 1.0 / 256.0);
            EXPECT(SimulationCubeFieldAdvanceTick(&fields[1], world, spawn));
            ExpectCubeStatesEqual(&fields[0], &fields[1]);
            if (fields[0].failed || fields[1].failed)
                break;
        }
        EXPECT(fields[0].tickCount == 384u);
        const int64_t shift[3] = {INT64_C(1) << 50, -(INT64_C(1) << 50), 0};
        const int64_t undo[3] = {-shift[0], -shift[1], 0};
        SimulationCubeFieldRebase(&fields[0], shift);
        SimulationCubeFieldRebase(&fields[0], undo);
        ExpectCubeStatesEqual(&fields[0], &fields[1]);
        for (uint32_t index = 0u; index < 2u; ++index)
            ConstructSystemReset(&systems[index]);
    }
    SimulationCubeFieldRelease(&fields[0]);
    SimulationCubeFieldRelease(&fields[1]);
    LaiueTaskPoolDestroy(pool);
    free(systems);
    WorldDestroy(world);
}

static void TestConstructPlateBudget(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        ConstructSystemInit(system, world, &field);
        ConstructBlock blocks[16];
        for (uint32_t index = 0u; index < 16u; ++index)
        {
            blocks[index].local[0] = (int32_t)(index % 4u);
            blocks[index].local[1] = (int32_t)(index / 4u);
            blocks[index].local[2] = 0;
            blocks[index].material = SIMULATION_MATERIAL_ACCENT;
        }
        uint64_t id = 0u;
        EXPECT(ConstructSpawn(system, blocks, 16u, (const double[3]){10.0, 10.0, 1.0}, &id));
        uint32_t peakContacts = 0u;
        for (uint32_t tick = 0u; tick < 256u; ++tick)
        {
            field.spawnPhase = 0u;
            EXPECT(
                SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){50.0, 50.0, 10.0}));
            if (field.lastContactCount > peakContacts)
                peakContacts = field.lastContactCount;
            if (field.failed)
                break;
        }
        EXPECT(!field.failed);
        EXPECT(field.tickCount == 256u);
        // The 16 editable blocks cook into one exact slab, not 16 overlapping
        // contact patches. Raw many-child contact-budget tests live in laiue.
        EXPECT(peakContacts > 0u && peakContacts <= 16u);
        EXPECT(field.shapes[0].boxCount == 1u);
        VoxelRigidBody *body = FindConstructRigid(&field, id);
        EXPECT(body != NULL);
        if (body != NULL)
        {
            double position[3];
            EXPECT(VoxelRigidBodyLocalPosition(body, position));
            EXPECT(position[2] > 1.48 && position[2] < 1.52);
        }
        ConstructSystemReset(system);
    }
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructCookedEditBudget(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        ConstructSystemInit(system, world, &field);
        ConstructBlock blocks[16];
        for (uint32_t index = 0u; index < 16u; ++index)
            blocks[index] = (ConstructBlock){{(int32_t)(index % 4u), (int32_t)(index / 4u), 0},
                                              SIMULATION_MATERIAL_ACCENT};
        uint64_t id = 0u;
        EXPECT(ConstructSpawn(system, blocks, 16u, (const double[3]){10.0, 10.0, 8.0}, &id));
        EXPECT(field.shapes[0].boxCount == 1u);
        // Force the actual old one-box budget. Removing one interior block
        // increases collision-box count despite decreasing editable block count.
        uint32_t exactBytes = VoxelRigidBodyStepCompoundScratchBytes(1u, 1u);
        void *exactScratch = malloc(exactBytes);
        EXPECT(exactScratch != NULL);
        if (exactScratch != NULL)
        {
            free(field.scratch);
            field.scratch = exactScratch;
            field.scratchBytes = exactBytes;
            EXPECT(ConstructBreakBlock(system, id, (const int32_t[3]){1, 1, 0},
                                       NULL, 0u, NULL, NULL));
            EXPECT(field.count == 1u && system->bodies[0].blockCount == 15u);
            EXPECT(field.shapes[0].boxCount > 1u);
            EXPECT(field.scratchBytes >=
                   VoxelRigidBodyStepCompoundScratchBytes(1u, field.shapes[0].boxCount));
            ConstructRaycastHit hit;
            EXPECT(!ConstructRaycast(system, (const double[3]){11.5, 11.5, 12.0},
                                     (const double[3]){0.0, 0.0, -1.0}, 5.0, &hit));
            EXPECT(ConstructPlaceBlock(system, id, (const int32_t[3]){1, 1, 0},
                                       SIMULATION_MATERIAL_ACCENT));
            EXPECT(field.shapes[0].boxCount == 1u);
            EXPECT(system->bodies[0].blockCount == 16u);
            field.spawningStopped = true;
            EXPECT(SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){0.0, 0.0, 19.0}));
        }
        ConstructSystemRelease(system);
    }
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructAppendKeepsContacts(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        ConstructSystemInit(system, world, &field);
        const ConstructBlock block = {{0, 0, 0}, SIMULATION_MATERIAL_ACCENT};
        EXPECT(ConstructSpawn(system, &block, 1u, (const double[3]){10.0, 10.0, 0.99}, NULL));
        field.spawningStopped = true;
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){0.0, 0.0, 19.0}));
        uint32_t contacts = field.contactCache.contactCount;
        EXPECT(contacts > 0u);
        void *storage = field.contactCache.storage;
        EXPECT(ConstructSpawn(system, &block, 1u, (const double[3]){50.0, 50.0, 20.0}, NULL));
        EXPECT(field.contactCache.storage == storage);
        EXPECT(field.contactCache.contactCount == contacts);
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, (const double[3]){0.0, 0.0, 19.0}));
        EXPECT(field.contactCache.matchedContactCount > 0u);
        ConstructSystemRelease(system);
    }
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestConstructRejectedEditPreservesBody(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        ConstructSystemInit(system, world, &field);
        const ConstructBlock disconnected[2] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                                {{2, 0, 0}, SIMULATION_MATERIAL_ACCENT}};
        uint64_t initialId = field.nextStableId;
        EXPECT(!ConstructSpawn(system, disconnected, 2u, (const double[3]){0.0, 0.0, 10.0}, NULL));
        EXPECT(field.count == 0u && field.nextStableId == initialId);
        const ConstructBlock chain[3] = {{{0, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                         {{1, 0, 0}, SIMULATION_MATERIAL_ACCENT},
                                         {{2, 0, 0}, SIMULATION_MATERIAL_ACCENT}};
        uint64_t id = 0u;
        EXPECT(ConstructSpawn(system, chain, 3u, (const double[3]){0.0, 0.0, 10.0}, &id));
        uint32_t slot = FindConstructSlot(system, id);
        VoxelRigidBody *body = FindConstructRigid(&field, id);
        EXPECT(body != NULL);
        if (body != NULL && slot < system->capacity)
        {
            double before[3];
            double after[3];
            EXPECT(VoxelRigidBodyLocalPosition(body, before));
            EXPECT(!ConstructPlaceBlock(system, id, (const int32_t[3]){INT32_MAX, INT32_MIN, 0},
                                        SIMULATION_MATERIAL_ACCENT));
            EXPECT(system->bodies[slot].blockCount == 3u && !field.failed);
            field.nextStableId = 0u;
            EXPECT(!ConstructBreakBlock(system, id, (const int32_t[3]){1, 0, 0}, NULL, 0u, NULL,
                                        NULL));
            EXPECT(system->bodies[slot].blockCount == 3u);
            EXPECT(field.count == 1u && field.nextStableId == 0u && !field.failed);
            EXPECT(VoxelRigidBodyLocalPosition(FindConstructRigid(&field, id), after));
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                EXPECT(before[axis] == after[axis]);
        }
        ConstructSystemReset(system);
    }
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestOriginShift(void)
{
    SimulationOriginShift shift;
    const double nearOrigin[3] = {511.9, -511.9, 2.0};
    EXPECT(SimulationOriginShiftPlan(nearOrigin, &shift));
    EXPECT(!shift.required);

    double distant[3] = {513.25, -513.75, 1025.5};
    EXPECT(SimulationOriginShiftPlan(distant, &shift));
    EXPECT(shift.required);
    EXPECT(shift.block[0] == 512);
    EXPECT(shift.block[1] == -512);
    EXPECT(shift.block[2] == 1024);
    SimulationOriginShiftApply(distant, &shift);
    EXPECT(fabs(distant[0] - 1.25) < 0.000001);
    EXPECT(fabs(distant[1] + 1.75) < 0.000001);
    EXPECT(fabs(distant[2] - 1.5) < 0.000001);

    const double invalid[3] = {NAN, 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(invalid, &shift));
    EXPECT(!shift.required);

    const double positiveLimit[3] = {ldexp(1.0, 63), 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(positiveLimit, &shift));
    const double negativeLimit[3] = {-ldexp(1.0, 63), 0.0, 0.0};
    EXPECT(SimulationOriginShiftPlan(negativeLimit, &shift));
    EXPECT(shift.block[0] == INT64_MIN);
    const double negativeOverflow[3] = {nextafter(-ldexp(1.0, 63), -INFINITY), 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(negativeOverflow, &shift));
    EXPECT(!shift.required);

    EXPECT(SimulationBlockToChunkFloor(64) == 1);
    EXPECT(SimulationBlockToChunkFloor(63) == 0);
    EXPECT(SimulationBlockToChunkFloor(0) == 0);
    EXPECT(SimulationBlockToChunkFloor(-1) == -1);
    EXPECT(SimulationBlockToChunkFloor(-64) == -1);
    EXPECT(SimulationBlockToChunkFloor(-65) == -2);
    EXPECT(SimulationBlockToChunkFloor(INT64_MIN) == INT64_MIN / 64);

    volatile int64_t runtimeNegativeMultiple = -INT64_C(7443687346329987200);
    EXPECT(SimulationBlockToChunkFloor(runtimeNegativeMultiple) == -INT64_C(116307614786406050));
}

static void TestFrameTiming(void)
{
    EXPECT(SimulationFrameDeltaSeconds(10.0, 10.016) > 0.015f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 10.016) < 0.017f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 9.0) == 0.0f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 12.0) == 0.1f);
    EXPECT(SimulationFrameDeltaSeconds(NAN, 12.0) == 0.0f);
}

// Политика роста буфера обязана удваивать ёмкость, а не выделять ровно под
// запрос: иначе буфер инстансов перевыделялся бы на каждом кадре с новым
// кубом — O(n) перевыделений за прогон вместо O(log n).
// Спавнит куб в заданной точке через публичный путь поля. Использует
// Update с нулевым временем, чтобы не продвигать физику лишними шагами.
static bool PlaceCube(SimulationCubeField *field, World *world, const double position[3])
{
    double spawn[3] = {position[0], position[1], position[2]};
    uint32_t before = field->count;
    field->spawnPhase = SIMULATION_CUBE_TICKS_PER_SECOND - SIMULATION_CUBE_SPAWNS_PER_SECOND;
    bool advanced = SimulationCubeFieldAdvanceTick(field, world, spawn);
    return advanced && field->count == before + 1u;
}

// Луч обязан найти ближайший куб, а не любой на пути, и не спутать ось:
// два куба на одной прямой, дальний не должен перекрыть ближний.
static void TestCubeRaycast(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    bool initialized = world != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        // Точка спавна не влияет на позицию: SpawnCube ставит тело от
        // переданной точки плюс золотое смещение. Прямой контроль позиции
        // тут не нужен — проверяется, что луч находит именно существующее
        // тело и возвращает конечное расстояние.
        EXPECT(PlaceCube(&field, world, (const double[3]){0.5, 0.5, 19.0}));
        EXPECT(PlaceCube(&field, world, (const double[3]){0.5, 0.5, 19.0}));
        EXPECT(field.count == 2u);
        if (field.count != 2u)
        {
            SimulationCubeFieldRelease(&field);
            WorldDestroy(world);
            return;
        }

        // Луч идёт из точки прямо над первым кубом вниз. Точное положение
        // тела берётся у движка: спавн смещает куб по золотому углу, и
        // угадывать его координату значило бы тестировать не луч, а спавнер.
        double position[3];
        EXPECT(VoxelRigidBodyLocalPosition(&field.bodies[0], position));
        double origin[3] = {position[0], position[1], position[2] + 5.0};
        double direction[3] = {0.0, 0.0, -1.0};
        uint32_t index = 0u;
        double distance = 0.0;
        EXPECT(SimulationCubeFieldRaycast(&field, origin, direction, 100.0, &index, &distance));
        EXPECT(index == 0u);
        EXPECT(distance >= 0.0);
        EXPECT(distance < 5.0);

        // Обратный луч: снизу вверх ближайший — тот же куб.
        double up[3] = {0.0, 0.0, 1.0};
        double below[3] = {position[0], position[1], position[2] - 5.0};
        EXPECT(SimulationCubeFieldRaycast(&field, below, up, 100.0, &index, &distance));
        EXPECT(index == 0u);

        // Мимо кубов: луч вбок с малым радиусом ни во что не попадает.
        double sideways[3] = {1.0, 0.0, 0.0};
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, sideways, 0.1, &index, &distance));

        // Луч, не достающий до куба, обязан вернуть false.
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, direction, 0.1, &index, &distance));

        // Неверные аргументы — отказ, а не мусор в out-параметрах.
        EXPECT(!SimulationCubeFieldRaycast(NULL, origin, direction, 100.0, &index, &distance));
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, direction, 0.0, &index, &distance));
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, direction, -1.0, &index, &distance));
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, direction, 100.0, NULL, &distance));
        EXPECT(!SimulationCubeFieldRaycast(&field, origin, direction, 100.0, &index, NULL));
    }
    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

// Радиальный импульс: чем дальше тело, тем меньше прибавка, на границе —
// ноль. Импульс домножается на обратную массу, поэтому единичная масса даёт
// ровно переданный импульс в центре.
static void TestCubeRadialImpulse(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    bool initialized = world != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (initialized)
    {
        EXPECT(PlaceCube(&field, world, (const double[3]){0.5, 0.5, 19.0}));
        EXPECT(field.count == 1u);
        if (field.count == 1u)
        {
            double position[3];
            EXPECT(VoxelRigidBodyLocalPosition(&field.bodies[0], position));

            double before[3];
            EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], before));

            const double direction[3] = {0.0, 0.0, 1.0};
            uint32_t affected =
                SimulationCubeFieldApplyRadialImpulse(&field, position, direction, 1.0, 5.0);
            EXPECT(affected == 1u);

            double after[3];
            EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], after));
            EXPECT(fabs((after[2] - before[2]) - 5.0) < 1e-9);

            // Тело на границе радиуса не трогается: расстояние ровно radius
            // исключается строгим сравнением квадратов.
            double far[3] = {position[0] + 2.0, position[1], position[2]};
            before[2] = after[2];
            EXPECT(SimulationCubeFieldApplyRadialImpulse(&field, far, direction, 2.0, 5.0) == 0u);
            EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], after));
            EXPECT(after[2] == before[2]);

            // Половина радиуса — прибавка строго между нулём и полной.
            const double quarter = 0.5;
            before[2] = after[2];
            EXPECT(SimulationCubeFieldApplyRadialImpulse(&field, far, direction, 4.0, 8.0) == 1u);
            EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], after));
            double gain = after[2] - before[2];
            // distanceSquared = 4, radiusSquared = 16, falloff = 1 - 1/4 = 0.75.
            EXPECT(fabs(gain - 8.0 * 0.75) < 1e-9);
            (void)quarter;

            // Пробуждение: импульс обязан разбудить спящее тело.
            field.bodies[0].sleeping = true;
            EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], before));
            EXPECT(SimulationCubeFieldApplyRadialImpulse(&field, position, direction, 1.0, 1.0) ==
                   1u);
            EXPECT(!field.bodies[0].sleeping);

            // Неверные аргументы и выключенное поле — ноль, без ошибки.
            EXPECT(SimulationCubeFieldApplyRadialImpulse(NULL, position, direction, 1.0, 1.0) ==
                   0u);
            EXPECT(SimulationCubeFieldApplyRadialImpulse(&field, position, direction, 0.0, 1.0) ==
                   0u);
            EXPECT(SimulationCubeFieldApplyRadialImpulse(&field, position, direction, 1.0, 0.0) ==
                   0u);
        }
    }
    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

static void TestGrownCapacity(void)
{
    uint32_t capacity = 0u;
    uint32_t reallocations = 0u;
    for (uint32_t required = 1u; required <= 4460u; ++required)
    {
        uint32_t next = SimulationGrownCapacity(capacity, required);
        EXPECT(next >= required);
        if (next != capacity)
        {
            capacity = next;
            ++reallocations;
        }
    }
    EXPECT(capacity >= 4460u);
    EXPECT(reallocations <= 8u);

    EXPECT(SimulationGrownCapacity(64u, 1u) == 64u);
    EXPECT(SimulationGrownCapacity(64u, 64u) == 64u);
    EXPECT(SimulationGrownCapacity(64u, 65u) == 128u);
    EXPECT(SimulationGrownCapacity(0u, 10u) == 64u);
    EXPECT(SimulationGrownCapacity(1024u, 2000u) == 2048u);
    EXPECT(SimulationGrownCapacity(UINT32_MAX, UINT32_MAX) == UINT32_MAX);
}

// Умолчание обязано включать исполнитель на машине с несколькими ядрами,
// не занимая при этом все процессоры, а переменные окружения — строго
// переопределять и число потоков, и широкий отбор.
static void TestPhysicsDefaults(void)
{
    uint32_t cap = SIMULATION_PHYSICS_DEFAULT_THREAD_CAP;
    EXPECT(cap > 1u);
    EXPECT(cap < 16u);
    EXPECT(SimulationDefaultPhysicsThreads(16u) == SIMULATION_PHYSICS_DEFAULT_THREAD_CAP);
    EXPECT(SimulationDefaultPhysicsThreads(8u) == SIMULATION_PHYSICS_DEFAULT_THREAD_CAP);
    EXPECT(SimulationDefaultPhysicsThreads(4u) == 4u);
    EXPECT(SimulationDefaultPhysicsThreads(2u) == 2u);
    EXPECT(SimulationDefaultPhysicsThreads(1u) == 1u);
    EXPECT(SimulationDefaultPhysicsThreads(0u) == 1u);

    uint32_t threads = 0u;
    EXPECT(SimulationParsePhysicsThreads("4", &threads) && threads == 4u);
    EXPECT(SimulationParsePhysicsThreads("64", &threads) && threads == 64u);
    EXPECT(!SimulationParsePhysicsThreads("0", &threads));
    EXPECT(!SimulationParsePhysicsThreads("65", &threads));
    EXPECT(!SimulationParsePhysicsThreads("1foo", &threads));
    EXPECT(!SimulationParsePhysicsThreads("-1", &threads));
    EXPECT(!SimulationParsePhysicsThreads("", &threads));
    EXPECT(!SimulationParsePhysicsThreads(NULL, &threads));
    EXPECT(!SimulationParsePhysicsThreads("4", NULL));

    EXPECT(SimulationPhysicsThreads("3", 16u) == 3u);
    EXPECT(SimulationPhysicsThreads("1foo", 16u) == SIMULATION_PHYSICS_DEFAULT_THREAD_CAP);
    EXPECT(SimulationPhysicsThreads(NULL, 2u) == 2u);

    EXPECT(SimulationUseSpatialIndex("tree"));
    EXPECT(!SimulationUseSpatialIndex("grid"));
    EXPECT(!SimulationUseSpatialIndex("tree "));
    EXPECT(!SimulationUseSpatialIndex(NULL));
}

int main(void)
{
    TestFoundationWorld();
    TestInfiniteGround();
    TestGroundFillRegionEquivalence();
    TestFallingCubes();
    TestCubeTickReplay();
    TestCubeCapacityLimit();
    TestCubeSpawnBeyondOneThousand();
    TestWorldBlockEdit();
    TestConstructTopology();
    TestConstructRaycast();
    TestConstructFalls();
    TestConstructEditPoseAndVelocity();
    TestConstructGrowthBeyondOneThousand();
    TestConstructLargeShapeOwnership();
    TestConstructLargeSplitOwnership();
    TestConstructSixWaySplit();
    TestConstructFrameReplay(VOXEL_RIGID_SOLVER_CANONICAL);
    TestConstructFrameReplay(VOXEL_RIGID_SOLVER_COLORED);
    TestConstructPlateBudget();
    TestConstructCookedEditBudget();
    TestConstructAppendKeepsContacts();
    TestConstructRejectedEditPreservesBody();
    TestOriginShift();
    TestFrameTiming();
    TestCubeRaycast();
    TestCubeRadialImpulse();
    TestGrownCapacity();
    TestPhysicsDefaults();
    return failures == 0 ? 0 : 1;
}
