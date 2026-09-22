#include "game/construct.h"
#include "game/construct_spawner.h"
#include "game/falling_cubes.h"
#include "game/ground_provider.h"

#include "numeric/infinite_coord.h"
#include "physics/rigid_body.h"
#include "world/world.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SIMULATION_CONSTRUCT_SPAWN_LIMIT == 1000u,
               "construct spawner contract: lifetime limit is 1000");
_Static_assert(SIMULATION_CONSTRUCT_SPAWN_BLOCKS == 9u,
               "construct spawner contract: shape uses 9 blocks");

// Позиция спавна повторяет приложение: центр сетки (0.5, 0.5) и низ над
// верхом пола. SIMULATION_CUBE_SPAWN_HEIGHT=18.0 объявлена в application.c;
// слой пола занимает [0, 1], поэтому локальная высота равна 1.0 + 18.0.
#define SOS_TEST_SPAWN_X 0.5
#define SOS_TEST_SPAWN_Y 0.5
#define SOS_TEST_SPAWN_HEIGHT 18.0
#define SOS_TEST_SPAWN_Z (1.0 + SOS_TEST_SPAWN_HEIGHT)

#define SOS_TEST_TICKS_TO_LIMIT 1280u
#define SOS_TEST_EXTRA_TICKS 64u
#define SOS_TEST_REPLAY_TICKS 128u
#define SOS_TEST_SOAK_TICKS 4096u
#define SOS_TEST_SPIN_LIMIT 0.35

static int failures;

#if defined(_WIN32)
#define TEST_FPRINTF fprintf_s
#else
#define TEST_FPRINTF fprintf
#endif

#define EXPECT(condition)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            TEST_FPRINTF(stderr, __FILE__ ":%d: expectation failed: " #condition "\n", __LINE__);  \
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

static void SpawnPosition(double out[3])
{
    out[0] = SOS_TEST_SPAWN_X;
    out[1] = SOS_TEST_SPAWN_Y;
    out[2] = SOS_TEST_SPAWN_Z;
}

static bool CallSpawner(SimulationCubeField *field, const double position[3])
{
    if (field == NULL || field->spawnBody == NULL)
    {
        return false;
    }
    return field->spawnBody(field->spawnContext, position);
}

static bool GBlock(int32_t x, int32_t z)
{
    if (x == 0 && z >= 0 && z <= 4)
    {
        return true;
    }
    if (x >= 1 && x <= 4 && z == 4)
    {
        return true;
    }
    return false;
}

// Форма — печатная «г»: стойка (0,0,0..4) и верхняя перекладина (1..4,0,4).
// Материал намеренно не проверяется: контракт задаёт только геометрию.
static bool BodyHasGShape(const ConstructBody *body)
{
    if (body == NULL || body->blockCount != SIMULATION_CONSTRUCT_SPAWN_BLOCKS)
    {
        return false;
    }
    bool present[5][5] = {{false}};
    uint32_t seen = 0u;
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        const int32_t x = body->blocks[index].local[0];
        const int32_t y = body->blocks[index].local[1];
        const int32_t z = body->blocks[index].local[2];
        if (y != 0 || x < 0 || x > 4 || z < 0 || z > 4 || !GBlock(x, z))
        {
            return false;
        }
        if (present[z][x])
        {
            return false;
        }
        present[z][x] = true;
        ++seen;
    }
    return seen == SIMULATION_CONSTRUCT_SPAWN_BLOCKS;
}

static ConstructBody *FindBodyByFieldIndex(ConstructSystem *system, uint32_t fieldIndex)
{
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        if (system->bodies[slot].active && system->bodies[slot].bodyIndex == fieldIndex)
        {
            return &system->bodies[slot];
        }
    }
    return NULL;
}

static void RotateQuaternion(const double rotation[4], const double value[3], double out[3])
{
    const double axis[3] = {rotation[0], rotation[1], rotation[2]};
    double doubled[3];
    doubled[0] = 2.0 * (axis[1] * value[2] - axis[2] * value[1]);
    doubled[1] = 2.0 * (axis[2] * value[0] - axis[0] * value[2]);
    doubled[2] = 2.0 * (axis[0] * value[1] - axis[1] * value[0]);
    out[0] = value[0] + rotation[3] * doubled[0] + axis[1] * doubled[2] - axis[2] * doubled[1];
    out[1] = value[1] + rotation[3] * doubled[1] + axis[2] * doubled[0] - axis[0] * doubled[2];
    out[2] = value[2] + rotation[3] * doubled[2] + axis[0] * doubled[1] - axis[1] * doubled[0];
}

// Минимальная вершина среди всех дочерних блоков всех построек. Дочерняя
// коробка растёт из origin в плюс, поэтому её вершины — origin + R*(t), где
// компоненты t равны 0 или 1. Учитываются именно повёрнутые вершины, а не
// только COM: тело на ребре/углу не должно проваливаться незамеченным.
static double ChildMinVertexZ(const ConstructSystem *system)
{
    double minimum = INFINITY;
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        const ConstructBody *body = &system->bodies[slot];
        if (!body->active)
        {
            continue;
        }
        for (uint32_t block = 0u; block < body->blockCount; ++block)
        {
            double origin[3];
            float rotation[4];
            if (!ConstructBlockPlacement(system, slot, block, origin, rotation))
            {
                return -INFINITY;
            }
            const double quaternion[4] = {(double)rotation[0], (double)rotation[1],
                                          (double)rotation[2], (double)rotation[3]};
            for (int32_t sx = 0; sx < 2; ++sx)
            {
                for (int32_t sy = 0; sy < 2; ++sy)
                {
                    for (int32_t sz = 0; sz < 2; ++sz)
                    {
                        const double corner[3] = {(double)sx, (double)sy, (double)sz};
                        double rotated[3];
                        RotateQuaternion(quaternion, corner, rotated);
                        const double z = origin[2] + rotated[2];
                        if (z < minimum)
                        {
                            minimum = z;
                        }
                    }
                }
            }
        }
    }
    return minimum;
}

// Проверяет форму всех 1000 тел и детерминированную сетку спавнера. Слот
// ConstructSystem совпадает с порядковым номером спавна, потому что слоты не
// освобождаются. Сетка: 10x10 с шагом 8; центрируется центр пятиширинной
// фигуры, поэтому блок (0,0,0) смещён на полразмера. Каждый следующий слой из
// 100 тел начинается на 6 блоков выше. Тела успели сделать один шаг физики до
// выключения, поэтому геометрия проверяется с запасом, а не побитово.
static void ValidateShapeAndGrid(const ConstructSystem *system, const double spawn[3],
                                 uint32_t expectedBodies)
{
    uint32_t active = 0u;
    for (uint32_t layer = 0u; layer < 10u; ++layer)
    {
        double offsets[100][2] = {{0.0}};
        bool seenPair[10][10] = {{false}};
        for (uint32_t within = 0u; within < 100u; ++within)
        {
            const uint32_t slot = layer * 100u + within;
            const ConstructBody *body = &system->bodies[slot];
            EXPECT(body->active);
            if (!body->active)
            {
                continue;
            }
            ++active;
            EXPECT(BodyHasGShape(body));
            double origin[3];
            float rotation[4];
            EXPECT(ConstructBlockPlacement(system, slot, 0u, origin, rotation));
            EXPECT(fabs(origin[2] - (spawn[2] + 6.0 * (double)layer)) < 0.05);
            const double quaternion[4] = {(double)rotation[0], (double)rotation[1],
                                          (double)rotation[2], (double)rotation[3]};
            // Пол пятого блока по X и половина блока по Y: этими смещениями
            // спавнер центрирует фигуру на точке сетки.
            const double half[3] = {2.5, 0.5, 2.5};
            double rotatedHalf[3];
            RotateQuaternion(quaternion, half, rotatedHalf);
            offsets[within][0] = origin[0] + rotatedHalf[0] - spawn[0];
            offsets[within][1] = origin[1] + rotatedHalf[1] - spawn[1];
        }

        double minX = INFINITY;
        double maxX = -INFINITY;
        double sumX = 0.0;
        double minY = INFINITY;
        double maxY = -INFINITY;
        double sumY = 0.0;
        for (uint32_t within = 0u; within < 100u; ++within)
        {
            if (offsets[within][0] < minX)
            {
                minX = offsets[within][0];
            }
            if (offsets[within][0] > maxX)
            {
                maxX = offsets[within][0];
            }
            if (offsets[within][1] < minY)
            {
                minY = offsets[within][1];
            }
            if (offsets[within][1] > maxY)
            {
                maxY = offsets[within][1];
            }
            sumX += offsets[within][0];
            sumY += offsets[within][1];
        }
        // 9 промежутков по 8 блоков и нулевое среднее — это и есть «10x10 с
        // шагом 8, центрированная на origin».
        EXPECT(fabs((maxX - minX) - 72.0) < 0.1);
        EXPECT(fabs((maxY - minY) - 72.0) < 0.1);
        EXPECT(fabs(sumX / 100.0) < 0.05);
        EXPECT(fabs(sumY / 100.0) < 0.05);

        bool seenX[10] = {false};
        bool seenY[10] = {false};
        for (uint32_t within = 0u; within < 100u; ++within)
        {
            const double row = (offsets[within][0] - minX) / 8.0;
            const double column = (offsets[within][1] - minY) / 8.0;
            const int32_t rowIndex = (int32_t)lround(row);
            const int32_t columnIndex = (int32_t)lround(column);
            EXPECT(fabs(row - (double)rowIndex) < 0.02);
            EXPECT(fabs(column - (double)columnIndex) < 0.02);
            EXPECT(rowIndex >= 0 && rowIndex < 10 && columnIndex >= 0 && columnIndex < 10);
            if (rowIndex >= 0 && rowIndex < 10 && columnIndex >= 0 && columnIndex < 10)
            {
                seenX[rowIndex] = true;
                seenY[columnIndex] = true;
                EXPECT(!seenPair[rowIndex][columnIndex]);
                seenPair[rowIndex][columnIndex] = true;
            }
        }
        for (uint32_t index = 0u; index < 10u; ++index)
        {
            EXPECT(seenX[index]);
            EXPECT(seenY[index]);
        }
    }
    EXPECT(active == expectedBodies);
}

static void ExpectFieldsEqual(const SimulationCubeField *first, const SimulationCubeField *second)
{
    EXPECT(first->tickCount == second->tickCount);
    EXPECT(first->count == second->count);
    EXPECT(first->spawnCounter == second->spawnCounter);
    EXPECT(first->spawnPhase == second->spawnPhase);
    EXPECT(first->randomState == second->randomState);
    EXPECT(first->nextStableId == second->nextStableId);
    EXPECT(first->spawnLimit == second->spawnLimit);
    EXPECT(first->spawningStopped == second->spawningStopped);
    EXPECT(first->failed == second->failed);
    EXPECT(first->lastCandidatePairCount == second->lastCandidatePairCount);
    EXPECT(first->lastContactCount == second->lastContactCount);
    EXPECT(first->contactCache.contactCount == second->contactCache.contactCount);
    EXPECT(first->contactCache.matchedContactCount == second->contactCache.matchedContactCount);
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
            EXPECT(left->halfExtent[axis] == right->halfExtent[axis]);
            EXPECT(left->inverseInertia[axis] == right->inverseInertia[axis]);
        }
        EXPECT(left->inverseMass == right->inverseMass);
        EXPECT(left->restitution == right->restitution);
        EXPECT(left->friction == right->friction);

        EXPECT(first->shapes[index].boxCount == second->shapes[index].boxCount);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        EXPECT(memcmp(first->shapes[index].inverseInertia, second->shapes[index].inverseInertia,
                      sizeof(first->shapes[index].inverseInertia)) == 0);
        if (first->shapes[index].boxCount == second->shapes[index].boxCount &&
            first->shapes[index].boxes != NULL && second->shapes[index].boxes != NULL)
        {
            for (uint32_t box = 0u; box < first->shapes[index].boxCount; ++box)
            {
                const VoxelRigidCompoundBox *leftBox = &first->shapes[index].boxes[box];
                const VoxelRigidCompoundBox *rightBox = &second->shapes[index].boxes[box];
                // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
                EXPECT(memcmp(leftBox, rightBox, sizeof(*leftBox)) == 0);
            }
        }
    }
}

static void ExpectConstructBodiesEqual(const ConstructSystem *first, const ConstructSystem *second)
{
    uint32_t activeFirst = 0u;
    uint32_t activeSecond = 0u;
    EXPECT(first->capacity == second->capacity);
    if (first->capacity != second->capacity)
        return;
    for (uint32_t slot = 0u; slot < first->capacity; ++slot)
    {
        if (first->bodies[slot].active)
        {
            ++activeFirst;
        }
        if (second->bodies[slot].active)
        {
            ++activeSecond;
        }
    }
    EXPECT(activeFirst == activeSecond);
    for (uint32_t slot = 0u; slot < first->capacity; ++slot)
    {
        const ConstructBody *left = &first->bodies[slot];
        const ConstructBody *right = &second->bodies[slot];
        EXPECT(left->active == right->active);
        if (!left->active || !right->active)
        {
            continue;
        }
        EXPECT(left->id == right->id);
        EXPECT(left->bodyIndex == right->bodyIndex);
        EXPECT(left->blockCount == right->blockCount);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            EXPECT(left->minimum[axis] == right->minimum[axis]);
            EXPECT(left->maximum[axis] == right->maximum[axis]);
            EXPECT(left->localCOM[axis] == right->localCOM[axis]);
        }
        if (left->blockCount == right->blockCount)
        {
            for (uint32_t block = 0u; block < left->blockCount; ++block)
            {
                // Поля сравниваются поимённо: у ConstructBlock есть хвостовой
                // padding, а он не часть состояния и не обязан совпадать.
                EXPECT(left->blocks[block].material == right->blocks[block].material);
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    EXPECT(left->blocks[block].local[axis] == right->blocks[block].local[axis]);
                }
            }
        }
    }
}

static uint64_t HashMix(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

// Сериализует bigint по значению (знак, длина и лимбы), а не по указателю:
// replay не должен зависеть от адресов. Точные double тоже попадают в хеш
// побайтово, включая знак нуля.
static uint64_t HashCoord(uint64_t hash, const InfiniteCoord *value)
{
    const int32_t sign = value->sign;
    const uint32_t limbCount = value->limbCount;
    hash = HashMix(hash, &sign, sizeof(sign));
    hash = HashMix(hash, &limbCount, sizeof(limbCount));
    if (limbCount > 0u && value->limbs != NULL)
    {
        hash = HashMix(hash, value->limbs, (size_t)limbCount * sizeof(uint64_t));
    }
    return hash;
}

static uint64_t HashBody(uint64_t hash, const VoxelRigidBody *body)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        hash = HashCoord(hash, &body->position[axis]);
        hash = HashCoord(hash, &body->linearVelocity[axis]);
        hash = HashCoord(hash, &body->angularVelocity[axis]);
    }
    hash = HashMix(hash, body->orientation, sizeof(body->orientation));
    hash = HashMix(hash, body->halfExtent, sizeof(body->halfExtent));
    hash = HashMix(hash, body->inverseInertia, sizeof(body->inverseInertia));
    hash = HashMix(hash, &body->inverseMass, sizeof(body->inverseMass));
    hash = HashMix(hash, &body->restitution, sizeof(body->restitution));
    hash = HashMix(hash, &body->friction, sizeof(body->friction));
    hash = HashMix(hash, &body->stableId, sizeof(body->stableId));
    hash = HashMix(hash, &body->active, sizeof(body->active));
    hash = HashMix(hash, &body->sleepCounter, sizeof(body->sleepCounter));
    hash = HashMix(hash, &body->sleeping, sizeof(body->sleeping));
    return hash;
}

static uint64_t FieldHash(const SimulationCubeField *field)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    if (field == NULL)
    {
        return hash;
    }
    hash = HashMix(hash, &field->tickCount, sizeof(field->tickCount));
    hash = HashMix(hash, &field->count, sizeof(field->count));
    hash = HashMix(hash, &field->spawnCounter, sizeof(field->spawnCounter));
    hash = HashMix(hash, &field->randomState, sizeof(field->randomState));
    hash = HashMix(hash, &field->spawnPhase, sizeof(field->spawnPhase));
    hash = HashMix(hash, &field->nextStableId, sizeof(field->nextStableId));
    hash = HashMix(hash, &field->spawnLimit, sizeof(field->spawnLimit));
    hash = HashMix(hash, field->spawnDirection, sizeof(field->spawnDirection));
    hash = HashMix(hash, &field->lastCandidatePairCount, sizeof(field->lastCandidatePairCount));
    hash = HashMix(hash, &field->lastContactCount, sizeof(field->lastContactCount));
    hash =
        HashMix(hash, &field->contactCache.contactCount, sizeof(field->contactCache.contactCount));
    hash = HashMix(hash, &field->contactCache.matchedContactCount,
                   sizeof(field->contactCache.matchedContactCount));
    hash = HashMix(hash, &field->failed, sizeof(field->failed));
    hash = HashMix(hash, &field->spawningStopped, sizeof(field->spawningStopped));
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        hash = HashBody(hash, &field->bodies[index]);
        if (field->shapes != NULL)
        {
            const VoxelRigidCompoundShape *shape = &field->shapes[index];
            hash = HashMix(hash, &shape->boxCount, sizeof(shape->boxCount));
            hash = HashMix(hash, shape->inverseInertia, sizeof(shape->inverseInertia));
            if (shape->boxCount > 0u && shape->boxes != NULL)
            {
                hash = HashMix(hash, shape->boxes,
                               (size_t)shape->boxCount * sizeof(VoxelRigidCompoundBox));
            }
        }
    }
    return hash;
}

static double OrientationError(const SimulationCubeField *field)
{
    double worst = 0.0;
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        const double *q = field->bodies[index].orientation;
        const double norm = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!isfinite(norm))
            return INFINITY;
        const double error = fabs(norm - 1.0);
        if (error > worst)
        {
            worst = error;
        }
    }
    return worst;
}

static void TestAttachContract(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = (ConstructSystem *)calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        WorldDestroy(world);
        return;
    }

    EXPECT(!SimulationConstructSpawnerAttach(NULL));

    system->world = NULL;
    system->field = &field;
    EXPECT(!SimulationConstructSpawnerAttach(system));

    system->world = world;
    system->field = NULL;
    EXPECT(!SimulationConstructSpawnerAttach(system));

    system->world = world;
    system->field = &field;
    field.count = 1u;
    EXPECT(!SimulationConstructSpawnerAttach(system));
    field.count = 0u;
    field.spawnCounter = 1u;
    EXPECT(!SimulationConstructSpawnerAttach(system));
    field.spawnCounter = 0u;

    EXPECT(SimulationConstructSpawnerAttach(system));
    EXPECT(field.spawnBody != NULL);
    EXPECT(field.spawnContext == (void *)system);
    EXPECT(field.spawnLimit == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    EXPECT(field.count == 0u);
    EXPECT(field.spawnCounter == 0u);

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

static void TestSpawnerShapeGridAndInterior(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = (ConstructSystem *)calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    EXPECT(SimulationConstructSpawnerAttach(system));

    double spawn[3];
    SpawnPosition(spawn);
    EXPECT(CallSpawner(&field, spawn));
    EXPECT(field.count == 1u);
    // Прямой вызов минует AdvanceTick, поэтому счётчик не растёт: его ведёт
    // тик, а колбэк только пишет тело по ordinal = spawnCounter.
    EXPECT(field.spawnCounter == 0u);
    EXPECT(!field.failed);
    ConstructBody *body = FindBodyByFieldIndex(system, 0u);
    EXPECT(body != NULL);
    if (body != NULL)
    {
        EXPECT(BodyHasGShape(body));
    }

    // Прямой вызов колбэка идёт без шага физики: у тела не должно быть
    // линейного броска, а начальная закрутка ограничена контрактом.
    double velocity[3];
    EXPECT(VoxelRigidBodyLinearVelocity(&field.bodies[0], velocity));
    EXPECT(fabs(velocity[0]) < 1e-12 && fabs(velocity[1]) < 1e-12 && fabs(velocity[2]) < 1e-12);
    double angular[3];
    EXPECT(VoxelRigidBodyAngularVelocity(&field.bodies[0], angular));
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        EXPECT(fabs(angular[axis]) <= SOS_TEST_SPIN_LIMIT + 1e-9);
    }
    EXPECT(VoxelRigidBodyAngularSpeed(&field.bodies[0]) <= SOS_TEST_SPIN_LIMIT * sqrt(3.0) + 1e-9);

    // Внутренность «г» пуста: луч сквозь вырез под перекладиной не находит
    // блок, а луч в стойку — находит. Проверка выполняется до первого шага.
    // Смещения и направления берутся в системе тела и поворачиваются его
    // кватернионом, чтобы проверка не зависела от начальной ориентации.
    double origin[3];
    float rotation[4];
    EXPECT(ConstructBlockPlacement(system, 0u, 0u, origin, rotation));
    const double quaternion[4] = {(double)rotation[0], (double)rotation[1], (double)rotation[2],
                                  (double)rotation[3]};
    const double localEmpty[3] = {2.5, 0.5, 2.5};
    double emptyOffset[3];
    RotateQuaternion(quaternion, localEmpty, emptyOffset);
    const double empty[3] = {origin[0] + emptyOffset[0], origin[1] + emptyOffset[1],
                             origin[2] + emptyOffset[2]};
    const double localDirections[3][3] = {{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}};
    ConstructRaycastHit hit = {0};
    double upward[3];
    double rightward[3];
    double leftward[3];
    RotateQuaternion(quaternion, localDirections[0], upward);
    RotateQuaternion(quaternion, localDirections[1], rightward);
    RotateQuaternion(quaternion, localDirections[2], leftward);
    EXPECT(!ConstructRaycast(system, empty, upward, 0.4, &hit));
    EXPECT(!ConstructRaycast(system, empty, rightward, 0.4, &hit));
    EXPECT(ConstructRaycast(system, empty, leftward, 2.0, &hit));
    EXPECT(hit.distance > 1.0 && hit.distance < 2.0);

    // Позиция сетки читает spawnCounter до инкремента: счётчик 100 обязан
    // дать первый блок второго слоя, то есть на 6 блоков выше.
    field.spawnCounter = 100u;
    EXPECT(CallSpawner(&field, spawn));
    EXPECT(field.count == 2u);
    EXPECT(field.spawnCounter == 100u);
    double secondOrigin[3];
    float secondRotation[4];
    EXPECT(ConstructBlockPlacement(system, 1u, 0u, secondOrigin, secondRotation));
    EXPECT(fabs(secondOrigin[2] - (spawn[2] + 6.0)) < 1e-9);
    EXPECT(ConstructActiveBodyCount(system) == 2u);

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// Дешёвая проверка логики лимита: тела после спавна сразу выключаются, поэтому
// шаг физики остаётся дешёвым. Это не подмена физики в --soak — там все тела
// настоящие и не выключаются.
static void TestSpawnerLimitBookkeeping(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    ConstructSystem *system = (ConstructSystem *)calloc(1u, sizeof(*system));
    bool initialized = world != NULL && system != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        free(system);
        WorldDestroy(world);
        return;
    }
    ConstructSystemInit(system, world, &field);
    EXPECT(SimulationConstructSpawnerAttach(system));
    EXPECT(field.spawnLimit == SIMULATION_CONSTRUCT_SPAWN_LIMIT);

    double spawn[3];
    SpawnPosition(spawn);
    for (uint32_t tick = 0u; tick < SOS_TEST_TICKS_TO_LIMIT && !field.failed; ++tick)
    {
        const uint32_t before = field.count;
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, spawn));
        for (uint32_t index = before; index < field.count; ++index)
        {
            field.bodies[index].active = false;
        }
    }
    EXPECT(!field.failed);
    EXPECT(field.tickCount == SOS_TEST_TICKS_TO_LIMIT);
    EXPECT(field.count == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    EXPECT(field.spawnCounter == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    EXPECT(field.spawnPhase == 0u);
    EXPECT(ConstructActiveBodyCount(system) == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    ValidateShapeAndGrid(system, spawn, SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    const double minChildZ = ChildMinVertexZ(system);
    EXPECT(isfinite(minChildZ) && minChildZ > 0.0);

    // За лимитом счётчик, PRNG и число тел заморожены, а tick продолжает идти.
    const uint64_t counter = field.spawnCounter;
    const uint64_t random = field.randomState;
    const uint32_t count = field.count;
    for (uint32_t tick = 0u; tick < SOS_TEST_EXTRA_TICKS; ++tick)
    {
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, spawn));
    }
    EXPECT(field.count == count);
    EXPECT(field.spawnCounter == counter);
    EXPECT(field.randomState == random);
    EXPECT(field.tickCount == SOS_TEST_TICKS_TO_LIMIT + SOS_TEST_EXTRA_TICKS);
    EXPECT(!field.failed);

    ConstructSystemReset(system);
    SimulationCubeFieldRelease(&field);
    free(system);
    WorldDestroy(world);
}

// Без Attach поле остаётся прежним стресс-спавнером без потолка: счётчик
// обязан перешагнуть лимит постройки, иначе старая механика молча сломалась.
static void TestLegacyUnattachedUnlimited(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField field = {0};
    bool initialized = world != NULL && SimulationCubeFieldInit(&field);
    EXPECT(initialized);
    if (!initialized)
    {
        WorldDestroy(world);
        return;
    }
    EXPECT(field.spawnBody == NULL);
    for (uint32_t index = 0u; index < 1001u; ++index)
    {
        field.spawnPhase = SIMULATION_CUBE_TICKS_PER_SECOND - SIMULATION_CUBE_SPAWNS_PER_SECOND;
        uint32_t row = index / 40u;
        const double spawn[3] = {(double)(index % 40u) * 3.0, (double)row * 3.0, 10.0};
        EXPECT(SimulationCubeFieldAdvanceTick(&field, world, spawn));
        if (field.failed)
        {
            break;
        }
        field.bodies[field.count - 1u].active = false;
    }
    EXPECT(!field.failed);
    EXPECT(field.spawnBody == NULL);
    EXPECT(field.spawnCounter == 1001u);
    SimulationCubeFieldRelease(&field);
    WorldDestroy(world);
}

static void TestSpawnerReplay(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField fields[2] = {{0}};
    ConstructSystem *systems[2] = {NULL, NULL};
    bool initialized = world != NULL;
    for (uint32_t run = 0u; run < 2u && initialized; ++run)
    {
        if (!SimulationCubeFieldInitWithSeed(&fields[run], UINT64_C(0x5EED5EED5EED5EED)))
        {
            initialized = false;
            break;
        }
        systems[run] = (ConstructSystem *)calloc(1u, sizeof(*systems[run]));
        if (systems[run] == NULL)
        {
            initialized = false;
        }
    }
    EXPECT(initialized);
    if (initialized)
    {
        double spawn[3];
        SpawnPosition(spawn);
        for (uint32_t run = 0u; run < 2u; ++run)
        {
            ConstructSystemInit(systems[run], world, &fields[run]);
            EXPECT(SimulationConstructSpawnerAttach(systems[run]));
        }
        for (uint32_t tick = 0u; tick < SOS_TEST_REPLAY_TICKS; ++tick)
        {
            EXPECT(SimulationCubeFieldAdvanceTick(&fields[0], world, spawn));
            EXPECT(SimulationCubeFieldAdvanceTick(&fields[1], world, spawn));
            if (fields[0].failed || fields[1].failed)
            {
                break;
            }
        }
        EXPECT(!fields[0].failed);
        EXPECT(!fields[1].failed);
        EXPECT(fields[0].count == 100u);
        EXPECT(fields[0].spawnCounter == 100u);
        EXPECT(OrientationError(&fields[0]) < 1e-3);
        ExpectFieldsEqual(&fields[0], &fields[1]);
        ExpectConstructBodiesEqual(systems[0], systems[1]);
        EXPECT(FieldHash(&fields[0]) == FieldHash(&fields[1]));
        const double minChildZ = ChildMinVertexZ(systems[0]);
        EXPECT(isfinite(minChildZ) && minChildZ > 0.0);
    }
    for (uint32_t run = 0u; run < 2u; ++run)
    {
        if (systems[run] != NULL)
        {
            ConstructSystemReset(systems[run]);
            free(systems[run]);
        }
        SimulationCubeFieldRelease(&fields[run]);
    }
    WorldDestroy(world);
}

static void RunSoak(void)
{
    SimulationGroundProvider ground;
    World *world = CreateGroundWorld(&ground);
    SimulationCubeField fields[2] = {{0}};
    ConstructSystem *systems[2] = {NULL, NULL};
    bool initialized = world != NULL;
    for (uint32_t run = 0u; run < 2u && initialized; ++run)
    {
        if (!SimulationCubeFieldInitWithSeed(&fields[run], UINT64_C(0x50AC50AC50AC50AC)))
        {
            initialized = false;
            break;
        }
        systems[run] = (ConstructSystem *)calloc(1u, sizeof(*systems[run]));
        if (systems[run] == NULL)
        {
            initialized = false;
        }
    }
    EXPECT(initialized);
    if (!initialized)
    {
        TEST_FPRINTF(stderr, "soak: setup failed\n");
        for (uint32_t run = 0u; run < 2u; ++run)
        {
            if (systems[run] != NULL)
            {
                ConstructSystemReset(systems[run]);
                free(systems[run]);
            }
            SimulationCubeFieldRelease(&fields[run]);
        }
        WorldDestroy(world);
        return;
    }

    double spawn[3];
    SpawnPosition(spawn);
    uint64_t randomAtLimit[2] = {0u, 0u};
    bool reachedLimit[2] = {false, false};
    bool stepFailed = false;
    double sampledMinZ = INFINITY;
    for (uint32_t run = 0u; run < 2u; ++run)
    {
        ConstructSystemInit(systems[run], world, &fields[run]);
        EXPECT(SimulationConstructSpawnerAttach(systems[run]));
    }
    for (uint32_t run = 0u; run < 2u && !stepFailed; ++run)
    {
        for (uint32_t tick = 0u; tick < SOS_TEST_SOAK_TICKS; ++tick)
        {
            if (!SimulationCubeFieldAdvanceTick(&fields[run], world, spawn))
            {
                TEST_FPRINTF(stderr, "soak: run %u failed at tick %u\n", run, tick);
                stepFailed = true;
                break;
            }
            if (!reachedLimit[run] && fields[run].spawnCounter == SIMULATION_CONSTRUCT_SPAWN_LIMIT)
            {
                randomAtLimit[run] = fields[run].randomState;
                reachedLimit[run] = true;
            }
            if ((tick + 1u) % 128u == 0u)
            {
                double minimum = ChildMinVertexZ(systems[run]);
                EXPECT(isfinite(minimum) && minimum > 0.90);
                EXPECT(OrientationError(&fields[run]) < 1e-3);
                if (minimum < sampledMinZ)
                    sampledMinZ = minimum;
            }
        }
    }

    for (uint32_t run = 0u; run < 2u; ++run)
    {
        EXPECT(reachedLimit[run]);
        EXPECT(!fields[run].failed);
        EXPECT(fields[run].count == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
        EXPECT(fields[run].spawnCounter == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    }
    ExpectFieldsEqual(&fields[0], &fields[1]);
    ExpectConstructBodiesEqual(systems[0], systems[1]);
    uint64_t hash0 = FieldHash(&fields[0]);
    uint64_t hash1 = FieldHash(&fields[1]);
    EXPECT(hash0 == hash1);

    // Дальше лимит уже достигнут: спавнов нет, но физика обязана продолжать
    // шагать, не сбрасывая счётчик, PRNG и тела.
    const uint32_t countAfter = fields[0].count;
    const uint32_t countAfter1 = fields[1].count;
    for (uint32_t tick = 0u; tick < SOS_TEST_EXTRA_TICKS; ++tick)
    {
        EXPECT(SimulationCubeFieldAdvanceTick(&fields[0], world, spawn));
        EXPECT(SimulationCubeFieldAdvanceTick(&fields[1], world, spawn));
    }
    EXPECT(fields[0].count == countAfter);
    EXPECT(fields[1].count == countAfter1);
    EXPECT(fields[0].spawnCounter == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    EXPECT(fields[1].spawnCounter == SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    EXPECT(fields[0].randomState == randomAtLimit[0]);
    EXPECT(fields[1].randomState == randomAtLimit[1]);
    ExpectFieldsEqual(&fields[0], &fields[1]);
    EXPECT(FieldHash(&fields[0]) == FieldHash(&fields[1]));

    double minChildZ = ChildMinVertexZ(systems[0]);
    double maxLinear = 0.0;
    double maxAngular = 0.0;
    for (uint32_t index = 0u; index < fields[0].count; ++index)
    {
        const double linear = VoxelRigidBodyLinearSpeed(&fields[0].bodies[index]);
        const double angular = VoxelRigidBodyAngularSpeed(&fields[0].bodies[index]);
        EXPECT(isfinite(linear) && linear < 100.0);
        EXPECT(isfinite(angular) && angular < 100.0);
        if (linear > maxLinear)
        {
            maxLinear = linear;
        }
        if (angular > maxAngular)
        {
            maxAngular = angular;
        }
    }
    const double orientationError = OrientationError(&fields[0]);
    const uint32_t childBlocks = fields[0].count * SIMULATION_CONSTRUCT_SPAWN_BLOCKS;

    // Rebase-free run: позиция каждого тела обязана переводиться в double.
    for (uint32_t index = 0u; index < fields[0].count; ++index)
    {
        double position[3];
        const bool finite = VoxelRigidBodyLocalPosition(&fields[0].bodies[index], position) &&
                            isfinite(position[0]) && isfinite(position[1]) && isfinite(position[2]);
        EXPECT(finite);
    }

    // Conservative bounds: тела не проваливаются сквозь пол, кватернион
    // остаётся почти единичным, скорости конечны.
    EXPECT(minChildZ > 0.90);
    EXPECT(isfinite(minChildZ));
    EXPECT(isfinite(maxLinear) && maxLinear >= 0.0);
    EXPECT(isfinite(maxAngular) && maxAngular >= 0.0);
    EXPECT(orientationError < 1e-3);
    EXPECT(!fields[0].failed);
    EXPECT(!fields[1].failed);

    printf("construct_spawner soak: 2 runs x %u ticks, all bodies real and simulated\n",
           SOS_TEST_SOAK_TICKS);
    printf("  (no deactivation, no teleport, no fake sleep, no dropped physics)\n");
    uint32_t primitives = 0u;
    EXPECT(SimulationCubeFieldPrimitiveCount(&fields[0], &primitives));
    EXPECT(primitives == 2u * SIMULATION_CONSTRUCT_SPAWN_LIMIT);
    printf("  spawned constructs=%u child blocks=%u collision boxes=%u (simulated)\n",
           fields[0].count, childBlocks, primitives);
    printf("  field.count=%u active=%u awake=%u contacts=%u\n", fields[0].count,
           ConstructActiveBodyCount(systems[0]), SimulationCubeFieldAwakeCount(&fields[0]),
           SimulationCubeFieldLastContactCount(&fields[0]));
    printf("  min child vertex z=%.6f\n", minChildZ);
    printf("  min child vertex z across sampled ticks=%.6f\n", sampledMinZ);
    printf("  max linear speed=%.6f max angular speed=%.6f\n", maxLinear, maxAngular);
    printf("  max orientation norm error=%.3e\n", orientationError);
    printf("  replay hash run0=%" PRIu64 " run1=%" PRIu64 " match=%s\n", hash0, hash1,
           hash0 == hash1 ? "yes" : "no");
    printf("  errors=%d\n", failures);

    for (uint32_t run = 0u; run < 2u; ++run)
    {
        ConstructSystemReset(systems[run]);
        free(systems[run]);
        SimulationCubeFieldRelease(&fields[run]);
    }
    WorldDestroy(world);
}

int main(int argc, char **argv)
{
    bool soak = false;
    for (int32_t index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--soak") == 0)
        {
            soak = true;
        }
        else
        {
            TEST_FPRINTF(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }

    TestAttachContract();
    TestSpawnerShapeGridAndInterior();
    TestSpawnerLimitBookkeeping();
    TestLegacyUnattachedUnlimited();
    TestSpawnerReplay();
    if (soak)
    {
        RunSoak();
    }

    if (failures == 0)
    {
        printf("construct_spawner: %s checks passed\n", soak ? "default+soak" : "default");
        return 0;
    }
    TEST_FPRINTF(stderr, "construct_spawner: %d expectation(s) failed\n", failures);
    return 1;
}
