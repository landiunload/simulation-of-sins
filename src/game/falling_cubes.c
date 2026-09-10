#include "game/falling_cubes.h"

#include "world/world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define SIMULATION_CUBE_GRAVITY (-24.0)
#define SIMULATION_CUBE_MAX_STEPS 16u
#define SIMULATION_CUBE_MASS 1.0
#define SIMULATION_CUBE_FRICTION 0.55
#define SIMULATION_CUBE_RESTITUTION 0.05

// Точки появления идут по кругу золотым углом: два соседних по времени
// куба не должны оказаться один внутри другого. За десять миллисекунд
// предыдущий уходит вниз лишь на сантиметр, поэтому расходиться им
// приходится по горизонтали.
#define SIMULATION_CUBE_SPAWN_RADIUS 1.6
// cos/sin золотого угла зафиксированы как double. Рекуррентный поворот
// не зависит от реализации libm платформы и не вычисляет огромные углы.
#define SIMULATION_CUBE_SPAWN_COS (-0.7373688780783197)
#define SIMULATION_CUBE_SPAWN_SIN 0.6754902942615238

#define SIMULATION_CUBE_THROW_MINIMUM 0.5
#define SIMULATION_CUBE_THROW_MAXIMUM 2.5
// Начальная закрутка: куб теперь настоящее твёрдое тело, и падать плашмя
// ему незачем. Кувырок в воздухе — самый заметный признак того, что
// вращение работает.
#define SIMULATION_CUBE_SPIN_MAXIMUM 6.0

typedef struct CubeCollisionContext
{
    World *world;
} CubeCollisionContext;

// Параметры фиксированы ABI движка.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryBlockPhysics(void *context, int64_t x, int64_t y, int64_t z,
                              VoxelBlockPhysics *outBlock)
{
    const CubeCollisionContext *cubes = (const CubeCollisionContext *)context;
    BlockType block = WorldGetBlock(cubes->world, x, y, z);
    outBlock->flags = block != BLOCK_AIR ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = (float)SIMULATION_CUBE_FRICTION;
}

static uint64_t NextRandom(SimulationCubeField *field)
{
    // Обычный LCG: последовательность обязана быть воспроизводимой, иначе
    // тест «куб упал и лёг» зависел бы от прогона.
    field->randomState = field->randomState * 6364136223846793005ULL + 1442695040888963407ULL;
    return field->randomState >> 33;
}

static double NextUnitInterval(SimulationCubeField *field)
{
    return (double)(NextRandom(field) & 0xFFFFu) / 65535.0;
}

static double NextSigned(SimulationCubeField *field, double magnitude)
{
    return (NextUnitInterval(field) * 2.0 - 1.0) * magnitude;
}

static void RotateByQuaternion(const double rotation[4], const double value[3], double out[3])
{
    double axis[3] = {rotation[0], rotation[1], rotation[2]};
    double doubled[3];
    doubled[0] = 2.0 * (axis[1] * value[2] - axis[2] * value[1]);
    doubled[1] = 2.0 * (axis[2] * value[0] - axis[0] * value[2]);
    doubled[2] = 2.0 * (axis[0] * value[1] - axis[1] * value[0]);

    out[0] = value[0] + rotation[3] * doubled[0] + axis[1] * doubled[2] - axis[2] * doubled[1];
    out[1] = value[1] + rotation[3] * doubled[1] + axis[2] * doubled[0] - axis[0] * doubled[2];
    out[2] = value[2] + rotation[3] * doubled[2] + axis[0] * doubled[1] - axis[1] * doubled[0];
}

// Растит массив тел и согласованный с ним буфер шага. Тела — обычные
// значения: переезд в новую память их не ломает, указатели внутри чисел
// произвольной точности ведут наружу, а не внутрь структуры.
static bool GrowTo(SimulationCubeField *field, uint32_t capacity)
{
    if (capacity <= field->capacity)
    {
        return true;
    }
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(capacity);
    uint32_t cacheBytes = VoxelRigidContactCacheBytes(capacity);
    uint32_t indexBytes = VoxelRigidBroadphaseBytes(capacity);
    if (scratchBytes == 0u || cacheBytes == 0u || indexBytes == 0u)
    {
        return false;
    }

    VoxelRigidBody *bodies =
        (VoxelRigidBody *)realloc(field->bodies, (size_t)capacity * sizeof(*bodies));
    if (bodies == NULL)
    {
        return false;
    }
    field->bodies = bodies;

    void *scratch = realloc(field->scratch, scratchBytes);
    if (scratch == NULL)
    {
        return false;
    }
    field->scratch = scratch;
    field->scratchBytes = scratchBytes;
    void *cacheStorage = realloc(field->contactCache.storage, cacheBytes);
    if (cacheStorage == NULL)
    {
        return false;
    }
    field->contactCache.storage = cacheStorage;
    if (!VoxelRigidContactCacheInitialize(&field->contactCache, cacheStorage, capacity, cacheBytes))
    {
        return false;
    }
    void *indexStorage = realloc(field->broadphase.storage, indexBytes);
    if (indexStorage == NULL) return false;
    field->broadphase.storage = indexStorage;
    if (!VoxelRigidBroadphaseInitialize(&field->broadphase, indexStorage, capacity, indexBytes))
    {
        return false;
    }
    field->capacity = capacity;
    return true;
}

bool SimulationCubeFieldInit(SimulationCubeField *field)
{
    return SimulationCubeFieldInitWithSeed(field, UINT64_C(0x9E3779B97F4A7C15));
}

bool SimulationCubeFieldInitWithSeed(SimulationCubeField *field, uint64_t seed)
{
    if (field == NULL)
    {
        return false;
    }
    SimulationCubeField empty = {0};
    *field = empty;
    // Ноль запрещён как идентификатор тела.
    field->nextStableId = 1u;
    field->randomState = seed;
    field->spawnDirection[0] = 1.0;
    // The uniform cube pile is currently faster with the grid. The persistent
    // tree remains an explicit, physically equivalent benchmark option.
    field->useSpatialIndex = false;
    field->stepOptions.structSize = sizeof(field->stepOptions);

    VoxelRigidStepSettingsDefault(&field->settings);
    field->settings.gravity[2] = SIMULATION_CUBE_GRAVITY;

    // Начальная ёмкость произвольна: дальше массив растёт удвоением.
    return GrowTo(field, 256u);
}

void SimulationCubeFieldRelease(SimulationCubeField *field)
{
    if (field == NULL)
    {
        return;
    }
    for (uint32_t index = 0; index < field->count; ++index)
    {
        VoxelRigidBodyRelease(&field->bodies[index]);
    }
    field->count = 0u;
    free(field->bodies);
    field->bodies = NULL;
    field->capacity = 0u;
    free(field->scratch);
    field->scratch = NULL;
    field->scratchBytes = 0u;
    free(field->contactCache.storage);
    VoxelRigidContactCache emptyCache = {0};
    field->contactCache = emptyCache;
    free(field->broadphase.storage);
    VoxelRigidBroadphase emptyIndex = {0};
    field->broadphase = emptyIndex;
}

void SimulationCubeFieldRebase(SimulationCubeField *field, const int64_t blockShift[3])
{
    if (field == NULL || blockShift == NULL)
    {
        return;
    }
    for (uint32_t index = 0; index < field->count; ++index)
    {
        if (!VoxelRigidBodyTranslateBlocks(&field->bodies[index], blockShift))
        {
            field->failed = true;
            return;
        }
    }
}

static bool SpawnCube(SimulationCubeField *field, const double spawnPosition[3])
{
    double outwardX = field->spawnDirection[0];
    double outwardY = field->spawnDirection[1];
    field->spawnDirection[0] =
        outwardX * SIMULATION_CUBE_SPAWN_COS - outwardY * SIMULATION_CUBE_SPAWN_SIN;
    field->spawnDirection[1] =
        outwardX * SIMULATION_CUBE_SPAWN_SIN + outwardY * SIMULATION_CUBE_SPAWN_COS;
    ++field->spawnCounter;
    double throwSpeed =
        SIMULATION_CUBE_THROW_MINIMUM +
        NextUnitInterval(field) * (SIMULATION_CUBE_THROW_MAXIMUM - SIMULATION_CUBE_THROW_MINIMUM);

    VoxelRigidBodyDescription description = {0};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        description.halfExtent[axis] = SIMULATION_CUBE_HALF_EXTENT;
    }
    description.position[0] = spawnPosition[0] + outwardX * SIMULATION_CUBE_SPAWN_RADIUS;
    description.position[1] = spawnPosition[1] + outwardY * SIMULATION_CUBE_SPAWN_RADIUS;
    description.position[2] = spawnPosition[2];
    description.mass = SIMULATION_CUBE_MASS;
    description.friction = SIMULATION_CUBE_FRICTION;
    description.restitution = SIMULATION_CUBE_RESTITUTION;

    // Деспавна и потолка нет: место кончилось — массив растёт. Отказ
    // фиксируется как ошибка, а не молча пропущенный спавн в replay.
    if (field->count >= field->capacity)
    {
        uint32_t capacity = field->capacity * 2u;
        if (capacity > VOXEL_RIGID_MAX_BODIES)
        {
            capacity = VOXEL_RIGID_MAX_BODIES;
        }
        if (capacity <= field->count || !GrowTo(field, capacity))
        {
            return false;
        }
    }
    uint32_t slot = field->count;

    if (!VoxelRigidBodyInitialize(&field->bodies[slot], field->nextStableId, &description))
    {
        VoxelRigidBodyRelease(&field->bodies[slot]);
        return false;
    }

    const double velocity[3] = {outwardX * throwSpeed, outwardY * throwSpeed, 0.0};
    const double spin[3] = {
        NextSigned(field, SIMULATION_CUBE_SPIN_MAXIMUM),
        NextSigned(field, SIMULATION_CUBE_SPIN_MAXIMUM),
        NextSigned(field, SIMULATION_CUBE_SPIN_MAXIMUM),
    };
    if (!VoxelRigidBodyAddLinearVelocity(&field->bodies[slot], velocity) ||
        !VoxelRigidBodyAddAngularVelocity(&field->bodies[slot], spin))
    {
        VoxelRigidBodyRelease(&field->bodies[slot]);
        return false;
    }

    ++field->nextStableId;
    ++field->count;
    return true;
}

static bool ValidTickArguments(const SimulationCubeField *field, const World *world,
                               const double spawnPosition[3])
{
    if (field == NULL || world == NULL || spawnPosition == NULL || field->scratch == NULL ||
        field->failed || field->tickCount == UINT64_MAX)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!isfinite(spawnPosition[axis]))
        {
            return false;
        }
    }
    return true;
}

bool SimulationCubeFieldAdvanceTick(SimulationCubeField *field, World *world,
                                    const double spawnPosition[3])
{
    if (!ValidTickArguments(field, world, spawnPosition))
    {
        return false;
    }
    VoxelPhysicsConfigureThread();

    // 100 спавнов на 128 tick без погрешности накопления 0.01 в double.
    // Спавн всегда принадлежит tick, а не пачке перед всеми шагами кадра.
    field->spawnPhase += SIMULATION_CUBE_SPAWNS_PER_SECOND;
    while (field->spawnPhase >= SIMULATION_CUBE_TICKS_PER_SECOND)
    {
        field->spawnPhase -= SIMULATION_CUBE_TICKS_PER_SECOND;
        if (!SpawnCube(field, spawnPosition))
        {
            field->failed = true;
            return false;
        }
    }

    CubeCollisionContext context = {world};
    VoxelCollisionSource collision;
    collision.context = &context;
    collision.queryBlockPhysics = QueryBlockPhysics;
    collision.queryDynamicColliders = NULL;

    if (field->count != 0u)
    {
        VoxelRigidStepOptions options = field->stepOptions;
        options.contactCache = &field->contactCache;
        options.broadphase = field->useSpatialIndex ? &field->broadphase : NULL;
        bool stepped = VoxelRigidBodyStepEx(field->bodies, field->count, &collision,
                                           &field->settings, field->scratch, field->scratchBytes,
                                           &options);
        if (!stepped)
        {
            field->failed = true;
            return false;
        }
        VoxelRigidStepStats stats;
        if (VoxelRigidBodyReadStepStats(field->scratch, field->count, field->scratchBytes, &stats))
        {
            field->lastCandidatePairCount = stats.candidatePairCount;
            field->lastContactCount = stats.contactCount;
        }
    }
    ++field->tickCount;
    return true;
}

void SimulationCubeFieldUpdate(SimulationCubeField *field, World *world,
                               const double spawnPosition[3], double deltaSeconds)
{
    if (!ValidTickArguments(field, world, spawnPosition) || !isfinite(deltaSeconds) ||
        deltaSeconds < 0.0 || !isfinite(field->stepAccumulator + deltaSeconds))
    {
        return;
    }
    VoxelPhysicsConfigureThread();
    field->stepAccumulator += deltaSeconds;
    uint32_t steps = 0u;
    while (field->stepAccumulator >= SIMULATION_CUBE_STEP_SECONDS &&
           steps < SIMULATION_CUBE_MAX_STEPS)
    {
        if (!SimulationCubeFieldAdvanceTick(field, world, spawnPosition))
        {
            return;
        }
        field->stepAccumulator -= SIMULATION_CUBE_STEP_SECONDS;
        ++steps;
    }
}

uint32_t SimulationCubeFieldCount(const SimulationCubeField *field)
{
    return field != NULL ? field->count : 0u;
}

uint32_t SimulationCubeFieldAwakeCount(const SimulationCubeField *field)
{
    if (field == NULL)
    {
        return 0u;
    }
    uint32_t awake = 0u;
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        if (field->bodies[index].active && !field->bodies[index].sleeping)
        {
            ++awake;
        }
    }
    return awake;
}

uint32_t SimulationCubeFieldLastCandidatePairCount(const SimulationCubeField *field)
{
    return field != NULL ? field->lastCandidatePairCount : 0u;
}

uint32_t SimulationCubeFieldLastContactCount(const SimulationCubeField *field)
{
    return field != NULL ? field->lastContactCount : 0u;
}

uint32_t SimulationGrownCapacity(uint32_t current, uint32_t required)
{
    // Нижняя граница не даёт маленькому буферу расти по одному элементу.
    uint32_t capacity = current < 64u ? 64u : current;
    while (capacity < required)
    {
        if (capacity > UINT32_MAX / 2u)
        {
            return required;
        }
        capacity *= 2u;
    }
    return capacity;
}

bool SimulationCubeFieldPlacement(const SimulationCubeField *field, uint32_t index,
                                  double outOrigin[3], float outRotation[4])
{
    if (field == NULL || index >= field->count || outOrigin == NULL || outRotation == NULL)
    {
        return false;
    }
    const VoxelRigidBody *body = &field->bodies[index];
    double centre[3];
    if (!VoxelRigidBodyLocalPosition(body, centre))
    {
        return false;
    }

    // Меш куба идёт от локального нуля в плюс, а тело задано центром.
    // Рендер поворачивает меш вокруг точки привязки, поэтому привязка —
    // это центр минус повёрнутый полуразмер.
    const double half[3] = {SIMULATION_CUBE_HALF_EXTENT, SIMULATION_CUBE_HALF_EXTENT,
                            SIMULATION_CUBE_HALF_EXTENT};
    double rotated[3];
    RotateByQuaternion(body->orientation, half, rotated);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outOrigin[axis] = centre[axis] - rotated[axis];
    }
    for (int32_t component = 0; component < 4; ++component)
    {
        outRotation[component] = (float)body->orientation[component];
    }
    return true;
}
