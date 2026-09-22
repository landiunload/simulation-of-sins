#include "game/falling_cubes.h"

#include "world/world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

// Точка входа луча в коробку в её собственной системе координат. origin и
// direction туда уже переведены поворотом, direction единичный. Пересечение
// ищется плитами (slab): параметр входа не отрицателен, поэтому старт внутри
// коробки даёт ноль, а не мнимое попадание за спиной.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool RayBoxDistance(const double origin[3], const double direction[3],
                           const double halfExtent[3], double maximumDistance,
                           double *outDistance)
{
    double entry = 0.0;
    double exit = maximumDistance;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] > -1e-9 && direction[axis] < 1e-9)
        {
            if (origin[axis] < -halfExtent[axis] || origin[axis] > halfExtent[axis])
            {
                return false;
            }
            continue;
        }
        double inverse = 1.0 / direction[axis];
        double near = (-halfExtent[axis] - origin[axis]) * inverse;
        double far = (halfExtent[axis] - origin[axis]) * inverse;
        if (near > far)
        {
            double swap = near;
            near = far;
            far = swap;
        }
        if (near > entry)
        {
            entry = near;
        }
        if (far < exit)
        {
            exit = far;
        }
        if (entry > exit)
        {
            return false;
        }
    }
    *outDistance = entry;
    return true;
}

static bool FiniteVector3(const double value[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!isfinite(value[axis]))
        {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool SimulationCubeFieldRaycast(const SimulationCubeField *field, const double origin[3],
                                const double direction[3], double maximumDistance,
                                uint32_t *outIndex, double *outDistance)
{
    if (field == NULL || origin == NULL || direction == NULL || outIndex == NULL ||
        outDistance == NULL || !(maximumDistance > 0.0) || !isfinite(maximumDistance) ||
        !FiniteVector3(origin) || !FiniteVector3(direction))
    {
        return false;
    }
    bool found = false;
    double nearest = maximumDistance;
    uint32_t nearestIndex = 0u;
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        // Envelope составного тела — не его форма: луч кубов не должен
        // попадать в дырку L-фигуры. Постройки разбирает ConstructRaycast.
        if (field->shapes != NULL && field->shapes[index].boxCount != 0u)
        {
            continue;
        }
        const VoxelRigidBody *body = &field->bodies[index];
        if (!body->active)
        {
            continue;
        }
        double position[3];
        if (!VoxelRigidBodyLocalPosition(body, position))
        {
            continue;
        }
        const double offset[3] = {origin[0] - position[0], origin[1] - position[1],
                                  origin[2] - position[2]};
        // Мировой вектор переводится в систему тела обратным поворотом:
        // кватернион хранит поворот тела наружу, значит внутрь ведёт сопряжение.
        const double conjugate[4] = {-body->orientation[0], -body->orientation[1],
                                     -body->orientation[2], body->orientation[3]};
        double localOrigin[3];
        double localDirection[3];
        RotateByQuaternion(conjugate, offset, localOrigin);
        RotateByQuaternion(conjugate, direction, localDirection);
        double distance = 0.0;
        if (RayBoxDistance(localOrigin, localDirection, body->halfExtent, nearest, &distance) &&
            (!found || distance < nearest))
        {
            nearest = distance;
            nearestIndex = index;
            found = true;
        }
    }
    if (!found)
    {
        return false;
    }
    *outIndex = nearestIndex;
    *outDistance = nearest;
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
uint32_t SimulationCubeFieldApplyRadialImpulse(SimulationCubeField *field,
                                               const double center[3],
                                               const double direction[3],
                                               double radius, double impulse)
{
    if (field == NULL || center == NULL || direction == NULL || field->failed ||
        !(radius > 0.0) || !(impulse > 0.0) || !isfinite(radius) || !isfinite(impulse) ||
        !FiniteVector3(center) || !FiniteVector3(direction))
    {
        return 0u;
    }
    // Квадрат расстояния вместо расстояния: форма спада та же, а sqrt не
    // требуется — переносимое ядро остаётся без libm. Переполнение квадрата
    // радиуса — отказ, а не молчаливое «все тела в шаре».
    double radiusSquared = radius * radius;
    if (!isfinite(radiusSquared))
    {
        return 0u;
    }
    uint32_t affected = 0u;
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        VoxelRigidBody *body = &field->bodies[index];
        if (!body->active || body->inverseMass == 0.0)
        {
            continue;
        }
        double position[3];
        if (!VoxelRigidBodyLocalPosition(body, position))
        {
            continue;
        }
        const double offset[3] = {position[0] - center[0], position[1] - center[1],
                                  position[2] - center[2]};
        double distanceSquared =
            offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
        if (distanceSquared >= radiusSquared)
        {
            continue;
        }
        double falloff = 1.0 - distanceSquared / radiusSquared;
        double delta[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            delta[axis] = direction[axis] * impulse * falloff * body->inverseMass;
        }
        if (!VoxelRigidBodyAddLinearVelocity(body, delta))
        {
            field->failed = true;
            return affected;
        }
        VoxelRigidBodyWake(body);
        ++affected;
    }
    return affected;
}

// Растит массив тел и согласованный с ним буфер шага. Тела — обычные
// значения: переезд в новую память их не ломает, указатели внутри чисел
// произвольной точности ведут наружу, а не внутрь структуры.
static bool GrowTo(SimulationCubeField *field, uint32_t capacity)
{
    uint32_t persistentRequired = field->contactCache.bodyCapacity == 0u
                                      ? capacity
                                      : field->count + 1u;
    bool persistentGrowth = persistentRequired > field->contactCache.bodyCapacity ||
                            persistentRequired > field->broadphase.bodyCapacity;
    if (capacity <= field->capacity && !persistentGrowth)
    {
        return true;
    }
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(capacity);
    // Keep persistent storage on its original power-of-two thresholds. Resetting
    // warm-start history at the extra scratch growth points changes the replay.
    // Persistent caches keep their original reset schedule.  Transient body
    // storage may grow between those thresholds, so do not use `capacity` as
    // the cache requirement after initialization: only the next body slot can
    // make a cache/index resize necessary.
    uint32_t persistentCapacity =
        SimulationGrownCapacity(field->contactCache.bodyCapacity, persistentRequired);
    uint32_t cacheBytes = VoxelRigidContactCacheBytes(persistentCapacity);
    uint32_t indexBytes = VoxelRigidBroadphaseBytes(persistentCapacity);
    if (scratchBytes == 0u || cacheBytes == 0u || indexBytes == 0u ||
        (uint64_t)capacity * sizeof(VoxelRigidBody) > SIZE_MAX ||
        (uint64_t)capacity * sizeof(VoxelRigidCompoundShape) > SIZE_MAX)
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

    // Формы переезжают вместе с телами. Их boxes указывают в отдельные
    // аллокации ConstructBody, поэтому realloc переносит только дескрипторы и
    // не рвёт связь. Новые слоты обнуляются: до явной записи это обычная
    // коробка (boxCount == 0), а не мусор.
    uint32_t oldCapacity = field->capacity;
    VoxelRigidCompoundShape *shapes =
        (VoxelRigidCompoundShape *)realloc(field->shapes, (size_t)capacity * sizeof(*shapes));
    if (shapes == NULL)
    {
        return false;
    }
    field->shapes = shapes;
    if (capacity > oldCapacity)
    {
        // Exact newly allocated tail; oldCapacity and capacity are checked above.
        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(field->shapes + oldCapacity, 0,
               (size_t)(capacity - oldCapacity) * sizeof(*shapes));
    }

    void *scratch = realloc(field->scratch, scratchBytes);
    if (scratch == NULL)
    {
        return false;
    }
    field->scratch = scratch;
    field->scratchBytes = scratchBytes;
    if (persistentCapacity > field->contactCache.bodyCapacity)
    {
        void *cacheStorage = realloc(field->contactCache.storage, cacheBytes);
        if (cacheStorage == NULL)
        {
            return false;
        }
        field->contactCache.storage = cacheStorage;
        if (!VoxelRigidContactCacheInitialize(&field->contactCache, cacheStorage,
                                              persistentCapacity, cacheBytes))
        {
            return false;
        }
    }
    if (persistentCapacity > field->broadphase.bodyCapacity)
    {
        void *indexStorage = realloc(field->broadphase.storage, indexBytes);
        if (indexStorage == NULL)
        {
            return false;
        }
        field->broadphase.storage = indexStorage;
        if (!VoxelRigidBroadphaseInitialize(&field->broadphase, indexStorage, persistentCapacity,
                                            indexBytes))
        {
            return false;
        }
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

    // Scratch grows by at most 1.5x; persistent state keeps its replay thresholds.
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
    free(field->shapes);
    field->shapes = NULL;
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
    field->spawnBody = NULL;
    field->spawnContext = NULL;
    field->spawningStopped = true;
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

bool SimulationCubeFieldBodyIsBox(const SimulationCubeField *field, uint32_t index)
{
    if (field == NULL || field->shapes == NULL || index >= field->count)
    {
        return false;
    }
    return field->shapes[index].boxCount == 0u;
}

void SimulationCubeFieldInvalidateSolver(SimulationCubeField *field)
{
    if (field == NULL)
    {
        return;
    }
    // stableId-якоря и индексы тел после сдвига больше не совпадают с
    // сохранёнными контактами; broadphase пересобирается на следующем шаге.
    VoxelRigidContactCacheReset(&field->contactCache);
    VoxelRigidBroadphaseReset(&field->broadphase);
}

bool SimulationCubeFieldPrimitiveCount(const SimulationCubeField *field, uint32_t *outCount)
{
    if (field == NULL || outCount == NULL || field->shapes == NULL)
    {
        return false;
    }
    uint64_t total = field->count;
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        uint32_t boxes = field->shapes[index].boxCount;
        if (boxes > 1u)
        {
            total += (uint64_t)(boxes - 1u);
        }
    }
    if (total > (uint64_t)UINT32_MAX)
    {
        return false;
    }
    *outCount = (uint32_t)total;
    return true;
}

bool SimulationCubeFieldReserveBodies(SimulationCubeField *field, uint32_t required)
{
    if (field == NULL || field->bodies == NULL || field->shapes == NULL ||
        required > VOXEL_RIGID_MAX_BODIES ||
        (required != 0u && VoxelRigidBodyStepScratchBytes(required) == 0u))
    {
        return false;
    }
    if (required > field->capacity)
    {
        // Транзиентная ёмкость растёт не меньше чем на 1.5x, как и у кубов, но
        // без касания scratch/cache: их готовит отдельный solver-план.
        uint32_t capacity = field->capacity < 64u ? 64u : field->capacity;
        while (capacity < required)
        {
            uint32_t increment = capacity / 2u;
            if (increment > VOXEL_RIGID_MAX_BODIES - capacity)
            {
                capacity = required;
                break;
            }
            capacity += increment;
        }
        if (capacity < required)
        {
            capacity = required;
        }
        if ((uint64_t)capacity * sizeof(VoxelRigidBody) > SIZE_MAX ||
            (uint64_t)capacity * sizeof(VoxelRigidCompoundShape) > SIZE_MAX)
            return false;
        VoxelRigidBody *bodies =
            (VoxelRigidBody *)realloc(field->bodies, (size_t)capacity * sizeof(*bodies));
        if (bodies == NULL)
        {
            return false;
        }
        field->bodies = bodies;
        VoxelRigidCompoundShape *shapes =
            (VoxelRigidCompoundShape *)realloc(field->shapes, (size_t)capacity * sizeof(*shapes));
        if (shapes == NULL)
        {
            return false;
        }
        field->shapes = shapes;
        uint32_t oldCapacity = field->capacity;
        if (capacity > oldCapacity)
        {
            // Exact newly allocated tail; no Annex K dependency in the portable core.
            // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
            memset(field->shapes + oldCapacity, 0,
                   (size_t)(capacity - oldCapacity) * sizeof(*shapes));
        }
        field->capacity = capacity;
    }
    if (required > field->broadphase.bodyCapacity)
    {
        uint32_t capacity = SimulationGrownCapacity(field->broadphase.bodyCapacity, required);
        uint32_t bytes = VoxelRigidBroadphaseBytes(capacity);
        if (bytes == 0u)
        {
            return false;
        }
        void *storage = realloc(field->broadphase.storage, bytes);
        if (storage == NULL)
        {
            return false;
        }
        field->broadphase.storage = storage;
        if (!VoxelRigidBroadphaseInitialize(&field->broadphase, storage, capacity, bytes))
        {
            return false;
        }
    }
    return true;
}

bool SimulationCubeFieldPrepareSolver(SimulationCubeField *field, uint32_t requiredBodyCount,
                                      uint32_t requiredPrimitives, SimulationCubeFieldSolverPlan *plan)
{
    if (plan == NULL)
    {
        return false;
    }
    SimulationCubeFieldSolverPlan empty = {0};
    *plan = empty;
    if (field == NULL || field->bodies == NULL || field->shapes == NULL ||
        requiredBodyCount > VOXEL_RIGID_MAX_BODIES || requiredPrimitives < requiredBodyCount)
    {
        return false;
    }

    uint32_t scratchBytes = requiredPrimitives > requiredBodyCount
                                ? VoxelRigidBodyStepCompoundScratchBytes(requiredBodyCount,
                                                                         requiredPrimitives)
                                : VoxelRigidBodyStepScratchBytes(requiredBodyCount);
    if (scratchBytes == 0u)
    {
        return false;
    }
    if (scratchBytes > field->scratchBytes)
    {
        void *scratch = malloc(scratchBytes);
        if (scratch == NULL)
        {
            return false;
        }
        plan->scratch = scratch;
        plan->scratchBytes = scratchBytes;
    }
    if (requiredPrimitives > field->contactCache.bodyCapacity)
    {
        uint32_t capacity =
            SimulationGrownCapacity(field->contactCache.bodyCapacity, requiredPrimitives);
        uint32_t bytes = VoxelRigidContactCacheBytes(capacity);
        if (bytes == 0u)
        {
            SimulationCubeFieldAbortSolver(plan);
            return false;
        }
        void *storage = malloc(bytes);
        if (storage == NULL)
        {
            SimulationCubeFieldAbortSolver(plan);
            return false;
        }
        VoxelRigidContactCache temp;
        if (!VoxelRigidContactCacheInitialize(&temp, storage, capacity, bytes))
        {
            free(storage);
            SimulationCubeFieldAbortSolver(plan);
            return false;
        }
        plan->cacheStorage = storage;
        plan->cacheCapacity = capacity;
        plan->cacheBytes = bytes;
    }
    return true;
}

void SimulationCubeFieldCommitSolver(SimulationCubeField *field, SimulationCubeFieldSolverPlan *plan)
{
    if (field == NULL || plan == NULL)
    {
        return;
    }
    if (plan->scratch != NULL)
    {
        free(field->scratch);
        field->scratch = plan->scratch;
        field->scratchBytes = plan->scratchBytes;
        plan->scratch = NULL;
        plan->scratchBytes = 0u;
    }
    if (plan->cacheStorage != NULL)
    {
        free(field->contactCache.storage);
        field->contactCache.storage = plan->cacheStorage;
        (void)VoxelRigidContactCacheInitialize(&field->contactCache, plan->cacheStorage,
                                               plan->cacheCapacity, plan->cacheBytes);
        plan->cacheStorage = NULL;
    }
}

void SimulationCubeFieldAbortSolver(SimulationCubeFieldSolverPlan *plan)
{
    if (plan == NULL)
    {
        return;
    }
    free(plan->scratch);
    plan->scratch = NULL;
    plan->scratchBytes = 0u;
    free(plan->cacheStorage);
    plan->cacheStorage = NULL;
    plan->cacheCapacity = 0u;
    plan->cacheBytes = 0u;
}

bool SimulationCubeFieldRemoveBody(SimulationCubeField *field, uint32_t index)
{
    if (field == NULL || field->bodies == NULL || field->shapes == NULL ||
        index >= field->count)
    {
        return false;
    }
    VoxelRigidBodyRelease(&field->bodies[index]);
    for (uint32_t i = index; i + 1u < field->count; ++i)
    {
        field->bodies[i] = field->bodies[i + 1u];
        field->shapes[i] = field->shapes[i + 1u];
    }
    --field->count;
    // Хвост хранит копию дескрипторов последнего тела. Обнуление отбрасывает
    // копию, не освобождая её: владелец уже переехал на count - 1.
    VoxelRigidBody emptyBody = {0};
    field->bodies[field->count] = emptyBody;
    VoxelRigidCompoundShape emptyShape = {0};
    field->shapes[field->count] = emptyShape;
    SimulationCubeFieldInvalidateSolver(field);
    return true;
}

// Держит scratch и contact cache достаточными для текущего числа примитивов.
// Для сцены из одних коробок это no-op: она уже уложена в legacy-пороги
// GrowTo, поэтому replay-пороги и хеши не меняются. Для смешанной сцены
// добирает compound-размер, если поздний спавн куба пересоздал scratch меньше
// нужного. Вызывается только после успешной вставки тела, не в самом шаге.
static bool EnsureSolverCapacity(SimulationCubeField *field)
{
    uint32_t primitives = 0u;
    if (!SimulationCubeFieldPrimitiveCount(field, &primitives))
    {
        return false;
    }
    uint32_t requiredScratch = primitives > field->count
                                   ? VoxelRigidBodyStepCompoundScratchBytes(field->count, primitives)
                                   : VoxelRigidBodyStepScratchBytes(field->count);
    if (requiredScratch == 0u)
    {
        return false;
    }
    if (requiredScratch <= field->scratchBytes && primitives <= field->contactCache.bodyCapacity)
    {
        return true;
    }
    SimulationCubeFieldSolverPlan plan;
    if (!SimulationCubeFieldPrepareSolver(field, field->count, primitives, &plan))
    {
        return false;
    }
    SimulationCubeFieldCommitSolver(field, &plan);
    return true;
}

static bool SpawnCube(SimulationCubeField *field, const double spawnPosition[3])
{
    // Предел движка — отказ: больше тел он представить не может.
    if (field->count >= VOXEL_RIGID_MAX_BODIES)
    {
        return false;
    }
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
        if (field->count >= VOXEL_RIGID_MAX_BODIES)
        {
            return false;
        }
        uint32_t increment = field->capacity / 2u;
        uint32_t capacity = increment > VOXEL_RIGID_MAX_BODIES - field->capacity
                                ? VOXEL_RIGID_MAX_BODIES
                                : field->capacity + increment;
        if (capacity <= field->count || !GrowTo(field, capacity))
        {
            return false;
        }
    }
    // Persistent structures use power-of-two capacities and may reach their
    // threshold while the transient body/scratch allocation still has room.
    // Grow them before inserting the body whose index would exceed that limit.
    if (field->count >= field->contactCache.bodyCapacity ||
        field->count >= field->broadphase.bodyCapacity)
    {
        if (!GrowTo(field, field->capacity))
        {
            return false;
        }
    }
    uint32_t slot = field->count;
    // Обычный куб — быстрый путь коробки: форма без дочерних коробок.
    VoxelRigidCompoundShape boxShape = {0};
    field->shapes[slot] = boxShape;

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
    // Новый куб мог увеличить и число примитивов: составные формы могли уже
    // упереться в бюджет. Отказ здесь — та же ошибка спавна, без retry.
    return EnsureSolverCapacity(field);
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
    if (field->spawnLimit != 0u && field->spawnCounter >= field->spawnLimit)
        field->spawningStopped = true;
    if (!field->spawningStopped)
        field->spawnPhase += SIMULATION_CUBE_SPAWNS_PER_SECOND;
    while (!field->spawningStopped && field->spawnPhase >= SIMULATION_CUBE_TICKS_PER_SECOND)
    {
        field->spawnPhase -= SIMULATION_CUBE_TICKS_PER_SECOND;
        bool spawned;
        if (field->spawnBody != NULL)
        {
            spawned = field->spawnBody(field->spawnContext, spawnPosition);
            if (spawned)
                ++field->spawnCounter;
        }
        else
            spawned = SpawnCube(field, spawnPosition);
        if (!spawned)
        {
            field->failed = true;
            return false;
        }
        if (field->spawnLimit != 0u && field->spawnCounter >= field->spawnLimit)
        {
            field->spawningStopped = true;
            field->spawnPhase = 0u;
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
        // Один общий шаг: обычные кубы (boxCount == 0) и составные постройки
        // решаются вместе, поэтому их столкновения взаимны.
        bool stepped = VoxelRigidBodyStepCompoundEx(field->bodies, field->count, &collision,
                                                    &field->settings, field->scratch,
                                                    field->scratchBytes, &options, field->shapes);
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
    // Составное тело рисуется по блокам через ConstructBlockPlacement:
    // его envelope-коробка не является формой.
    if (field->shapes != NULL && field->shapes[index].boxCount != 0u)
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
