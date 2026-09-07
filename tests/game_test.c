#include "game/falling_cubes.h"
#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/ground_provider.h"
#include "game/rebase_policy.h"

#include "world/world.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
        for (uint32_t batch = 0u; batch < 16u; ++batch)
        {
            for (uint32_t tick = 0u; tick < 16u; ++tick)
            {
                EXPECT(SimulationCubeFieldAdvanceTick(&reference, world, spawn));
            }
            // Разные длины render-кадров, включая кадр без physics tick.
            SimulationCubeFieldUpdate(&frames, world, spawn, SIMULATION_CUBE_STEP_SECONDS * 0.25);
            SimulationCubeFieldUpdate(&frames, world, spawn, SIMULATION_CUBE_STEP_SECONDS * 15.75);
            ExpectCubeStatesEqual(&reference, &frames);
        }
        EXPECT(reference.tickCount == 256u);
        EXPECT(reference.count == 200u);

        // Две секунды долга не теряются при ограничении 16 шагов за кадр.
        SimulationCubeFieldUpdate(&backlog, world, spawn, 2.0);
        EXPECT(backlog.tickCount == 16u);
        EXPECT(backlog.stepAccumulator == 2.0 - 16.0 * SIMULATION_CUBE_STEP_SECONDS);
        for (uint32_t batch = 1u; batch < 16u; ++batch)
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

int main(void)
{
    TestFoundationWorld();
    TestInfiniteGround();
    TestFallingCubes();
    TestCubeTickReplay();
    TestOriginShift();
    TestFrameTiming();
    return failures == 0 ? 0 : 1;
}
