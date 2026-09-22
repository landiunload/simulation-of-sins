#include "game/construct.h"
#include "physics/compound_shape.h"

#include "numeric/infinite_coord.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Масса одного блока. Блоки единичного объёма при однородной плотности,
// поэтому масса тела — просто число блоков.
#define CONSTRUCT_BLOCK_MASS 1.0
#define CONSTRUCT_FRICTION 0.55
#define CONSTRUCT_RESTITUTION 0.05

// Позиции и скорости тела хранятся в фиксированной точке с этим масштабом.
// COM-смещение блока ограничено сеткой, поэтому его можно сложить с bigint
// как целое число масштабированных единиц, не округляя саму позу.
#define CONSTRUCT_COORD_SCALE 4294967296.0
#define CONSTRUCT_MAX_COORD_DELTA 1.0e9

// Начальная ёмкость метаданных и коэффициент роста (~1.5x). Слоты
// переиспользуются по наименьшему свободному индексу, поэтому порядок тел
// детерминирован и не зависит от истории перевыделений.
#define CONSTRUCT_INITIAL_CAPACITY 64u

static bool FinitePosition(const double position[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!isfinite(position[axis]))
        {
            return false;
        }
    }
    return true;
}

static bool SameLocal(const int32_t left[3], const int32_t right[3])
{
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

// Поворот вектора кватернионом (x, y, z, w) наружу.
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

static void RefreshBounds(ConstructBody *body)
{
    if (body->blockCount == 0u)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            body->minimum[axis] = 0;
            body->maximum[axis] = 0;
        }
        return;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        body->minimum[axis] = body->blocks[0].local[axis];
        body->maximum[axis] = body->blocks[0].local[axis];
    }
    for (uint32_t index = 1u; index < body->blockCount; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            int32_t value = body->blocks[index].local[axis];
            if (value < body->minimum[axis])
            {
                body->minimum[axis] = value;
            }
            if (value > body->maximum[axis])
            {
                body->maximum[axis] = value;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Хеш-карта воксельных координат. Нужна, чтобы проверка дубликатов,
// связность и компоненты были O(n) по ожиданию, а не O(n^2): большая форма
// (тысячи блоков) иначе упиралась бы в квадрат на каждом спавне и правке.
// Координаты ключа уникальны, значение — индекс блока в массиве тела.
// Пустой слот помечен value == UINT32_MAX; индекс блока таким быть не может,
// потому что count <= UINT32_MAX.

#define CONSTRUCT_VOXEL_EMPTY UINT32_MAX

typedef struct ConstructVoxelEntry
{
    int32_t local[3];
    uint32_t value;
} ConstructVoxelEntry;

typedef struct ConstructVoxelMap
{
    ConstructVoxelEntry *entries;
    uint32_t capacity;
} ConstructVoxelMap;

static const int32_t CONSTRUCT_FACE_OFFSETS[6][3] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

static uint32_t ConstructHashLocal(const int32_t local[3])
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        hash ^= (uint64_t)(uint32_t)local[axis];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    return (uint32_t)hash;
}

static void ConstructVoxelMapDestroy(ConstructVoxelMap *map)
{
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0u;
}

// Ёмкость — степень двойки не меньше 2 * count (load factor <= 0.5).
static bool ConstructVoxelMapInit(ConstructVoxelMap *map, uint32_t count)
{
    map->entries = NULL;
    map->capacity = 0u;
    if (count == 0u)
    {
        return false;
    }
    uint64_t needed = (uint64_t)count * 2u;
    if (needed > (uint64_t)UINT32_MAX)
    {
        return false;
    }
    uint32_t capacity = 1u;
    while ((uint64_t)capacity < needed)
    {
        if (capacity > UINT32_MAX / 2u)
        {
            return false;
        }
        capacity <<= 1u;
    }
    if (capacity < 8u)
    {
        capacity = 8u;
    }
    if ((uint64_t)capacity > (uint64_t)SIZE_MAX / sizeof(ConstructVoxelEntry))
    {
        return false;
    }
    ConstructVoxelEntry *entries =
        (ConstructVoxelEntry *)malloc((size_t)capacity * sizeof(*entries));
    if (entries == NULL)
    {
        return false;
    }
    for (uint32_t slot = 0u; slot < capacity; ++slot)
    {
        entries[slot].value = CONSTRUCT_VOXEL_EMPTY;
    }
    map->entries = entries;
    map->capacity = capacity;
    return true;
}

static uint32_t ConstructVoxelMapFind(const ConstructVoxelMap *map, const int32_t local[3])
{
    if (map->capacity == 0u)
    {
        return CONSTRUCT_VOXEL_EMPTY;
    }
    uint32_t mask = map->capacity - 1u;
    uint32_t slot = ConstructHashLocal(local) & mask;
    for (uint32_t step = 0u; step < map->capacity; ++step)
    {
        const ConstructVoxelEntry *entry = &map->entries[slot];
        if (entry->value == CONSTRUCT_VOXEL_EMPTY)
        {
            return CONSTRUCT_VOXEL_EMPTY;
        }
        if (entry->local[0] == local[0] && entry->local[1] == local[1] &&
            entry->local[2] == local[2])
        {
            return entry->value;
        }
        slot = (slot + 1u) & mask;
    }
    return CONSTRUCT_VOXEL_EMPTY;
}

static bool ConstructVoxelMapInsert(ConstructVoxelMap *map, const int32_t local[3], uint32_t value)
{
    uint32_t mask = map->capacity - 1u;
    uint32_t slot = ConstructHashLocal(local) & mask;
    for (uint32_t step = 0u; step < map->capacity; ++step)
    {
        ConstructVoxelEntry *entry = &map->entries[slot];
        if (entry->value == CONSTRUCT_VOXEL_EMPTY)
        {
            entry->local[0] = local[0];
            entry->local[1] = local[1];
            entry->local[2] = local[2];
            entry->value = value;
            return true;
        }
        slot = (slot + 1u) & mask;
    }
    return false;
}

// Соседняя клетка без переполнения signed int32: промежуточная сумма — int64.
static bool OffsetLocal(const int32_t local[3], const int32_t offset[3], int32_t out[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        int64_t value = (int64_t)local[axis] + (int64_t)offset[axis];
        if (value < INT32_MIN || value > INT32_MAX)
        {
            return false;
        }
        out[axis] = (int32_t)value;
    }
    return true;
}

// Карта всех блоков, кроме exclude (exclude == UINT32_MAX — включая все).
static bool BuildBlockMap(const ConstructBlock *blocks, uint32_t count, uint32_t exclude,
                          ConstructVoxelMap *map)
{
    if (!ConstructVoxelMapInit(map, count))
    {
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (index == exclude)
        {
            continue;
        }
        if (!ConstructVoxelMapInsert(map, blocks[index].local, index))
        {
            ConstructVoxelMapDestroy(map);
            return false;
        }
    }
    return true;
}

// Набор блоков связен по граням. Нужен на спавне: разреженная форма сразу
// распалась бы сама собой, а её части заняли бы разные тела без команды.
static bool BlocksConnected(const ConstructBlock *blocks, uint32_t count,
                            const ConstructVoxelMap *map)
{
    if (count <= 1u)
    {
        return true;
    }
    if ((uint64_t)count > (uint64_t)SIZE_MAX / sizeof(uint32_t))
    {
        return false;
    }
    uint8_t *visited = (uint8_t *)malloc((size_t)count * sizeof(*visited));
    uint32_t *queue = (uint32_t *)malloc((size_t)count * sizeof(*queue));
    if (visited == NULL || queue == NULL)
    {
        free(visited);
        free(queue);
        return false;
    }
    // Exact allocated arrays; no Annex K dependency in the portable core.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(visited, 0, (size_t)count * sizeof(*visited));
    uint32_t head = 0u;
    uint32_t tail = 0u;
    visited[0] = 1u;
    queue[tail++] = 0u;
    while (head < tail)
    {
        uint32_t current = queue[head++];
        for (uint32_t face = 0u; face < 6u; ++face)
        {
            int32_t neighbor[3];
            if (!OffsetLocal(blocks[current].local, CONSTRUCT_FACE_OFFSETS[face], neighbor))
            {
                continue;
            }
            uint32_t found = ConstructVoxelMapFind(map, neighbor);
            if (found != CONSTRUCT_VOXEL_EMPTY && !visited[found])
            {
                visited[found] = 1u;
                queue[tail++] = found;
            }
        }
    }
    bool connected = true;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!visited[index])
        {
            connected = false;
            break;
        }
    }
    free(visited);
    free(queue);
    return connected;
}

// Связные компоненты по граням, нумеруются в порядке блоков (сид — наименьший
// ещё не помеченный индекс). exclude исключается из формы, но сохраняет
// позицию в массиве (component == UINT32_MAX). Компоненты нумеруются подряд с
// нуля; возвращается их число.
static uint32_t LabelComponents(const ConstructBody *body, uint32_t exclude, uint32_t *component,
                                uint32_t *queue, const ConstructVoxelMap *map)
{
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        component[index] = CONSTRUCT_VOXEL_EMPTY;
    }
    uint32_t label = 0u;
    for (uint32_t seed = 0u; seed < body->blockCount; ++seed)
    {
        if (seed == exclude || component[seed] != CONSTRUCT_VOXEL_EMPTY)
        {
            continue;
        }
        uint32_t head = 0u;
        uint32_t tail = 0u;
        queue[tail++] = seed;
        component[seed] = label;
        while (head < tail)
        {
            uint32_t current = queue[head++];
            for (uint32_t face = 0u; face < 6u; ++face)
            {
                int32_t neighbor[3];
                if (!OffsetLocal(body->blocks[current].local, CONSTRUCT_FACE_OFFSETS[face],
                                 neighbor))
                {
                    continue;
                }
                uint32_t found = ConstructVoxelMapFind(map, neighbor);
                if (found != CONSTRUCT_VOXEL_EMPTY && component[found] == CONSTRUCT_VOXEL_EMPTY)
                {
                    component[found] = label;
                    queue[tail++] = found;
                }
            }
        }
        ++label;
    }
    return label;
}

static uint32_t CountComponent(const ConstructBody *body, const uint32_t *component, uint32_t wanted,
                               uint32_t exclude)
{
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        if (index != exclude && component[index] == wanted)
        {
            ++count;
        }
    }
    return count;
}

static void CollectComponent(const ConstructBody *body, const uint32_t *component, uint32_t wanted,
                             uint32_t exclude, ConstructBlock *out)
{
    uint32_t write = 0u;
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        if (index == exclude || component[index] != wanted)
        {
            continue;
        }
        out[write++] = body->blocks[index];
    }
}

static ConstructBody *FindBodyById(ConstructSystem *system, uint64_t id)
{
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        ConstructBody *body = &system->bodies[index];
        if (body->active && body->id == id)
        {
            return body;
        }
    }
    return NULL;
}

static ConstructBody *FindFreeBody(ConstructSystem *system)
{
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        if (!system->bodies[index].active)
        {
            return &system->bodies[index];
        }
    }
    return NULL;
}

static uint32_t CountActiveBodies(const ConstructSystem *system)
{
    if (system == NULL || system->bodies == NULL)
    {
        return 0u;
    }
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        if (system->bodies[index].active)
        {
            ++count;
        }
    }
    return count;
}

// Ничего не освобождает: только обнуляет указатели и мету слота.
static void ClearBody(ConstructBody *body)
{
    // ConstructBody содержит только указатели и POD-мету; обнуление на месте
    // не рвёт ничего, что уже освобождено отдельно.
    // Exact object size; Annex K is not part of the portable runtime.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(body, 0, sizeof(*body));
}

static void FreeBodyArrays(ConstructBody *body)
{
    free(body->blocks);
    body->blocks = NULL;
    free(body->shapeBoxes);
    body->shapeBoxes = NULL;
    body->blockCount = 0u;
}

// Растит массив метаданных минимум под required слотов. Рост ~1.5x, новые
// слоты обнуляются. Переезд массива безопасен: blocks/shapeBoxes тел лежат
// отдельно, а поле держит указатели только на shapeBoxes.
static bool ConstructReserveBodies(ConstructSystem *system, uint32_t required)
{
    if (required <= system->capacity)
    {
        return true;
    }
    uint32_t capacity = system->capacity < CONSTRUCT_INITIAL_CAPACITY
                            ? CONSTRUCT_INITIAL_CAPACITY
                            : system->capacity;
    while (capacity < required)
    {
        uint32_t grown = capacity + capacity / 2u;
        if (grown <= capacity)
        {
            capacity = required;
            break;
        }
        capacity = grown;
    }
    if (capacity < required)
    {
        capacity = required;
    }
    if ((uint64_t)capacity > (uint64_t)SIZE_MAX / sizeof(ConstructBody))
    {
        return false;
    }
    ConstructBody *bodies = (ConstructBody *)realloc(system->bodies, (size_t)capacity * sizeof(*bodies));
    if (bodies == NULL)
    {
        return false;
    }
    if (capacity > system->capacity)
    {
        // Exact newly allocated tail; no Annex K dependency in the portable core.
        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(bodies + system->capacity, 0,
               (size_t)(capacity - system->capacity) * sizeof(*bodies));
    }
    system->bodies = bodies;
    system->capacity = capacity;
    return true;
}

// После удаления слота поля все постройки с большим bodyIndex сдвигаются.
static void AdjustBodyIndicesAfterRemoval(ConstructSystem *system, uint32_t removedIndex,
                                          const ConstructBody *skip)
{
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        ConstructBody *body = &system->bodies[index];
        if (body->active && body != skip && body->bodyIndex > removedIndex)
        {
            --body->bodyIndex;
        }
    }
}

// ----------------------------------------------------------------------------
// Bigint-перенос позиции и скорости. Публичный API копирует InfiniteCoord и
// добавляет ограниченную fixed-point дельту: магнитуда позы/скорости не
// проходит через насыщающий double, теряется только sub-2^-32 часть сдвига.

static bool AddBoundedCoordDelta(InfiniteCoord *value, double delta)
{
    if (delta == 0.0)
    {
        return true;
    }
    if (!isfinite(delta) || fabs(delta) > CONSTRUCT_MAX_COORD_DELTA)
    {
        return false;
    }
    double scaled = delta * CONSTRUCT_COORD_SCALE;
    if (!(scaled > -9.0e18 && scaled < 9.0e18))
    {
        return false;
    }
    return InfiniteCoordTryAddInt64InPlace(value, (int64_t)scaled);
}

static bool ShiftedPosition(InfiniteCoord *out, const InfiniteCoord *source, double delta)
{
    InfiniteCoord temporary;
    InfiniteCoordInit(&temporary);
    if (!InfiniteCoordTryCopyAddInt64(&temporary, source, 0))
    {
        return false;
    }
    if (!AddBoundedCoordDelta(&temporary, delta))
    {
        InfiniteCoordDestroy(&temporary);
        return false;
    }
    *out = temporary;
    return true;
}

// omega x (worldDelta в блоках), результат в том же масштабе, что и скорости.
static bool SpinDelta(const InfiniteCoord omega[3], const double worldDelta[3],
                      InfiniteCoord out[3])
{
    int64_t fixed[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double scaled = worldDelta[axis] * CONSTRUCT_COORD_SCALE;
        if (!isfinite(worldDelta[axis]) || !(scaled > -9.0e18 && scaled < 9.0e18))
        {
            return false;
        }
        fixed[axis] = (int64_t)scaled;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&out[axis]);
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        int32_t next = (axis + 1) % 3;
        int32_t previous = (axis + 2) % 3;
        InfiniteCoord first;
        InfiniteCoord second;
        InfiniteCoord negated;
        InfiniteCoord difference;
        InfiniteCoordInit(&first);
        InfiniteCoordInit(&second);
        InfiniteCoordInit(&negated);
        InfiniteCoordInit(&difference);
        bool ok = InfiniteCoordTryCopyMultiplyInt64(&first, &omega[next], fixed[previous]);
        if (ok)
        {
            ok = InfiniteCoordTryCopyMultiplyInt64(&second, &omega[previous], fixed[next]);
        }
        if (ok)
        {
            ok = InfiniteCoordTryCopyNegate(&negated, &second);
        }
        if (ok)
        {
            ok = InfiniteCoordTryAdd(&difference, &first, &negated);
        }
        if (ok)
        {
            ok = InfiniteCoordTryCopyShiftRight(&out[axis], &difference, 32u);
        }
        InfiniteCoordDestroy(&first);
        InfiniteCoordDestroy(&second);
        InfiniteCoordDestroy(&negated);
        InfiniteCoordDestroy(&difference);
        if (!ok)
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&out[cleanup]);
            }
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Подготовленная замена тела. Все выделения и bigint живут здесь, поле и мета
// ConstructBody не меняются до Commit. При отказе PlanFree освобождает всё,
// а прежнее тело остаётся нетронутым. Commit передаёт владение blocks/boxes
// телу и обнуляет их в плане, поэтому PlanFree после Commit ничего не трогает.

typedef struct ConstructRigidPlan
{
    ConstructBlock *blocks;
    VoxelRigidCompoundBox *boxes;
    uint32_t blockCount;
    uint32_t boxCount;
    VoxelRigidBody body;
    double center[3];
    double halfExtent[3];
    double inverseInertia[9];
    bool bodyReady;
} ConstructRigidPlan;

static void PlanFree(ConstructRigidPlan *plan)
{
    if (plan->bodyReady)
    {
        VoxelRigidBodyRelease(&plan->body);
        plan->bodyReady = false;
    }
    free(plan->blocks);
    plan->blocks = NULL;
    free(plan->boxes);
    plan->boxes = NULL;
    plan->blockCount = 0u;
    plan->boxCount = 0u;
}

// Забирает владение blocks и считает форму в plan->boxes (центры сначала в
// координатах сетки, затем сдвинуты в COM). blocks освобождается при отказе.
static bool PlanSetShape(ConstructRigidPlan *plan, ConstructBlock *blocks, uint32_t count,
                         double mass)
{
    plan->blocks = blocks;
    if (count == 0u || (uint64_t)count > (uint64_t)SIZE_MAX / sizeof(VoxelRigidCompoundBox))
    {
        free(plan->blocks);
        plan->blocks = NULL;
        return false;
    }
    VoxelRigidCompoundBox *boxes = (VoxelRigidCompoundBox *)malloc((size_t)count * sizeof(*boxes));
    if (boxes == NULL)
    {
        free(plan->blocks);
        plan->blocks = NULL;
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            boxes[index].center[axis] = (double)blocks[index].local[axis] + 0.5;
            boxes[index].halfExtent[axis] = 0.5;
        }
    }
    double center[3], halfExtent[3], inverseInertia[9];
    if (!VoxelRigidCompoundMassProperties(boxes, count, mass, center, halfExtent, inverseInertia))
    {
        free(plan->blocks);
        plan->blocks = NULL;
        free(boxes);
        return false;
    }
    uint32_t boxCount = 0u;
    // Cook exact half-integer grid boxes before subtracting the fractional COM.
    // Mass/inertia retain the original block summation; rendering/editing keep
    // every block, while collision work follows the much smaller partition.
    if (!VoxelRigidCompoundMergeBoxes(boxes, count, boxes, count, &boxCount) ||
        boxCount == 0u || boxCount > count)
    {
        free(plan->blocks);
        plan->blocks = NULL;
        free(boxes);
        return false;
    }
    // A merged very long rod can exceed the world's per-child sampling budget
    // after rotation. Keep unit children for those constructs: a large parent
    // has an existing per-child fallback, an oversized single child does not.
    // Extent sum <= 10 bounds every rotated AABB side by 10, leaving ample
    // room for cell rounding/query halo within the 4096-cell budget.
    bool boundedChildren = true;
    for (uint32_t index = 0u; index < boxCount; ++index)
        if (boxes[index].halfExtent[0] + boxes[index].halfExtent[1] + boxes[index].halfExtent[2] >
            5.0)
            boundedChildren = false;
    if (!boundedChildren)
    {
        boxCount = count;
        for (uint32_t index = 0u; index < count; ++index)
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                boxes[index].center[axis] = (double)blocks[index].local[axis] + 0.5;
                boxes[index].halfExtent[axis] = 0.5;
            }
    }
    for (uint32_t index = 0u; index < boxCount; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            boxes[index].center[axis] -= center[axis];
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        plan->center[axis] = center[axis];
        plan->halfExtent[axis] = halfExtent[axis];
    }
    for (uint32_t component = 0u; component < 9u; ++component)
        plan->inverseInertia[component] = inverseInertia[component];
    plan->boxes = boxes;
    plan->blockCount = count;
    plan->boxCount = boxCount;
    return true;
}

// Parameter roles are fixed by this pose/topology API; call sites name each input.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void PlanApplyCommon(VoxelRigidBody *body, const double orientation[4],
                            const double inverseInertia[9])
{
    for (int32_t component = 0; component < 4; ++component)
    {
        body->orientation[component] = orientation[component];
    }
    body->inverseInertia[0] = inverseInertia[0];
    body->inverseInertia[1] = inverseInertia[4];
    body->inverseInertia[2] = inverseInertia[8];
}

// Свежее тело ставится по double-позиции (точность ограничена самим origin).
// Parameter roles are fixed by this pose/topology API; call sites name each input.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
static bool PlanInitializePositional(ConstructRigidPlan *plan, uint64_t stableId,
                                     const double orientation[4], const double position[3])
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    VoxelRigidBodyDescription description = {0};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        description.halfExtent[axis] = plan->halfExtent[axis];
        description.position[axis] = position[axis];
    }
    description.mass = (double)plan->blockCount * CONSTRUCT_BLOCK_MASS;
    description.friction = CONSTRUCT_FRICTION;
    description.restitution = CONSTRUCT_RESTITUTION;
    if (!VoxelRigidBodyInitialize(&plan->body, stableId, &description))
    {
        VoxelRigidBodyRelease(&plan->body);
        return false;
    }
    plan->bodyReady = true;
    PlanApplyCommon(&plan->body, orientation, plan->inverseInertia);
    return true;
}

// Замена существующего тела: позиция и скорости клонируются из source, поза
// смещается на worldDelta, линейная скорость получает omega x worldDelta.
static bool PlanInitializeFromSource(ConstructRigidPlan *plan, uint64_t stableId,
                                     const double orientation[4], const VoxelRigidBody *source,
                                     const double worldDelta[3])
{
    VoxelRigidBodyDescription description = {0};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        description.halfExtent[axis] = plan->halfExtent[axis];
        description.position[axis] = 0.0;
    }
    description.mass = (double)plan->blockCount * CONSTRUCT_BLOCK_MASS;
    description.friction = CONSTRUCT_FRICTION;
    description.restitution = CONSTRUCT_RESTITUTION;
    if (!VoxelRigidBodyInitialize(&plan->body, stableId, &description))
    {
        VoxelRigidBodyRelease(&plan->body);
        return false;
    }
    plan->bodyReady = true;
    PlanApplyCommon(&plan->body, orientation, plan->inverseInertia);

    InfiniteCoord spin[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&spin[axis]);
    }
    bool ok = SpinDelta(source->angularVelocity, worldDelta, spin);
    for (int32_t axis = 0; axis < 3 && ok; ++axis)
    {
        InfiniteCoord position;
        InfiniteCoordInit(&position);
        ok = ShiftedPosition(&position, &source->position[axis], worldDelta[axis]);
        if (ok)
        {
            InfiniteCoordDestroy(&plan->body.position[axis]);
            plan->body.position[axis] = position;
        }
        else
        {
            InfiniteCoordDestroy(&position);
        }

        InfiniteCoord velocity;
        InfiniteCoordInit(&velocity);
        if (ok)
        {
            ok = InfiniteCoordTryAdd(&velocity, &source->linearVelocity[axis], &spin[axis]);
        }
        if (ok)
        {
            InfiniteCoordDestroy(&plan->body.linearVelocity[axis]);
            plan->body.linearVelocity[axis] = velocity;
        }
        else
        {
            InfiniteCoordDestroy(&velocity);
        }

        InfiniteCoord angular;
        InfiniteCoordInit(&angular);
        if (ok)
        {
            ok = InfiniteCoordTryCopyAddInt64(&angular, &source->angularVelocity[axis], 0);
        }
        if (ok)
        {
            InfiniteCoordDestroy(&plan->body.angularVelocity[axis]);
            plan->body.angularVelocity[axis] = angular;
        }
        else
        {
            InfiniteCoordDestroy(&angular);
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&spin[axis]);
    }
    return ok;
}

// Передаёт телу подготовленные blocks/boxes: старые массивы тела
// освобождаются только после того, как поле перестало на них смотреть.
static bool PlanCommitExisting(ConstructSystem *system, ConstructBody *target,
                               ConstructRigidPlan *plan)
{
    SimulationCubeField *field = system->field;
    if (field == NULL || field->bodies == NULL || field->shapes == NULL || target == NULL ||
        plan == NULL || !plan->bodyReady || target->bodyIndex >= field->count)
    {
        return false;
    }
    uint32_t slot = target->bodyIndex;
    uint32_t boxCount = plan->boxCount;
    ConstructBlock *previousBlocks = target->blocks;
    VoxelRigidCompoundBox *previousBoxes = target->shapeBoxes;

    target->blocks = plan->blocks;
    target->shapeBoxes = plan->boxes;
    target->blockCount = plan->blockCount;
    plan->blocks = NULL;
    plan->boxes = NULL;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        target->localCOM[axis] = plan->center[axis];
    }
    RefreshBounds(target);

    VoxelRigidBody previous = field->bodies[slot];
    field->bodies[slot] = plan->body;
    plan->bodyReady = false;
    VoxelRigidCompoundShape *shape = &field->shapes[slot];
    shape->boxes = target->shapeBoxes;
    shape->boxCount = boxCount;
    for (uint32_t component = 0u; component < 9u; ++component)
        shape->inverseInertia[component] = plan->inverseInertia[component];

    free(previousBlocks);
    free(previousBoxes);
    VoxelRigidBodyRelease(&previous);
    return true;
}

static bool PlanCommitNew(ConstructSystem *system, ConstructBody *fresh, ConstructRigidPlan *plan,
                          uint64_t stableId)
{
    SimulationCubeField *field = system->field;
    if (field == NULL || field->bodies == NULL || field->shapes == NULL || fresh == NULL ||
        plan == NULL || !plan->bodyReady || field->count >= field->capacity)
    {
        return false;
    }
    uint32_t slot = field->count;
    uint32_t boxCount = plan->boxCount;
    ConstructBlock *previousBlocks = fresh->blocks;
    VoxelRigidCompoundBox *previousBoxes = fresh->shapeBoxes;

    fresh->blocks = plan->blocks;
    fresh->shapeBoxes = plan->boxes;
    fresh->blockCount = plan->blockCount;
    plan->blocks = NULL;
    plan->boxes = NULL;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        fresh->localCOM[axis] = plan->center[axis];
    }
    RefreshBounds(fresh);

    field->bodies[slot] = plan->body;
    plan->bodyReady = false;
    VoxelRigidCompoundShape *shape = &field->shapes[slot];
    shape->boxes = fresh->shapeBoxes;
    shape->boxCount = boxCount;
    for (uint32_t component = 0u; component < 9u; ++component)
        shape->inverseInertia[component] = plan->inverseInertia[component];
    fresh->bodyIndex = slot;
    fresh->id = stableId;
    fresh->active = true;
    field->count = slot + 1u;

    free(previousBlocks);
    free(previousBoxes);
    return true;
}

// ----------------------------------------------------------------------------
// Публичный API.

void ConstructSystemInit(ConstructSystem *system, World *world, SimulationCubeField *field)
{
    if (system == NULL)
    {
        return;
    }
    // Система теперь мала (world/field/указатель/capacity): массив тел растёт
    // в куче. Contract: перед повторным Init вызывается Release.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(system, 0, sizeof(*system));
    system->world = world;
    system->field = field;
}

void ConstructSystemReset(ConstructSystem *system)
{
    if (system == NULL)
    {
        return;
    }
    if (system->field != NULL && system->field->bodies != NULL && system->field->shapes != NULL)
    {
        for (uint32_t index = 0u; index < system->capacity; ++index)
        {
            ConstructBody *body = &system->bodies[index];
            if (!body->active)
            {
                continue;
            }
            uint32_t slot = body->bodyIndex;
            if (slot < system->field->count)
            {
                (void)SimulationCubeFieldRemoveBody(system->field, slot);
                AdjustBodyIndicesAfterRemoval(system, slot, body);
            }
            FreeBodyArrays(body);
            ClearBody(body);
        }
    }
    else
    {
        for (uint32_t index = 0u; index < system->capacity; ++index)
        {
            FreeBodyArrays(&system->bodies[index]);
            ClearBody(&system->bodies[index]);
        }
    }
    free(system->bodies);
    system->bodies = NULL;
    system->capacity = 0u;
}

void ConstructSystemRelease(ConstructSystem *system)
{
    if (system == NULL)
    {
        return;
    }
    ConstructSystemReset(system);
    if (system->field != NULL && system->field->spawnContext == system)
    {
        system->field->spawnBody = NULL;
        system->field->spawnContext = NULL;
        system->field->spawningStopped = true;
    }
    system->world = NULL;
    system->field = NULL;
}

bool ConstructSpawn(ConstructSystem *system, const ConstructBlock *blocks, uint32_t count,
                    const double origin[3], uint64_t *outBodyId)
{
    if (system == NULL || system->field == NULL || blocks == NULL || origin == NULL ||
        count == 0u || !FinitePosition(origin))
    {
        return false;
    }
    if ((uint64_t)count > (uint64_t)SIZE_MAX / sizeof(ConstructBlock))
    {
        return false;
    }
    ConstructVoxelMap map;
    if (!ConstructVoxelMapInit(&map, count))
    {
        return false;
    }
    bool valid = true;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (blocks[index].material == BLOCK_AIR ||
            ConstructVoxelMapFind(&map, blocks[index].local) != CONSTRUCT_VOXEL_EMPTY ||
            !ConstructVoxelMapInsert(&map, blocks[index].local, index))
        {
            valid = false;
            break;
        }
    }
    if (!valid || !BlocksConnected(blocks, count, &map))
    {
        ConstructVoxelMapDestroy(&map);
        return false;
    }
    ConstructVoxelMapDestroy(&map);

    SimulationCubeField *field = system->field;
    if (field->nextStableId == 0u)
    {
        return false;
    }
    uint32_t activeCount = CountActiveBodies(system);
    if (activeCount == UINT32_MAX || !ConstructReserveBodies(system, activeCount + 1u))
    {
        return false;
    }
    ConstructBody *body = FindFreeBody(system);
    if (body == NULL)
    {
        return false;
    }

    uint32_t currentPrimitives = 0u;
    if (!SimulationCubeFieldPrimitiveCount(field, &currentPrimitives))
    {
        return false;
    }
    uint32_t requiredBodies = field->count + 1u;

    ConstructBlock *newBlocks = (ConstructBlock *)malloc((size_t)count * sizeof(ConstructBlock));
    if (newBlocks == NULL)
    {
        return false;
    }
    // newBlocks was allocated for exactly count elements.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memcpy(newBlocks, blocks, (size_t)count * sizeof(ConstructBlock));

    ConstructRigidPlan plan = {0};
    if (!PlanSetShape(&plan, newBlocks, count, (double)count * CONSTRUCT_BLOCK_MASS))
    {
        PlanFree(&plan);
        return false;
    }
    double position[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        position[axis] = origin[axis] + plan.center[axis];
    }
    const double identity[4] = {0.0, 0.0, 0.0, 1.0};
    if (!PlanInitializePositional(&plan, field->nextStableId, identity, position))
    {
        PlanFree(&plan);
        return false;
    }
    if (!SimulationCubeFieldReserveBodies(field, requiredBodies))
    {
        PlanFree(&plan);
        return false;
    }
    SimulationCubeFieldSolverPlan solver;
    uint64_t requiredPrimitives = (uint64_t)currentPrimitives + plan.boxCount;
    if (requiredPrimitives > UINT32_MAX ||
        !SimulationCubeFieldPrepareSolver(field, requiredBodies, (uint32_t)requiredPrimitives,
                                          &solver))
    {
        PlanFree(&plan);
        return false;
    }
    if (!PlanCommitNew(system, body, &plan, field->nextStableId))
    {
        SimulationCubeFieldAbortSolver(&solver);
        PlanFree(&plan);
        return false;
    }
    ++field->nextStableId;
    SimulationCubeFieldCommitSolver(field, &solver);
    // Appending preserves every existing stableId, pose and body index. Keep
    // their warm-start history and incremental broadphase; the new body joins
    // through the normal bounds/wake pass. Edits/removal still invalidate.
    PlanFree(&plan);
    if (outBodyId != NULL)
    {
        *outBodyId = body->id;
    }
    return true;
}

bool ConstructPlaceBlock(ConstructSystem *system, uint64_t bodyId, const int32_t local[3],
                         uint8_t material)
{
    if (system == NULL || system->field == NULL || local == NULL || material == BLOCK_AIR)
    {
        return false;
    }
    ConstructBody *body = FindBodyById(system, bodyId);
    if (body == NULL || body->blockCount == UINT32_MAX ||
        body->bodyIndex >= system->field->count)
    {
        return false;
    }
    uint32_t newCount = body->blockCount + 1u;
    if ((uint64_t)newCount > (uint64_t)SIZE_MAX / sizeof(ConstructBlock))
    {
        return false;
    }
    ConstructVoxelMap map;
    if (!BuildBlockMap(body->blocks, body->blockCount, CONSTRUCT_VOXEL_EMPTY, &map))
    {
        return false;
    }
    bool allowed = ConstructVoxelMapFind(&map, local) == CONSTRUCT_VOXEL_EMPTY;
    if (allowed)
    {
        bool neighbor = false;
        for (uint32_t face = 0u; face < 6u; ++face)
        {
            int32_t adjacent[3];
            if (!OffsetLocal(local, CONSTRUCT_FACE_OFFSETS[face], adjacent))
            {
                continue;
            }
            if (ConstructVoxelMapFind(&map, adjacent) != CONSTRUCT_VOXEL_EMPTY)
            {
                neighbor = true;
                break;
            }
        }
        allowed = neighbor;
    }
    ConstructVoxelMapDestroy(&map);
    if (!allowed)
    {
        return false;
    }
    SimulationCubeField *field = system->field;
    ConstructBlock *newBlocks =
        (ConstructBlock *)malloc((size_t)newCount * sizeof(ConstructBlock));
    if (newBlocks == NULL)
    {
        return false;
    }
    // newBlocks has blockCount + 1 entries; the source count was validated.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memcpy(newBlocks, body->blocks, (size_t)body->blockCount * sizeof(ConstructBlock));
    newBlocks[body->blockCount].local[0] = local[0];
    newBlocks[body->blockCount].local[1] = local[1];
    newBlocks[body->blockCount].local[2] = local[2];
    newBlocks[body->blockCount].material = material;

    ConstructRigidPlan plan = {0};
    if (!PlanSetShape(&plan, newBlocks, newCount, (double)newCount * CONSTRUCT_BLOCK_MASS))
    {
        PlanFree(&plan);
        return false;
    }
    const VoxelRigidBody *source = &field->bodies[body->bodyIndex];
    double localDelta[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        localDelta[axis] = plan.center[axis] - body->localCOM[axis];
    }
    double worldDelta[3];
    RotateByQuaternion(source->orientation, localDelta, worldDelta);
    if (!PlanInitializeFromSource(&plan, body->id, source->orientation, source, worldDelta))
    {
        PlanFree(&plan);
        return false;
    }
    uint32_t currentPrimitives = 0u;
    if (!SimulationCubeFieldPrimitiveCount(field, &currentPrimitives) ||
        currentPrimitives == UINT32_MAX)
    {
        PlanFree(&plan);
        return false;
    }
    SimulationCubeFieldSolverPlan solver;
    uint64_t requiredPrimitives =
        (uint64_t)currentPrimitives - field->shapes[body->bodyIndex].boxCount + plan.boxCount;
    if (requiredPrimitives > UINT32_MAX ||
        !SimulationCubeFieldPrepareSolver(field, field->count, (uint32_t)requiredPrimitives,
                                          &solver))
    {
        PlanFree(&plan);
        return false;
    }
    if (!PlanCommitExisting(system, body, &plan))
    {
        SimulationCubeFieldAbortSolver(&solver);
        PlanFree(&plan);
        return false;
    }
    SimulationCubeFieldCommitSolver(field, &solver);
    SimulationCubeFieldInvalidateSolver(field);
    PlanFree(&plan);
    return true;
}

// Готовит замену основного тела и осколков до публикации. При отказе очищает
// всё созданное; source и мета не меняются.
// Parameter roles are fixed by this pose/topology API; call sites name each input.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
static bool PrepareSplitPlans(ConstructSystem *system, const ConstructBody *body, uint32_t found,
                              const uint32_t *component, uint32_t extra,
                              const VoxelRigidBody *source, uint64_t firstChildId,
                              ConstructRigidPlan *mainPlan, ConstructRigidPlan *childPlans,
                              ConstructBody **childBodies)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    uint32_t mainCount = CountComponent(body, component, 0u, found);
    if (mainCount == 0u)
    {
        return false;
    }
    ConstructBlock *mainBlocks =
        (ConstructBlock *)malloc((size_t)mainCount * sizeof(ConstructBlock));
    if (mainBlocks == NULL)
    {
        return false;
    }
    CollectComponent(body, component, 0u, found, mainBlocks);
    if (!PlanSetShape(mainPlan, mainBlocks, mainCount, (double)mainCount * CONSTRUCT_BLOCK_MASS))
    {
        return false;
    }
    double mainDelta[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        mainDelta[axis] = mainPlan->center[axis] - body->localCOM[axis];
    }
    double mainWorldDelta[3];
    RotateByQuaternion(source->orientation, mainDelta, mainWorldDelta);
    if (!PlanInitializeFromSource(mainPlan, body->id, source->orientation, source, mainWorldDelta))
    {
        PlanFree(mainPlan);
        return false;
    }
    uint32_t nextFreeSlot = 0u;
    for (uint32_t index = 0u; index < extra; ++index)
    {
        // Reserve distinct slots in the plan without publishing active metadata.
        // Repeated FindFreeBody calls would return the same uncommitted slot.
        while (nextFreeSlot < system->capacity && system->bodies[nextFreeSlot].active)
            ++nextFreeSlot;
        ConstructBody *fresh =
            nextFreeSlot < system->capacity ? &system->bodies[nextFreeSlot++] : NULL;
        if (fresh == NULL)
        {
            PlanFree(mainPlan);
            for (uint32_t done = 0u; done < index; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            return false;
        }
        childBodies[index] = fresh;
        uint32_t childCount = CountComponent(body, component, index + 1u, found);
        if (childCount == 0u)
        {
            PlanFree(mainPlan);
            for (uint32_t done = 0u; done < index; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            return false;
        }
        ConstructBlock *childBlocks =
            (ConstructBlock *)malloc((size_t)childCount * sizeof(ConstructBlock));
        if (childBlocks == NULL)
        {
            PlanFree(mainPlan);
            for (uint32_t done = 0u; done < index; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            return false;
        }
        CollectComponent(body, component, index + 1u, found, childBlocks);
        if (!PlanSetShape(&childPlans[index], childBlocks, childCount,
                          (double)childCount * CONSTRUCT_BLOCK_MASS))
        {
            PlanFree(mainPlan);
            for (uint32_t done = 0u; done < index; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            return false;
        }
        double childDelta[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            childDelta[axis] = childPlans[index].center[axis] - body->localCOM[axis];
        }
        double childWorldDelta[3];
        RotateByQuaternion(source->orientation, childDelta, childWorldDelta);
        if (!PlanInitializeFromSource(&childPlans[index], firstChildId + index, source->orientation,
                                      source, childWorldDelta))
        {
            PlanFree(mainPlan);
            for (uint32_t done = 0u; done <= index; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            return false;
        }
    }
    return true;
}

bool ConstructBreakBlock(ConstructSystem *system, uint64_t bodyId, const int32_t local[3],
                         uint64_t *outBodyIds, uint32_t bodyIdCapacity, uint32_t *outBodyCount,
                         uint8_t *outRemovedMaterial)
{
    if (outBodyCount != NULL)
    {
        *outBodyCount = 0u;
    }
    if (system == NULL || system->field == NULL || local == NULL)
    {
        return false;
    }
    SimulationCubeField *field = system->field;
    ConstructBody *body = FindBodyById(system, bodyId);
    if (body == NULL || body->blockCount == 0u || body->bodyIndex >= field->count)
    {
        return false;
    }
    int32_t found = -1;
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        if (SameLocal(body->blocks[index].local, local))
        {
            found = (int32_t)index;
            break;
        }
    }
    if (found < 0)
    {
        return false;
    }
    uint32_t foundIndex = (uint32_t)found;
    uint8_t removedMaterial = body->blocks[foundIndex].material;

    uint32_t *component = NULL;
    uint32_t *queue = NULL;
    uint32_t extra = 0u;
    if (body->blockCount > 1u)
    {
        if ((uint64_t)body->blockCount > (uint64_t)SIZE_MAX / sizeof(uint32_t))
        {
            return false;
        }
        component = (uint32_t *)malloc((size_t)body->blockCount * sizeof(*component));
        queue = (uint32_t *)malloc((size_t)body->blockCount * sizeof(*queue));
        if (component == NULL || queue == NULL)
        {
            free(component);
            free(queue);
            return false;
        }
        ConstructVoxelMap map;
        if (!BuildBlockMap(body->blocks, body->blockCount, foundIndex, &map))
        {
            free(component);
            free(queue);
            return false;
        }
        uint32_t componentCount = LabelComponents(body, foundIndex, component, queue, &map);
        ConstructVoxelMapDestroy(&map);
        extra = componentCount > 1u ? componentCount - 1u : 0u;
    }
    free(queue);
    queue = NULL;

    if (extra > 0u)
    {
        // Уникальные id для всех осколков назначаются заранее: нулевой id
        // запрещён, а переполнение счётчика не должно застать правку врасплох.
        uint64_t next = field->nextStableId;
        if (next == 0u || next > UINT64_MAX - (uint64_t)(extra - 1u))
        {
            free(component);
            return false;
        }
        uint32_t activeCount = CountActiveBodies(system);
        if (activeCount > UINT32_MAX - extra ||
            !ConstructReserveBodies(system, activeCount + extra))
        {
            free(component);
            return false;
        }
        // Резерв мог перевезти метаданные: указатель на тело берём заново.
        // component[] указывает в отдельный массив blocks и остаётся валиден.
        body = FindBodyById(system, bodyId);
        if (body == NULL)
        {
            free(component);
            return false;
        }
    }

    if (body->blockCount == 1u)
    {
        uint32_t slot = body->bodyIndex;
        (void)SimulationCubeFieldRemoveBody(field, slot);
        AdjustBodyIndicesAfterRemoval(system, slot, body);
        FreeBodyArrays(body);
        ClearBody(body);
        free(component);
        if (outRemovedMaterial != NULL)
            *outRemovedMaterial = removedMaterial;
        return true;
    }

    uint32_t currentPrimitives = 0u;
    if (!SimulationCubeFieldPrimitiveCount(field, &currentPrimitives) || currentPrimitives == 0u)
    {
        free(component);
        return false;
    }
    if (extra > UINT32_MAX - field->count)
    {
        free(component);
        return false;
    }
    uint32_t requiredBodies = field->count + extra;
    if (!SimulationCubeFieldReserveBodies(field, requiredBodies))
    {
        free(component);
        return false;
    }
    // reserve мог перевезти массив тел: source берём после него.
    const VoxelRigidBody *source = &field->bodies[body->bodyIndex];

    ConstructRigidPlan mainPlan = {0};
    ConstructRigidPlan *childPlans = NULL;
    ConstructBody **childBodies = NULL;
    if (extra > 0u)
    {
        if ((uint64_t)extra > (uint64_t)SIZE_MAX / sizeof(ConstructRigidPlan) ||
            (uint64_t)extra > (uint64_t)SIZE_MAX / sizeof(ConstructBody *))
        {
            free(component);
            return false;
        }
        childPlans = (ConstructRigidPlan *)calloc((size_t)extra, sizeof(ConstructRigidPlan));
        childBodies = (ConstructBody **)malloc((size_t)extra * sizeof(ConstructBody *));
        if (childPlans == NULL || childBodies == NULL)
        {
            free(childPlans);
            free((void *)childBodies);
            free(component);
            return false;
        }
    }
    uint64_t firstChildId = field->nextStableId;
    if (!PrepareSplitPlans(system, body, foundIndex, component, extra, source, firstChildId,
                           &mainPlan, childPlans, childBodies))
    {
        PlanFree(&mainPlan);
        free(childPlans);
        free((void *)childBodies);
        free(component);
        return false;
    }
    free(component);
    component = NULL;
    SimulationCubeFieldSolverPlan solver;
    uint64_t requiredPrimitives =
        (uint64_t)currentPrimitives - field->shapes[body->bodyIndex].boxCount + mainPlan.boxCount;
    for (uint32_t part = 0u; part < extra; ++part)
        requiredPrimitives += childPlans[part].boxCount;
    if (requiredPrimitives > UINT32_MAX ||
        !SimulationCubeFieldPrepareSolver(field, requiredBodies, (uint32_t)requiredPrimitives,
                                          &solver))
    {
        PlanFree(&mainPlan);
        if (childPlans != NULL)
        {
            for (uint32_t index = 0u; index < extra; ++index)
            {
                PlanFree(&childPlans[index]);
            }
        }
        free(childPlans);
        free((void *)childBodies);
        return false;
    }

    for (uint32_t index = 0u; index < extra; ++index)
    {
        if (!PlanCommitNew(system, childBodies[index], &childPlans[index], firstChildId + index))
        {
            SimulationCubeFieldAbortSolver(&solver);
            PlanFree(&mainPlan);
            for (uint32_t done = 0u; done < extra; ++done)
            {
                PlanFree(&childPlans[done]);
            }
            free(childPlans);
            free((void *)childBodies);
            return false;
        }
    }
    if (!PlanCommitExisting(system, body, &mainPlan))
    {
        SimulationCubeFieldAbortSolver(&solver);
        for (uint32_t done = 0u; done < extra; ++done)
        {
            PlanFree(&childPlans[done]);
        }
        free(childPlans);
        free((void *)childBodies);
        PlanFree(&mainPlan);
        return false;
    }
    field->nextStableId += extra;
    SimulationCubeFieldCommitSolver(field, &solver);
    SimulationCubeFieldInvalidateSolver(field);
    for (uint32_t index = 0u; index < extra; ++index)
    {
        if (outBodyIds != NULL && index < bodyIdCapacity)
        {
            outBodyIds[index] = childBodies[index]->id;
        }
        PlanFree(&childPlans[index]);
    }
    PlanFree(&mainPlan);
    free(childPlans);
    free((void *)childBodies);
    if (outBodyCount != NULL)
    {
        *outBodyCount = extra;
    }
    if (outRemovedMaterial != NULL)
        *outRemovedMaterial = removedMaterial;
    return true;
}

// Точка входа луча в дочернюю коробку в системе тела. normalAxis — ось, по
// которой точка входа максимальна; normalSign — знак локальной грани.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool RayBoxEntry(const double origin[3], const double direction[3], const double center[3],
                        const double halfExtent[3], double maximumDistance, double *outEntry,
                        int32_t *outNormalAxis, double *outNormalSign)
{
    double entry = 0.0;
    double exit = maximumDistance;
    int32_t normalAxis = -1;
    double normalSign = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] > -1e-9 && direction[axis] < 1e-9)
        {
            double offset = origin[axis] - center[axis];
            if (offset < -halfExtent[axis] || offset > halfExtent[axis])
            {
                return false;
            }
            continue;
        }
        double inverse = 1.0 / direction[axis];
        double near = (center[axis] - halfExtent[axis] - origin[axis]) * inverse;
        double far = (center[axis] + halfExtent[axis] - origin[axis]) * inverse;
        double sign = -1.0;
        if (near > far)
        {
            double swap = near;
            near = far;
            far = swap;
            sign = 1.0;
        }
        if (near > entry)
        {
            entry = near;
            normalAxis = axis;
            normalSign = sign;
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
    if (normalAxis < 0 || entry > maximumDistance)
    {
        return false;
    }
    *outEntry = entry;
    *outNormalAxis = normalAxis;
    *outNormalSign = normalSign;
    return true;
}

static void BlockLocalCenter(const ConstructBody *body, uint32_t block, double center[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
        center[axis] = (double)body->blocks[block].local[axis] + 0.5 - body->localCOM[axis];
}

bool ConstructRaycast(const ConstructSystem *system, const double origin[3],
                      const double direction[3], double maximumDistance,
                      ConstructRaycastHit *outHit)
{
    if (system == NULL || system->field == NULL || origin == NULL || direction == NULL ||
        outHit == NULL || !(maximumDistance > 0.0) || !isfinite(maximumDistance) ||
        !FinitePosition(origin) || !FinitePosition(direction))
    {
        return false;
    }
    bool found = false;
    double nearest = maximumDistance;
    ConstructRaycastHit best = {0};
    for (uint32_t index = 0u; index < system->capacity; ++index)
    {
        const ConstructBody *body = &system->bodies[index];
        if (!body->active || body->blockCount == 0u || body->bodyIndex >= system->field->count)
        {
            continue;
        }
        const VoxelRigidBody *fieldBody = &system->field->bodies[body->bodyIndex];
        double centre[3];
        if (!VoxelRigidBodyLocalPosition(fieldBody, centre))
        {
            continue;
        }
        double offset[3];
        double localOrigin[3];
        double localDirection[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            offset[axis] = origin[axis] - centre[axis];
        }
        const double conjugate[4] = {-fieldBody->orientation[0], -fieldBody->orientation[1],
                                     -fieldBody->orientation[2], fieldBody->orientation[3]};
        RotateByQuaternion(conjugate, offset, localOrigin);
        RotateByQuaternion(conjugate, direction, localDirection);
        for (uint32_t block = 0u; block < body->blockCount; ++block)
        {
            double blockCenter[3];
            BlockLocalCenter(body, block, blockCenter);
            const double blockHalf[3] = {0.5, 0.5, 0.5};
            double entry = 0.0;
            int32_t normalAxis = -1;
            double normalSign = 0.0;
            if (!RayBoxEntry(localOrigin, localDirection, blockCenter, blockHalf, nearest, &entry,
                             &normalAxis, &normalSign))
            {
                continue;
            }
            if (found && entry >= nearest)
            {
                continue;
            }
            nearest = entry;
            best.bodyId = body->id;
            best.blockIndex = block;
            best.distance = entry;
            best.normal[0] = 0;
            best.normal[1] = 0;
            best.normal[2] = 0;
            best.normal[normalAxis] = (int8_t)normalSign;
            found = true;
        }
    }
    if (!found)
    {
        return false;
    }
    *outHit = best;
    return true;
}

// Parameter roles are fixed by this pose/topology API; call sites name each input.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool ConstructBlockPlacement(const ConstructSystem *system, uint32_t bodyIndex, uint32_t blockIndex,
                             double outOrigin[3], float outRotation[4])
{
    if (system == NULL || system->field == NULL || outOrigin == NULL || outRotation == NULL ||
        bodyIndex >= system->capacity)
    {
        return false;
    }
    const ConstructBody *body = &system->bodies[bodyIndex];
    if (!body->active || blockIndex >= body->blockCount || body->bodyIndex >= system->field->count)
    {
        return false;
    }
    const VoxelRigidBody *fieldBody = &system->field->bodies[body->bodyIndex];
    double centre[3];
    if (!VoxelRigidBodyLocalPosition(fieldBody, centre))
    {
        return false;
    }
    double rotatedCenter[3];
    double blockCenter[3];
    BlockLocalCenter(body, blockIndex, blockCenter);
    RotateByQuaternion(fieldBody->orientation, blockCenter, rotatedCenter);
    const double half[3] = {0.5, 0.5, 0.5};
    double rotatedHalf[3];
    RotateByQuaternion(fieldBody->orientation, half, rotatedHalf);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outOrigin[axis] = centre[axis] + rotatedCenter[axis] - rotatedHalf[axis];
    }
    for (int32_t component = 0; component < 4; ++component)
    {
        outRotation[component] = (float)fieldBody->orientation[component];
    }
    return true;
}

bool ConstructRebase(ConstructSystem *system, const int64_t blockShift[3])
{
    // Общий rebase тел делает SimulationCubeFieldRebase; здесь только проверка
    // аргументов, чтобы повторный сдвиг не сломал позы.
    return system != NULL && blockShift != NULL;
}

uint32_t ConstructActiveBodyCount(const ConstructSystem *system)
{
    return CountActiveBodies(system);
}
