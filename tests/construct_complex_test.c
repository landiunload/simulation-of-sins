#include "game/construct.h"
#include "game/falling_cubes.h"
#include "game/foundation_world.h"
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
#include <time.h>

// Интеграционный тест сложных случайных построек. Проверяется не отдельный
// блок, а полный контракт на масштабе 257/1024/4096 блоков: произвольная
// связная по граням форма (в том числе пористая, с полостями), точная укладка
// дочерних коробок по редактируемым блокам, исходная масса и объём, лучи
// через полость и в стенку, настоящий прогон физики с столкновением
// постройка-постройка и побитовый replay двух независимых миров.
//
// Тест не зависит от внутренних макросов ConstructSystem: ёмкость и массив
// тел теперь динамические, лимита блоков/детей у игры нет. Большие формы
// проверяются на heap; автоматических массивов на тысячи элементов нет.
//
// По умолчанию прогон короткий (64 тика). --stress запускает долгий сценарий
// отдельно от CI и печатает wall time, хеш и валидность опоры. Wall time не
// является критерием прохождения: он информационный. Никаких утверждений о
// «произвольной сцене без лагов» здесь не делается.

#define TEST_DEFAULT_TICKS 64u
#define TEST_STRESS_DEFAULT_TICKS 256u
#define TEST_STRESS_DEFAULT_BODIES 8u
#define TEST_STRESS_DEFAULT_BLOCKS 512u
#define TEST_LATTICE_SIDE 32
#define TEST_LATTICE_VOLUME (TEST_LATTICE_SIDE * TEST_LATTICE_SIDE * TEST_LATTICE_SIDE)
#define TEST_HOLE_CAP 512u
#define TEST_COLLISION_GAP 0.2

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

// ---------------------------------------------------------------------------
// Детерминированный PRNG. Один и тот же seed обязан давать одну и ту же форму
// на любой платформе, поэтому здесь нет зависимости от libc rand.

static uint64_t NextRandom(uint64_t *state)
{
    uint64_t value = (*state += UINT64_C(0x9E3779B97F4A7C15));
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

static uint32_t NextBounded(uint64_t *state, uint32_t bound)
{
    return bound == 0u ? 0u : (uint32_t)(NextRandom(state) % (uint64_t)bound);
}

// ---------------------------------------------------------------------------
// Конечная TEST-решётка для случайного роста. Рост идёт от центра, поэтому
// форма связна по граням по построению. Полости вырезаются обратимо: после
// удаления внутренней клетки проверяется глобальная связность BFS, а размер
// восстанавливается добавлением одной пограничной клетки.

typedef struct TestLattice
{
    int32_t side;
    int32_t volume;
    uint8_t *occupied;
    uint8_t *blocked;
    uint8_t *queued;
    int32_t *frontier;
    uint32_t frontierCount;
    uint32_t occupiedCount;
    int32_t *cells;
    uint32_t *cellPosition;
    uint32_t cellCount;
    int32_t *queue;
    uint32_t *visitStamp;
    uint32_t stamp;
} TestLattice;

static const int32_t kNeighborOffset[6][3] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int32_t LatticeIndex(int32_t x, int32_t y, int32_t z)
{
    return (z * TEST_LATTICE_SIDE + y) * TEST_LATTICE_SIDE + x;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void LatticeCoords(int32_t cell, int32_t *x, int32_t *y, int32_t *z)
{
    *x = cell % TEST_LATTICE_SIDE;
    *y = (cell / TEST_LATTICE_SIDE) % TEST_LATTICE_SIDE;
    *z = cell / (TEST_LATTICE_SIDE * TEST_LATTICE_SIDE);
}

static void LatticeDispose(TestLattice *lattice)
{
    free(lattice->occupied);
    free(lattice->blocked);
    free(lattice->queued);
    free(lattice->frontier);
    free(lattice->cells);
    free(lattice->cellPosition);
    free(lattice->queue);
    free(lattice->visitStamp);
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(lattice, 0, sizeof(*lattice));
}

static bool LatticeAllocate(TestLattice *lattice)
{
    lattice->side = TEST_LATTICE_SIDE;
    lattice->volume = TEST_LATTICE_VOLUME;
    lattice->occupied = (uint8_t *)calloc((size_t)lattice->volume, sizeof(uint8_t));
    lattice->blocked = (uint8_t *)calloc((size_t)lattice->volume, sizeof(uint8_t));
    lattice->queued = (uint8_t *)calloc((size_t)lattice->volume, sizeof(uint8_t));
    lattice->frontier = (int32_t *)malloc((size_t)lattice->volume * sizeof(int32_t));
    lattice->cells = (int32_t *)malloc((size_t)lattice->volume * sizeof(int32_t));
    lattice->cellPosition = (uint32_t *)calloc((size_t)lattice->volume, sizeof(uint32_t));
    lattice->queue = (int32_t *)malloc((size_t)lattice->volume * sizeof(int32_t));
    lattice->visitStamp = (uint32_t *)calloc((size_t)lattice->volume, sizeof(uint32_t));
    return lattice->occupied != NULL && lattice->blocked != NULL && lattice->queued != NULL &&
           lattice->frontier != NULL && lattice->cells != NULL && lattice->cellPosition != NULL &&
           lattice->queue != NULL && lattice->visitStamp != NULL;
}

static void LatticeOccupy(TestLattice *lattice, int32_t cell)
{
    lattice->occupied[cell] = 1u;
    ++lattice->occupiedCount;
    lattice->cellPosition[cell] = lattice->cellCount + 1u;
    lattice->cells[lattice->cellCount] = cell;
    ++lattice->cellCount;
}

static void LatticeVacate(TestLattice *lattice, int32_t cell)
{
    lattice->occupied[cell] = 0u;
    --lattice->occupiedCount;
    uint32_t position = lattice->cellPosition[cell];
    if (position == 0u)
    {
        return;
    }
    uint32_t index = position - 1u;
    int32_t last = lattice->cells[lattice->cellCount - 1u];
    lattice->cells[index] = last;
    lattice->cellPosition[last] = index + 1u;
    --lattice->cellCount;
    lattice->cellPosition[cell] = 0u;
}

static void LatticeAddNeighbors(TestLattice *lattice, int32_t cell)
{
    int32_t x, y, z;
    LatticeCoords(cell, &x, &y, &z);
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        int32_t nx = x + kNeighborOffset[index][0];
        int32_t ny = y + kNeighborOffset[index][1];
        int32_t nz = z + kNeighborOffset[index][2];
        if (nx < 0 || nx >= TEST_LATTICE_SIDE || ny < 0 || ny >= TEST_LATTICE_SIDE || nz < 0 ||
            nz >= TEST_LATTICE_SIDE)
        {
            continue;
        }
        int32_t neighbor = LatticeIndex(nx, ny, nz);
        if (lattice->occupied[neighbor] || lattice->blocked[neighbor] || lattice->queued[neighbor])
        {
            continue;
        }
        lattice->queued[neighbor] = 1u;
        lattice->frontier[lattice->frontierCount++] = neighbor;
    }
}

static bool LatticeGrowTo(TestLattice *lattice, uint32_t target, uint64_t *rng)
{
    while (lattice->occupiedCount < target && lattice->frontierCount > 0u)
    {
        uint32_t pick = NextBounded(rng, lattice->frontierCount);
        int32_t cell = lattice->frontier[pick];
        lattice->frontier[pick] = lattice->frontier[lattice->frontierCount - 1u];
        --lattice->frontierCount;
        lattice->queued[cell] = 0u;
        if (lattice->occupied[cell] || lattice->blocked[cell])
        {
            continue;
        }
        LatticeOccupy(lattice, cell);
        LatticeAddNeighbors(lattice, cell);
    }
    return lattice->occupiedCount == target;
}

static bool LatticeConnected(TestLattice *lattice)
{
    if (lattice->occupiedCount <= 1u)
    {
        return true;
    }
    if (lattice->stamp == UINT32_MAX)
    {
        for (int32_t index = 0; index < lattice->volume; ++index)
        {
            lattice->visitStamp[index] = 0u;
        }
        lattice->stamp = 0u;
    }
    ++lattice->stamp;
    int32_t start = lattice->cells[0];
    lattice->visitStamp[start] = lattice->stamp;
    uint32_t head = 0u;
    uint32_t tail = 0u;
    lattice->queue[tail++] = start;
    uint32_t seen = 1u;
    while (head < tail)
    {
        int32_t cell = lattice->queue[head++];
        int32_t x, y, z;
        LatticeCoords(cell, &x, &y, &z);
        for (uint32_t index = 0u; index < 6u; ++index)
        {
            int32_t nx = x + kNeighborOffset[index][0];
            int32_t ny = y + kNeighborOffset[index][1];
            int32_t nz = z + kNeighborOffset[index][2];
            if (nx < 0 || nx >= TEST_LATTICE_SIDE || ny < 0 || ny >= TEST_LATTICE_SIDE || nz < 0 ||
                nz >= TEST_LATTICE_SIDE)
            {
                continue;
            }
            int32_t neighbor = LatticeIndex(nx, ny, nz);
            if (!lattice->occupied[neighbor] || lattice->visitStamp[neighbor] == lattice->stamp)
            {
                continue;
            }
            lattice->visitStamp[neighbor] = lattice->stamp;
            lattice->queue[tail++] = neighbor;
            ++seen;
        }
    }
    return seen == lattice->occupiedCount;
}

static bool LatticeAllNeighborsOccupied(const TestLattice *lattice, int32_t cell)
{
    int32_t x, y, z;
    LatticeCoords(cell, &x, &y, &z);
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        int32_t nx = x + kNeighborOffset[index][0];
        int32_t ny = y + kNeighborOffset[index][1];
        int32_t nz = z + kNeighborOffset[index][2];
        if (nx < 0 || nx >= TEST_LATTICE_SIDE || ny < 0 || ny >= TEST_LATTICE_SIDE || nz < 0 ||
            nz >= TEST_LATTICE_SIDE)
        {
            return false;
        }
        if (!lattice->occupied[LatticeIndex(nx, ny, nz)])
        {
            return false;
        }
    }
    return true;
}

static bool LatticeAddOneOpen(TestLattice *lattice, uint64_t *rng)
{
    uint32_t tries = lattice->frontierCount;
    while (tries > 0u && lattice->frontierCount > 0u)
    {
        --tries;
        uint32_t pick = NextBounded(rng, lattice->frontierCount);
        int32_t cell = lattice->frontier[pick];
        lattice->frontier[pick] = lattice->frontier[lattice->frontierCount - 1u];
        --lattice->frontierCount;
        lattice->queued[cell] = 0u;
        if (lattice->occupied[cell] || lattice->blocked[cell])
        {
            continue;
        }
        LatticeOccupy(lattice, cell);
        LatticeAddNeighbors(lattice, cell);
        return true;
    }
    return false;
}

// Вырезает до wanted внутренних клеток, каждая из которых становится закрытой
// полостью: все шесть соседей заняты, а удаление проверено на связность.
// Размер набора при этом не меняется — вместо вырезанной клетки растёт одна
// пограничная. Если вырастить не удалось, вырез откатывается.
static void LatticePerforate(TestLattice *lattice, uint32_t wanted, uint64_t *rng)
{
    uint32_t holes = 0u;
    uint32_t attempts = wanted * 8u + 64u;
    for (uint32_t attempt = 0u; attempt < attempts && holes < wanted; ++attempt)
    {
        if (lattice->cellCount == 0u)
        {
            return;
        }
        int32_t cell = lattice->cells[NextBounded(rng, lattice->cellCount)];
        if (!LatticeAllNeighborsOccupied(lattice, cell))
        {
            continue;
        }
        LatticeVacate(lattice, cell);
        if (!LatticeConnected(lattice))
        {
            LatticeOccupy(lattice, cell);
            continue;
        }
        lattice->blocked[cell] = 1u;
        if (!LatticeAddOneOpen(lattice, rng))
        {
            lattice->blocked[cell] = 0u;
            LatticeOccupy(lattice, cell);
            continue;
        }
        ++holes;
    }
}

static ConstructBlock *LatticeExtract(const TestLattice *lattice, uint32_t *outCount)
{
    uint32_t count = lattice->occupiedCount;
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)count * sizeof(*blocks));
    if (blocks == NULL)
    {
        *outCount = 0u;
        return NULL;
    }
    uint32_t write = 0u;
    for (int32_t cell = 0; cell < lattice->volume; ++cell)
    {
        if (!lattice->occupied[cell])
        {
            continue;
        }
        int32_t x, y, z;
        LatticeCoords(cell, &x, &y, &z);
        blocks[write].local[0] = x;
        blocks[write].local[1] = y;
        blocks[write].local[2] = z;
        blocks[write].material = (uint8_t)SIMULATION_MATERIAL_ACCENT;
        ++write;
    }
    *outCount = write;
    return blocks;
}

// Произвольная случайная связная фигура ровно из blockCount блоков.
static ConstructBlock *GenerateRandomBody(uint32_t blockCount, uint64_t seed, uint32_t *outCount)
{
    *outCount = 0u;
    if (blockCount == 0u || blockCount > (uint32_t)TEST_LATTICE_VOLUME)
    {
        return NULL;
    }
    TestLattice lattice;
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(&lattice, 0, sizeof(lattice));
    if (!LatticeAllocate(&lattice))
    {
        LatticeDispose(&lattice);
        return NULL;
    }
    uint64_t rng = seed;
    int32_t start =
        LatticeIndex(TEST_LATTICE_SIDE / 2, TEST_LATTICE_SIDE / 2, TEST_LATTICE_SIDE / 2);
    LatticeOccupy(&lattice, start);
    LatticeAddNeighbors(&lattice, start);
    bool grown = LatticeGrowTo(&lattice, blockCount, &rng);
    if (grown)
    {
        uint32_t holes = blockCount / 16u;
        if (holes > TEST_HOLE_CAP)
        {
            holes = TEST_HOLE_CAP;
        }
        LatticePerforate(&lattice, holes, &rng);
    }
    if (grown && lattice.occupiedCount != blockCount)
    {
        grown = false;
    }
    ConstructBlock *blocks = NULL;
    uint32_t count = 0u;
    if (grown)
    {
        blocks = LatticeExtract(&lattice, &count);
    }
    LatticeDispose(&lattice);
    if (blocks != NULL && count != blockCount)
    {
        free(blocks);
        blocks = NULL;
        count = 0u;
    }
    *outCount = count;
    return blocks;
}

// Комб: длинное основание и зубья через клетку. Форма связна по граням, но не
// склеивается в одну коробку — при готовке получается много дочерних коробок.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static ConstructBlock *GenerateCombBody(uint32_t blockCount, uint32_t teeth, uint32_t toothHeight,
                                        uint32_t *outCount)
{
    *outCount = 0u;
    uint32_t toothCells = teeth * toothHeight;
    if (teeth == 0u || toothCells >= blockCount)
    {
        return NULL;
    }
    uint32_t base = blockCount - toothCells;
    if (base < 2u * teeth - 1u)
    {
        return NULL;
    }
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)blockCount * sizeof(*blocks));
    if (blocks == NULL)
    {
        return NULL;
    }
    uint32_t write = 0u;
    for (uint32_t x = 0u; x < base; ++x)
    {
        blocks[write].local[0] = (int32_t)x;
        blocks[write].local[1] = 0;
        blocks[write].local[2] = 0;
        blocks[write].material = (uint8_t)SIMULATION_MATERIAL_ACCENT;
        ++write;
    }
    for (uint32_t tooth = 0u; tooth < teeth; ++tooth)
    {
        for (uint32_t y = 1u; y <= toothHeight; ++y)
        {
            blocks[write].local[0] = (int32_t)(2u * tooth);
            blocks[write].local[1] = (int32_t)y;
            blocks[write].local[2] = 0;
            blocks[write].material = (uint8_t)SIMULATION_MATERIAL_MARKER;
            ++write;
        }
    }
    *outCount = write;
    return blocks;
}

// Кольцо в плоскости XZ со сквозным отверстием по Y. Нужно для луча, который
// обязан промахнуться сквозь полость.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static ConstructBlock *GenerateHollowRing(uint32_t outer, uint32_t inner, uint32_t *outCount)
{
    *outCount = 0u;
    if (inner == 0u || inner + 2u > outer || (outer - inner) % 2u != 0u)
    {
        return NULL;
    }
    uint32_t count = outer * outer - inner * inner;
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)count * sizeof(*blocks));
    if (blocks == NULL)
    {
        return NULL;
    }
    uint32_t start = (outer - inner) / 2u;
    uint32_t write = 0u;
    for (uint32_t z = 0u; z < outer; ++z)
    {
        for (uint32_t x = 0u; x < outer; ++x)
        {
            bool inside = x >= start && x < start + inner && z >= start && z < start + inner;
            if (inside)
            {
                continue;
            }
            blocks[write].local[0] = (int32_t)x;
            blocks[write].local[1] = 0;
            blocks[write].local[2] = (int32_t)z;
            blocks[write].material = (uint8_t)SIMULATION_MATERIAL_ACCENT;
            ++write;
        }
    }
    *outCount = write;
    return blocks;
}

// Полый куб со стенкой в один блок: у формы есть закрытая полость, внутрь
// которой луч обязан попасть изнутри и не пройти насквозь снаружи.
static ConstructBlock *GenerateHollowBox(uint32_t outer, uint32_t *outCount)
{
    *outCount = 0u;
    if (outer < 3u)
    {
        return NULL;
    }
    uint32_t inner = outer - 2u;
    uint32_t count = outer * outer * outer - inner * inner * inner;
    ConstructBlock *blocks = (ConstructBlock *)malloc((size_t)count * sizeof(*blocks));
    if (blocks == NULL)
    {
        return NULL;
    }
    uint32_t write = 0u;
    for (uint32_t z = 0u; z < outer; ++z)
    {
        for (uint32_t y = 0u; y < outer; ++y)
        {
            for (uint32_t x = 0u; x < outer; ++x)
            {
                bool shell = x == 0u || x == outer - 1u || y == 0u || y == outer - 1u || z == 0u ||
                             z == outer - 1u;
                if (!shell)
                {
                    continue;
                }
                blocks[write].local[0] = (int32_t)x;
                blocks[write].local[1] = (int32_t)y;
                blocks[write].local[2] = (int32_t)z;
                blocks[write].material = (uint8_t)SIMULATION_MATERIAL_ACCENT;
                ++write;
            }
        }
    }
    *outCount = write;
    return blocks;
}

// ---------------------------------------------------------------------------
// Проверки геометрии.

static bool AdjacentLocal(const int32_t left[3], const int32_t right[3])
{
    int64_t difference[3] = {(int64_t)left[0] - (int64_t)right[0],
                             (int64_t)left[1] - (int64_t)right[1],
                             (int64_t)left[2] - (int64_t)right[2]};
    int64_t magnitude = 0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (difference[axis] > 1 || difference[axis] < -1)
        {
            return false;
        }
        magnitude += difference[axis] < 0 ? -difference[axis] : difference[axis];
    }
    return magnitude == 1;
}

static bool BlocksFaceConnected(const ConstructBlock *blocks, uint32_t count)
{
    if (count <= 1u)
    {
        return true;
    }
    uint8_t *visited = (uint8_t *)calloc((size_t)count, sizeof(uint8_t));
    int32_t *queue = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    if (visited == NULL || queue == NULL)
    {
        free(visited);
        free(queue);
        return false;
    }
    uint32_t head = 0u;
    uint32_t tail = 0u;
    visited[0] = 1u;
    queue[tail++] = 0;
    uint32_t seen = 1u;
    while (head < tail)
    {
        int32_t current = queue[head++];
        for (uint32_t candidate = 0u; candidate < count; ++candidate)
        {
            if (visited[candidate])
            {
                continue;
            }
            if (AdjacentLocal(blocks[current].local, blocks[candidate].local))
            {
                visited[candidate] = 1u;
                queue[tail++] = (int32_t)candidate;
                ++seen;
            }
        }
    }
    free(visited);
    free(queue);
    return seen == count;
}

static int64_t PackLocal(const int32_t local[3])
{
    const int64_t bias = 65536;
    return ((int64_t)((int64_t)local[0] + bias) << 42) |
           ((int64_t)((int64_t)local[1] + bias) << 21) | ((int64_t)local[2] + bias);
}

static int CompareInt64(const void *left, const void *right)
{
    int64_t a = *(const int64_t *)left;
    int64_t b = *(const int64_t *)right;
    return (a > b) - (a < b);
}

// Клетки тела обязаны быть уникальными и совпадать с входом по множеству.
static void ExpectBlockSet(const ConstructBody *body, const ConstructBlock *blocks, uint32_t count)
{
    int64_t *inputKeys = (int64_t *)malloc((size_t)count * sizeof(*inputKeys));
    int64_t *bodyKeys = (int64_t *)malloc((size_t)count * sizeof(*bodyKeys));
    EXPECT(inputKeys != NULL && bodyKeys != NULL);
    if (inputKeys == NULL || bodyKeys == NULL)
    {
        free(inputKeys);
        free(bodyKeys);
        return;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        inputKeys[index] = PackLocal(blocks[index].local);
        bodyKeys[index] = PackLocal(body->blocks[index].local);
        EXPECT(body->blocks[index].material != (uint8_t)BLOCK_AIR);
    }
    qsort(inputKeys, (size_t)count, sizeof(*inputKeys), CompareInt64);
    qsort(bodyKeys, (size_t)count, sizeof(*bodyKeys), CompareInt64);
    for (uint32_t index = 0u; index < count; ++index)
    {
        EXPECT(inputKeys[index] == bodyKeys[index]);
        if (index > 0u)
        {
            EXPECT(bodyKeys[index] != bodyKeys[index - 1u]);
        }
    }
    free(inputKeys);
    free(bodyKeys);
}

static void ExpectBoundsAndMass(const ConstructBody *body, const VoxelRigidBody *rigid)
{
    int32_t minimum[3] = {body->blocks[0].local[0], body->blocks[0].local[1],
                          body->blocks[0].local[2]};
    int32_t maximum[3] = {body->blocks[0].local[0], body->blocks[0].local[1],
                          body->blocks[0].local[2]};
    double sum[3] = {0.0, 0.0, 0.0};
    for (uint32_t index = 0u; index < body->blockCount; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            int32_t value = body->blocks[index].local[axis];
            if (value < minimum[axis])
            {
                minimum[axis] = value;
            }
            if (value > maximum[axis])
            {
                maximum[axis] = value;
            }
            sum[axis] += (double)value + 0.5;
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        EXPECT(body->minimum[axis] == minimum[axis]);
        EXPECT(body->maximum[axis] == maximum[axis]);
        double mean = sum[axis] / (double)body->blockCount;
        EXPECT(fabs(body->localCOM[axis] - mean) < 1e-9);
        double low = (double)minimum[axis];
        double high = (double)maximum[axis] + 1.0;
        double lower = body->localCOM[axis] - low;
        double upper = high - body->localCOM[axis];
        double expected = lower > upper ? lower : upper;
        EXPECT(rigid->halfExtent[axis] + 1e-9 >= expected);
        EXPECT(rigid->halfExtent[axis] <= expected + 1e-6);
    }
    EXPECT(fabs(1.0 / rigid->inverseMass - (double)body->blockCount) < 1e-6);
    EXPECT(rigid->active);
    EXPECT(rigid->stableId == body->id);
}

// Точная укладка: готовые дочерние коробки не перекрываются, выровнены по
// целой сетке (после возврата COM) и ровно покрывают редактируемые блоки.
// Дополнительно суммарный объём равен числу блоков — это и есть «исходный
// объём», а аддитивная масса проверяется отдельно в ExpectBoundsAndMass.
static void ExpectChildPartition(const ConstructBody *body, const VoxelRigidCompoundShape *shape)
{
    EXPECT(shape->boxCount >= 1u);
    EXPECT(shape->boxes != NULL);
    if (shape->boxes == NULL)
    {
        return;
    }
    double volume = 0.0;
    for (uint32_t box = 0u; box < shape->boxCount; ++box)
    {
        const VoxelRigidCompoundBox *child = &shape->boxes[box];
        double product = 1.0;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT(child->halfExtent[axis] > 0.0);
            double width = child->halfExtent[axis] * 2.0;
            product *= width;
            double center = child->center[axis] + body->localCOM[axis];
            double low = center - child->halfExtent[axis];
            double high = center + child->halfExtent[axis];
            EXPECT(fabs(low - round(low)) < 1e-7);
            EXPECT(fabs(high - round(high)) < 1e-7);
            double bodyLow = (double)body->minimum[axis] - 1e-7;
            double bodyHigh = (double)body->maximum[axis] + 1.0 + 1e-7;
            EXPECT(low >= bodyLow && high <= bodyHigh);
        }
        EXPECT(isfinite(product) && product > 0.0);
        volume += product;
    }
    EXPECT(fabs(volume - (double)body->blockCount) < 1e-4 * (double)body->blockCount + 1e-4);
    for (uint32_t block = 0u; block < body->blockCount; ++block)
    {
        double center[3];
        uint32_t covering = 0u;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            center[axis] = (double)body->blocks[block].local[axis] + 0.5;
        }
        for (uint32_t box = 0u; box < shape->boxCount; ++box)
        {
            const VoxelRigidCompoundBox *child = &shape->boxes[box];
            bool inside = true;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                double low = child->center[axis] + body->localCOM[axis] - child->halfExtent[axis];
                double high = child->center[axis] + body->localCOM[axis] + child->halfExtent[axis];
                if (center[axis] < low - 1e-6 || center[axis] > high + 1e-6)
                {
                    inside = false;
                    break;
                }
            }
            if (inside)
            {
                ++covering;
            }
        }
        EXPECT(covering == 1u);
    }
}

// ---------------------------------------------------------------------------
// Мир и система под тест.

typedef struct TestRig
{
    SimulationGroundProvider ground;
    World *world;
    SimulationCubeField field;
    ConstructSystem *system;
} TestRig;

static World *CreateGroundWorld(SimulationGroundProvider *ground)
{
    SimulationGroundProviderInit(ground);
    WorldBaseProvider provider;
    SimulationGroundProviderBind(ground, &provider);
    return WorldCreate(&provider);
}

static bool TestRigInit(TestRig *rig)
{
    rig->world = CreateGroundWorld(&rig->ground);
    rig->system = (ConstructSystem *)calloc(1u, sizeof(*rig->system));
    if (rig->world == NULL || rig->system == NULL)
    {
        return false;
    }
    if (!SimulationCubeFieldInit(&rig->field))
    {
        return false;
    }
    ConstructSystemInit(rig->system, rig->world, &rig->field);
    rig->field.spawningStopped = true;
    return true;
}

static void TestRigDestroy(TestRig *rig)
{
    if (rig->system != NULL)
    {
        ConstructSystemRelease(rig->system);
        free(rig->system);
        rig->system = NULL;
    }
    SimulationCubeFieldRelease(&rig->field);
    if (rig->world != NULL)
    {
        WorldDestroy(rig->world);
    }
    rig->world = NULL;
}

static ConstructBody *FindBodyById(const ConstructSystem *system, uint64_t id)
{
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        if (system->bodies[slot].active && system->bodies[slot].id == id)
        {
            return &system->bodies[slot];
        }
    }
    return NULL;
}

static uint32_t FindSlotById(const ConstructSystem *system, uint64_t id)
{
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        if (system->bodies[slot].active && system->bodies[slot].id == id)
        {
            return slot;
        }
    }
    return system->capacity;
}

static ConstructBody *FindFirstActive(ConstructSystem *system, uint32_t *outSlot)
{
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        if (system->bodies[slot].active)
        {
            if (outSlot != NULL)
            {
                *outSlot = slot;
            }
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void BodyWorldZRange(const ConstructSystem *system, uint32_t slot, double *outMin,
                            double *outMax)
{
    double minimum = INFINITY;
    double maximum = -INFINITY;
    const ConstructBody *body = &system->bodies[slot];
    for (uint32_t block = 0u; block < body->blockCount; ++block)
    {
        double origin[3];
        float rotation[4];
        if (!ConstructBlockPlacement(system, slot, block, origin, rotation))
        {
            *outMin = -INFINITY;
            *outMax = INFINITY;
            return;
        }
        const double quaternion[4] = {(double)rotation[0], (double)rotation[1], (double)rotation[2],
                                      (double)rotation[3]};
        for (int32_t sx = 0; sx < 2; ++sx)
        {
            for (int32_t sy = 0; sy < 2; ++sy)
            {
                for (int32_t sz = 0; sz < 2; ++sz)
                {
                    const double corner[3] = {(double)sx, (double)sy, (double)sz};
                    double rotated[3];
                    RotateQuaternion(quaternion, corner, rotated);
                    double z = origin[2] + rotated[2];
                    if (z < minimum)
                    {
                        minimum = z;
                    }
                    if (z > maximum)
                    {
                        maximum = z;
                    }
                }
            }
        }
    }
    *outMin = minimum;
    *outMax = maximum;
}

static double SystemMinVertexZ(const ConstructSystem *system)
{
    double minimum = INFINITY;
    for (uint32_t slot = 0u; slot < system->capacity; ++slot)
    {
        if (!system->bodies[slot].active)
        {
            continue;
        }
        double bodyMin, bodyMax;
        BodyWorldZRange(system, slot, &bodyMin, &bodyMax);
        if (bodyMin < minimum)
        {
            minimum = bodyMin;
        }
    }
    return minimum;
}

static void SpawnAndValidate(ConstructSystem *system, const ConstructBlock *blocks, uint32_t count,
                             const double origin[3], bool expectManyChildren)
{
    EXPECT(BlocksFaceConnected(blocks, count));
    uint64_t id = 0u;
    EXPECT(ConstructSpawn(system, blocks, count, origin, &id));
    EXPECT(id != 0u);
    ConstructBody *body = FindBodyById(system, id);
    EXPECT(body != NULL);
    if (body == NULL)
    {
        return;
    }
    EXPECT(body->active);
    EXPECT(body->blockCount == count);
    uint32_t slot = body->bodyIndex;
    EXPECT(slot < system->field->count);
    if (slot >= system->field->count)
    {
        return;
    }
    const VoxelRigidBody *rigid = &system->field->bodies[slot];
    const VoxelRigidCompoundShape *shape = &system->field->shapes[slot];
    EXPECT(shape->boxes == body->shapeBoxes);
    if (expectManyChildren)
    {
        EXPECT(shape->boxCount > 256u);
    }
    ExpectBlockSet(body, blocks, count);
    ExpectBoundsAndMass(body, rigid);
    ExpectChildPartition(body, shape);
    for (int32_t component = 0; component < 9; ++component)
    {
        EXPECT(isfinite(shape->inverseInertia[component]));
    }
    EXPECT(shape->inverseInertia[0] > 0.0 && shape->inverseInertia[4] > 0.0 &&
           shape->inverseInertia[8] > 0.0);
    // Привязка каждого блока обязана возвращать конечную позу.
    for (uint32_t block = 0u; block < count; block += (count / 32u) + 1u)
    {
        double blockOrigin[3];
        float blockRotation[4];
        EXPECT(ConstructBlockPlacement(system, FindSlotById(system, id), block, blockOrigin,
                                       blockRotation));
        EXPECT(isfinite(blockOrigin[0]) && isfinite(blockOrigin[1]) && isfinite(blockOrigin[2]));
    }
}

static void TestGeneratedGeometry(void)
{
    TestRig rig;
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(&rig, 0, sizeof(rig));
    if (!TestRigInit(&rig))
    {
        EXPECT(false);
        TestRigDestroy(&rig);
        return;
    }
    uint32_t count = 0u;
    ConstructBlock *blocks = NULL;
    uint64_t editId = 0u;

    static const uint32_t kSizes[3] = {257u, 1024u, 4096u};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        blocks = GenerateRandomBody(kSizes[index], UINT64_C(0x5EED0000) + kSizes[index], &count);
        EXPECT(blocks != NULL && count == kSizes[index]);
        if (blocks != NULL)
        {
            SpawnAndValidate(rig.system, blocks, count, (const double[3]){0.0, 0.0, 40.0}, false);
            if (index == 0u)
            {
                ConstructBody *body = FindFirstActive(rig.system, NULL);
                // Идентификатор нужен для правки ниже; тело могли перевезти.
                if (body != NULL)
                {
                    editId = body->id;
                }
            }
            free(blocks);
        }
    }

    // Комбы: не склеиваются в одну коробку и дают >256 детей.
    blocks = GenerateCombBody(1024u, 300u, 1u, &count);
    EXPECT(blocks != NULL && count == 1024u);
    if (blocks != NULL)
    {
        SpawnAndValidate(rig.system, blocks, count, (const double[3]){40.0, 0.0, 40.0}, true);
        free(blocks);
    }
    blocks = GenerateCombBody(4096u, 300u, 11u, &count);
    EXPECT(blocks != NULL && count == 4096u);
    if (blocks != NULL)
    {
        SpawnAndValidate(rig.system, blocks, count, (const double[3]){80.0, 0.0, 40.0}, true);
        free(blocks);
    }

    // Правка на масштабе: поставить и сломать один блок, форма возвращается.
    if (editId != 0u)
    {
        ConstructBody *body = FindBodyById(rig.system, editId);
        EXPECT(body != NULL);
        if (body != NULL)
        {
            const uint32_t before = body->blockCount;
            int32_t target[3] = {0, 0, 0};
            bool found = false;
            for (uint32_t block = 0u; block < body->blockCount && !found; ++block)
            {
                for (uint32_t face = 0u; face < 6u && !found; ++face)
                {
                    int32_t candidate[3] = {body->blocks[block].local[0] + kNeighborOffset[face][0],
                                            body->blocks[block].local[1] + kNeighborOffset[face][1],
                                            body->blocks[block].local[2] +
                                                kNeighborOffset[face][2]};
                    bool occupied = false;
                    for (uint32_t other = 0u; other < body->blockCount; ++other)
                    {
                        if (body->blocks[other].local[0] == candidate[0] &&
                            body->blocks[other].local[1] == candidate[1] &&
                            body->blocks[other].local[2] == candidate[2])
                        {
                            occupied = true;
                            break;
                        }
                    }
                    if (!occupied)
                    {
                        target[0] = candidate[0];
                        target[1] = candidate[1];
                        target[2] = candidate[2];
                        found = true;
                    }
                }
            }
            EXPECT(found);
            if (found)
            {
                EXPECT(ConstructPlaceBlock(rig.system, editId, target,
                                           (uint8_t)SIMULATION_MATERIAL_MARKER));
                body = FindBodyById(rig.system, editId);
                EXPECT(body != NULL && body->blockCount == before + 1u);
                uint32_t splitCount = 0u;
                EXPECT(
                    ConstructBreakBlock(rig.system, editId, target, NULL, 0u, &splitCount, NULL));
                body = FindBodyById(rig.system, editId);
                EXPECT(body != NULL && body->blockCount == before);
                EXPECT(splitCount == 0u);
            }
        }
    }

    TestRigDestroy(&rig);
}

static void TestCavityRaycasts(void)
{
    TestRig rig;
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(&rig, 0, sizeof(rig));
    if (!TestRigInit(&rig))
    {
        EXPECT(false);
        TestRigDestroy(&rig);
        return;
    }
    uint32_t ringCount = 0u;
    uint32_t boxCount = 0u;
    ConstructBlock *ring = GenerateHollowRing(9u, 5u, &ringCount);
    ConstructBlock *box = GenerateHollowBox(7u, &boxCount);
    EXPECT(ring != NULL && ringCount == 56u);
    EXPECT(box != NULL && boxCount == 218u);
    const double ringOrigin[3] = {0.0, 0.0, 2.0};
    const double boxOrigin[3] = {10.0, 0.0, 2.0};
    uint64_t ringId = 0u;
    uint64_t boxId = 0u;
    EXPECT(ConstructSpawn(rig.system, ring, ringCount, ringOrigin, &ringId));
    EXPECT(ConstructSpawn(rig.system, box, boxCount, boxOrigin, &boxId));
    ConstructRaycastHit hit = {0};
    const double down[3] = {0.0, -1.0, 0.0};
    const double right[3] = {1.0, 0.0, 0.0};
    const double left[3] = {-1.0, 0.0, 0.0};

    // Сквозь отверстие кольца луч не должен ничего найти.
    const double holeOrigin[3] = {ringOrigin[0] + 4.5, ringOrigin[1] + 5.0, ringOrigin[2] + 4.5};
    EXPECT(!ConstructRaycast(rig.system, holeOrigin, down, 20.0, &hit));

    // В стенку кольца — попадание ровно на верхнюю грань.
    const double wallOrigin[3] = {ringOrigin[0] + 0.5, ringOrigin[1] + 5.0, ringOrigin[2] + 0.5};
    EXPECT(ConstructRaycast(rig.system, wallOrigin, down, 20.0, &hit));
    EXPECT(hit.bodyId == ringId);
    EXPECT(fabs(hit.distance - 4.0) < 1e-9);
    EXPECT(hit.normal[1] == 1);

    // Внутри закрытой полости куба луч упирается во внутреннюю стенку.
    const double inner[3] = {boxOrigin[0] + 3.5, boxOrigin[1] + 3.5, boxOrigin[2] + 3.5};
    EXPECT(ConstructRaycast(rig.system, inner, right, 20.0, &hit));
    EXPECT(hit.bodyId == boxId);
    EXPECT(fabs(hit.distance - 2.5) < 1e-9);
    EXPECT(hit.normal[0] == -1);
    EXPECT(ConstructRaycast(rig.system, inner, left, 20.0, &hit));
    EXPECT(hit.bodyId == boxId);
    EXPECT(fabs(hit.distance - 2.5) < 1e-9);
    EXPECT(hit.normal[0] == 1);

    // Снаружи полость не пробивается: ближайшая стенка останавливает луч.
    const double outside[3] = {boxOrigin[0] - 5.0, boxOrigin[1] + 3.5, boxOrigin[2] + 3.5};
    EXPECT(ConstructRaycast(rig.system, outside, right, 20.0, &hit));
    EXPECT(hit.bodyId == boxId);
    EXPECT(fabs(hit.distance - 5.0) < 1e-9);

    // Луч в пустоте не находит постройки.
    EXPECT(!ConstructRaycast(rig.system, (const double[3]){100.0, 100.0, 100.0},
                             (const double[3]){0.0, 0.0, -1.0}, 1.0, &hit));

    free(ring);
    free(box);
    TestRigDestroy(&rig);
}

// ---------------------------------------------------------------------------
// Побитовый replay. Сравниваются только числовые поля и полные лимбы bigint;
// указатели, memory padding и адреса в сравнение не входят.

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool DoubleSame(double left, double right)
{
    union
    {
        double value;
        uint64_t bits;
    } a, b;
    a.value = left;
    b.value = right;
    return a.bits == b.bits;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool CoordSame(const InfiniteCoord *left, const InfiniteCoord *right)
{
    int32_t leftSign = InfiniteCoordSign(left);
    int32_t rightSign = InfiniteCoordSign(right);
    if (leftSign != rightSign)
    {
        return false;
    }
    if (leftSign == 0)
    {
        return true;
    }
    if (left->limbCount != right->limbCount)
    {
        return false;
    }
    for (uint32_t index = 0u; index < left->limbCount; ++index)
    {
        if (left->limbs[index] != right->limbs[index])
        {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ExpectShapesEqual(const VoxelRigidCompoundShape *left,
                              const VoxelRigidCompoundShape *right)
{
    EXPECT(left->boxCount == right->boxCount);
    if (left->boxCount != right->boxCount || left->boxes == NULL || right->boxes == NULL)
    {
        return;
    }
    for (int32_t component = 0; component < 9; ++component)
    {
        EXPECT(DoubleSame(left->inverseInertia[component], right->inverseInertia[component]));
    }
    for (uint32_t box = 0u; box < left->boxCount; ++box)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT(DoubleSame(left->boxes[box].center[axis], right->boxes[box].center[axis]));
            EXPECT(
                DoubleSame(left->boxes[box].halfExtent[axis], right->boxes[box].halfExtent[axis]));
        }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ExpectFieldsEqual(const SimulationCubeField *left, const SimulationCubeField *right)
{
    EXPECT(left->tickCount == right->tickCount);
    EXPECT(left->count == right->count);
    EXPECT(left->nextStableId == right->nextStableId);
    EXPECT(left->randomState == right->randomState);
    EXPECT(left->failed == right->failed);
    EXPECT(left->lastCandidatePairCount == right->lastCandidatePairCount);
    EXPECT(left->lastContactCount == right->lastContactCount);
    EXPECT(left->contactCache.contactCount == right->contactCache.contactCount);
    EXPECT(left->contactCache.matchedContactCount == right->contactCache.matchedContactCount);
    if (left->count != right->count)
    {
        return;
    }
    for (uint32_t index = 0u; index < left->count; ++index)
    {
        const VoxelRigidBody *a = &left->bodies[index];
        const VoxelRigidBody *b = &right->bodies[index];
        EXPECT(a->stableId == b->stableId);
        EXPECT(a->active == b->active);
        EXPECT(a->sleeping == b->sleeping);
        EXPECT(a->sleepCounter == b->sleepCounter);
        for (int32_t component = 0; component < 4; ++component)
        {
            EXPECT(DoubleSame(a->orientation[component], b->orientation[component]));
        }
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT(DoubleSame(a->halfExtent[axis], b->halfExtent[axis]));
            EXPECT(DoubleSame(a->inverseInertia[axis], b->inverseInertia[axis]));
            EXPECT(CoordSame(&a->position[axis], &b->position[axis]));
            EXPECT(CoordSame(&a->linearVelocity[axis], &b->linearVelocity[axis]));
            EXPECT(CoordSame(&a->angularVelocity[axis], &b->angularVelocity[axis]));
        }
        EXPECT(DoubleSame(a->inverseMass, b->inverseMass));
        EXPECT(DoubleSame(a->restitution, b->restitution));
        EXPECT(DoubleSame(a->friction, b->friction));
        ExpectShapesEqual(&left->shapes[index], &right->shapes[index]);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ExpectConstructBodiesEqual(const ConstructSystem *left, const ConstructSystem *right)
{
    EXPECT(left->capacity == right->capacity);
    for (uint32_t slot = 0u; slot < left->capacity && slot < right->capacity; ++slot)
    {
        const ConstructBody *a = &left->bodies[slot];
        const ConstructBody *b = &right->bodies[slot];
        EXPECT(a->active == b->active);
        if (!a->active || !b->active)
        {
            continue;
        }
        EXPECT(a->id == b->id);
        EXPECT(a->bodyIndex == b->bodyIndex);
        EXPECT(a->blockCount == b->blockCount);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT(a->minimum[axis] == b->minimum[axis]);
            EXPECT(a->maximum[axis] == b->maximum[axis]);
            EXPECT(DoubleSame(a->localCOM[axis], b->localCOM[axis]));
        }
        if (a->blockCount != b->blockCount)
        {
            continue;
        }
        for (uint32_t block = 0u; block < a->blockCount; ++block)
        {
            EXPECT(a->blocks[block].material == b->blocks[block].material);
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                EXPECT(a->blocks[block].local[axis] == b->blocks[block].local[axis]);
            }
        }
        if (a->bodyIndex >= left->field->count || b->bodyIndex >= right->field->count)
        {
            continue;
        }
        uint32_t boxCountA = left->field->shapes[a->bodyIndex].boxCount;
        uint32_t boxCountB = right->field->shapes[b->bodyIndex].boxCount;
        EXPECT(boxCountA == boxCountB);
        if (boxCountA != boxCountB)
        {
            continue;
        }
        for (uint32_t box = 0u; box < boxCountA; ++box)
        {
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                EXPECT(
                    DoubleSame(a->shapeBoxes[box].center[axis], b->shapeBoxes[box].center[axis]));
                EXPECT(DoubleSame(a->shapeBoxes[box].halfExtent[axis],
                                  b->shapeBoxes[box].halfExtent[axis]));
            }
        }
    }
}

static uint64_t HashMix(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t HashCoord(uint64_t hash, const InfiniteCoord *value)
{
    int32_t sign = InfiniteCoordSign(value);
    hash = HashMix(hash, &sign, sizeof(sign));
    hash = HashMix(hash, &value->limbCount, sizeof(value->limbCount));
    if (value->limbCount > 0u && value->limbs != NULL)
    {
        hash = HashMix(hash, value->limbs, (size_t)value->limbCount * sizeof(uint64_t));
    }
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
    hash = HashMix(hash, &field->nextStableId, sizeof(field->nextStableId));
    hash = HashMix(hash, &field->randomState, sizeof(field->randomState));
    hash = HashMix(hash, &field->lastContactCount, sizeof(field->lastContactCount));
    hash =
        HashMix(hash, &field->contactCache.contactCount, sizeof(field->contactCache.contactCount));
    for (uint32_t index = 0u; index < field->count; ++index)
    {
        const VoxelRigidBody *body = &field->bodies[index];
        hash = HashMix(hash, &body->stableId, sizeof(body->stableId));
        hash = HashMix(hash, &body->active, sizeof(body->active));
        hash = HashMix(hash, &body->sleeping, sizeof(body->sleeping));
        for (int32_t component = 0; component < 4; ++component)
        {
            hash = HashMix(hash, &body->orientation[component], sizeof(double));
        }
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            hash = HashCoord(hash, &body->position[axis]);
            hash = HashCoord(hash, &body->linearVelocity[axis]);
            hash = HashCoord(hash, &body->angularVelocity[axis]);
        }
        const VoxelRigidCompoundShape *shape = &field->shapes[index];
        hash = HashMix(hash, &shape->boxCount, sizeof(shape->boxCount));
        if (shape->boxes != NULL)
        {
            hash = HashMix(hash, shape->boxes,
                           (size_t)shape->boxCount * sizeof(VoxelRigidCompoundBox));
        }
    }
    return hash;
}

// Один и тот же набор форм и поз ставится в оба мира. Затем оба прогоняются
// тик в тик, сравниваются канонические состояния, выполняется огромный rebase
// со сдвигом в 2^40 блоков (bigint обязан дать больше одного лимба) и возврат.
static void TestComplexCollisionAndReplay(uint32_t ticks)
{
    TestRig runs[2];
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        memset(&runs[index], 0, sizeof(runs[index]));
    }
    bool initialized = TestRigInit(&runs[0]) && TestRigInit(&runs[1]);
    EXPECT(initialized);
    if (!initialized)
    {
        TestRigDestroy(&runs[0]);
        TestRigDestroy(&runs[1]);
        return;
    }

    uint32_t count = 0u;
    ConstructBlock *shape = GenerateRandomBody(257u, UINT64_C(0xC0111D), &count);
    EXPECT(shape != NULL && count == 257u);
    if (shape == NULL)
    {
        TestRigDestroy(&runs[0]);
        TestRigDestroy(&runs[1]);
        return;
    }
    int32_t minimum[3] = {shape[0].local[0], shape[0].local[1], shape[0].local[2]};
    int32_t maximum[3] = {shape[0].local[0], shape[0].local[1], shape[0].local[2]};
    for (uint32_t block = 0u; block < count; ++block)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (shape[block].local[axis] < minimum[axis])
            {
                minimum[axis] = shape[block].local[axis];
            }
            if (shape[block].local[axis] > maximum[axis])
            {
                maximum[axis] = shape[block].local[axis];
            }
        }
    }
    double span[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        span[axis] = (double)(maximum[axis] - minimum[axis] + 1);
    }
    double base[3];
    base[0] = 0.5 - (span[0] * 0.5) - (double)minimum[0];
    base[1] = 0.5 - (span[1] * 0.5) - (double)minimum[1];
    base[2] = 1.0 - (double)minimum[2];
    double top[3] = {base[0], base[1], base[2] + span[2] + TEST_COLLISION_GAP};

    uint64_t firstId = 0u;
    uint64_t secondId = 0u;
    // Отдельная комб-форма >256 детей, лежащая на полу далеко от пары: это
    // прогоняет child-BVH шага на форме, которую готовка не схлопнула в одну
    // коробку. base = 800 - 257 = 543 >= 2*257 - 1, поэтому форма валидна.
    uint32_t combCount = 0u;
    ConstructBlock *comb = GenerateCombBody(800u, 257u, 1u, &combCount);
    EXPECT(comb != NULL && combCount == 800u);
    const double combOrigin[3] = {50.0, 0.0, 1.0};
    uint32_t expectedActive = comb != NULL ? 3u : 2u;
    for (uint32_t run = 0u; run < 2u; ++run)
    {
        EXPECT(ConstructSpawn(runs[run].system, shape, count, base, &firstId));
        EXPECT(ConstructSpawn(runs[run].system, shape, count, top, &secondId));
        if (comb != NULL)
        {
            EXPECT(ConstructSpawn(runs[run].system, comb, combCount, combOrigin, NULL));
        }
        EXPECT(ConstructActiveBodyCount(runs[run].system) == expectedActive);
    }

    const double spawnPosition[3] = {200.0, 200.0, 40.0};
    bool stepFailed = false;
    bool contactSeen = false;
    double closest = span[2] + TEST_COLLISION_GAP;
    double lowest = INFINITY;
    for (uint32_t tick = 0u; tick < ticks && !stepFailed; ++tick)
    {
        for (uint32_t run = 0u; run < 2u; ++run)
        {
            if (!SimulationCubeFieldAdvanceTick(&runs[run].field, runs[run].world, spawnPosition))
            {
                stepFailed = true;
                break;
            }
        }
        if (runs[0].field.lastContactCount > 0u || runs[1].field.lastContactCount > 0u)
        {
            contactSeen = true;
        }
        uint32_t firstSlot = FindSlotById(runs[0].system, firstId);
        uint32_t secondSlot = FindSlotById(runs[0].system, secondId);
        if (firstSlot < runs[0].system->capacity && secondSlot < runs[0].system->capacity)
        {
            double firstMin, firstMax, secondMin, secondMax;
            BodyWorldZRange(runs[0].system, firstSlot, &firstMin, &firstMax);
            BodyWorldZRange(runs[0].system, secondSlot, &secondMin, &secondMax);
            if (secondMin - firstMax < closest)
            {
                closest = secondMin - firstMax;
            }
            double candidate = firstMin < secondMin ? firstMin : secondMin;
            if (candidate < lowest)
            {
                lowest = candidate;
            }
        }
        ExpectFieldsEqual(&runs[0].field, &runs[1].field);
        ExpectConstructBodiesEqual(runs[0].system, runs[1].system);
    }
    EXPECT(!stepFailed);
    EXPECT(contactSeen);
    // Постройки стартовали с зазором 0.2 и обязаны сблизиться до касания.
    EXPECT(closest < 0.1);
    // Опора не пробита.
    EXPECT(lowest > -0.05);
    EXPECT(!runs[0].field.failed && !runs[1].field.failed);
    EXPECT(SystemMinVertexZ(runs[0].system) > -0.05);

    ExpectFieldsEqual(&runs[0].field, &runs[1].field);
    ExpectConstructBodiesEqual(runs[0].system, runs[1].system);
    uint64_t hashBefore = FieldHash(&runs[0].field);
    EXPECT(hashBefore == FieldHash(&runs[1].field));

    // Огромный целочисленный сдвиг. Движок ограничивает rebase величиной
    // 2^31-1 блоков, поэтому берём 2^30: позиция в масштабе 2^32 даёт значение
    // больше 2^53, которое double уже не хранит побитово. Точность проверяется
    // возвратом сдвига и сравнением полных лимбов bigint.
    const int64_t shiftUp[3] = {INT64_C(1) << 30, -(INT64_C(1) << 30), INT64_C(1) << 29};
    const int64_t shiftDown[3] = {-shiftUp[0], -shiftUp[1], -shiftUp[2]};
    SimulationCubeFieldRebase(&runs[0].field, shiftUp);
    EXPECT(!runs[0].field.failed);
    bool beyondDouble = false;
    for (uint32_t index = 0u; index < runs[0].field.count; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            const InfiniteCoord *position = &runs[0].field.bodies[index].position[axis];
            if (position->limbCount >= 1u && position->limbs != NULL &&
                position->limbs[0] > (UINT64_C(1) << 53))
            {
                beyondDouble = true;
            }
        }
    }
    EXPECT(beyondDouble);
    SimulationCubeFieldRebase(&runs[0].field, shiftDown);
    EXPECT(!runs[0].field.failed);
    ExpectFieldsEqual(&runs[0].field, &runs[1].field);
    ExpectConstructBodiesEqual(runs[0].system, runs[1].system);
    EXPECT(FieldHash(&runs[0].field) == hashBefore);

    printf("construct_complex replay: ticks=%" PRIu32 " closest=%0.6f min_z=%0.6f hash=%" PRIu64
           "\n",
           ticks, closest, lowest, hashBefore);

    free(comb);
    free(shape);
    TestRigDestroy(&runs[0]);
    TestRigDestroy(&runs[1]);
}

// ---------------------------------------------------------------------------
// Опциональный --stress. Это тяжёлый сценарий вне дефолтного CI: он печатает
// wall time и валидность, но никогда не делает время критерием прохождения.

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int RunStress(uint32_t ticks, uint32_t bodyCount, uint32_t blockCount)
{
    if (bodyCount == 0u)
    {
        bodyCount = TEST_STRESS_DEFAULT_BODIES;
    }
    if (blockCount < 2u)
    {
        blockCount = TEST_STRESS_DEFAULT_BLOCKS;
    }
    TestRig rig;
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(&rig, 0, sizeof(rig));
    clock_t generateStart = clock();
    if (!TestRigInit(&rig))
    {
        EXPECT(false);
        TestRigDestroy(&rig);
        return failures == 0 ? 0 : 1;
    }
    ConstructBlock **shapes = (ConstructBlock **)calloc((size_t)bodyCount, sizeof(*shapes));
    uint32_t *counts = (uint32_t *)calloc((size_t)bodyCount, sizeof(*counts));
    EXPECT(shapes != NULL && counts != NULL);
    if (shapes == NULL || counts == NULL)
    {
        free(shapes);
        free(counts);
        TestRigDestroy(&rig);
        return failures == 0 ? 0 : 1;
    }
    for (uint32_t index = 0u; index < bodyCount; ++index)
    {
        if (index % 3u == 0u)
        {
            uint32_t teeth = (blockCount + 1u) / 4u;
            shapes[index] = GenerateCombBody(blockCount, teeth, 1u, &counts[index]);
        }
        if (shapes[index] == NULL)
        {
            shapes[index] =
                GenerateRandomBody(blockCount, UINT64_C(0xA11CE) + index, &counts[index]);
        }
        EXPECT(shapes[index] != NULL && counts[index] == blockCount);
    }
    double generateSeconds = (double)(clock() - generateStart) / (double)CLOCKS_PER_SEC;

    clock_t spawnStart = clock();
    for (uint32_t index = 0u; index < bodyCount && shapes[index] != NULL; ++index)
    {
        const double origin[3] = {(double)(index % 4u) * 20.0, (double)(index / 4u) * 20.0,
                                  30.0 + (double)(index % 3u) * 4.0};
        EXPECT(ConstructSpawn(rig.system, shapes[index], counts[index], origin, NULL));
    }
    double spawnSeconds = (double)(clock() - spawnStart) / (double)CLOCKS_PER_SEC;

    const double spawnPosition[3] = {200.0, 200.0, 60.0};
    clock_t physicsStart = clock();
    bool stepFailed = false;
    double lowest = INFINITY;
    uint32_t peakContacts = 0u;
    for (uint32_t tick = 0u; tick < ticks && !stepFailed; ++tick)
    {
        if (!SimulationCubeFieldAdvanceTick(&rig.field, rig.world, spawnPosition))
        {
            stepFailed = true;
            break;
        }
        if (rig.field.lastContactCount > peakContacts)
        {
            peakContacts = rig.field.lastContactCount;
        }
        double minimum = SystemMinVertexZ(rig.system);
        if (minimum < lowest)
        {
            lowest = minimum;
        }
    }
    double physicsSeconds = (double)(clock() - physicsStart) / (double)CLOCKS_PER_SEC;
    uint64_t hash = FieldHash(&rig.field);

    EXPECT(!stepFailed);
    EXPECT(!rig.field.failed);
    EXPECT(isfinite(lowest) && lowest > -1.0);
    uint32_t active = ConstructActiveBodyCount(rig.system);
    EXPECT(active == bodyCount);
    uint64_t totalBlocks = (uint64_t)bodyCount * (uint64_t)blockCount;

    printf("construct_complex stress: bodies=%" PRIu32 " blocks/body=%" PRIu32 " ticks=%" PRIu32
           " blocks total=%" PRIu64 "\n",
           bodyCount, blockCount, ticks, totalBlocks);
    printf("  active=%" PRIu32 " awake=%" PRIu32 " peak_contacts=%" PRIu32 " min_vertex_z=%0.6f\n",
           active, SimulationCubeFieldAwakeCount(&rig.field), peakContacts, lowest);
    printf("  wall generate=%0.3fs spawn=%0.3fs physics=%0.3fs (informational, not a pass gate)\n",
           generateSeconds, spawnSeconds, physicsSeconds);
    printf("  replay hash=%" PRIu64 " errors=%d\n", hash, failures);
    printf("  note: measured for this scenario only; no general no-lag claim is made\n");

    for (uint32_t index = 0u; index < bodyCount; ++index)
    {
        free(shapes[index]);
    }
    free(shapes);
    free(counts);
    TestRigDestroy(&rig);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Разбор аргументов.

static bool ParseUnsigned(const char *text, uint32_t *out)
{
    if (text == NULL || *text == '\0' || out == NULL)
    {
        return false;
    }
    uint64_t value = 0u;
    for (const char *character = text; *character != '\0'; ++character)
    {
        if (*character < '0' || *character > '9')
        {
            return false;
        }
        value = value * 10u + (uint64_t)(*character - '0');
        if (value > (uint64_t)UINT32_MAX)
        {
            return false;
        }
    }
    if (value == 0u)
    {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool MatchesOption(const char *argument, const char *name, const char **outValue)
{
    size_t length = strlen(name);
    if (strncmp(argument, name, length) != 0 || argument[length] != '=')
    {
        return false;
    }
    *outValue = argument + length + 1u;
    return true;
}

int main(int argc, char **argv)
{
    bool stress = false;
    bool ticksSet = false;
    uint32_t ticks = TEST_DEFAULT_TICKS;
    uint32_t stressBodies = TEST_STRESS_DEFAULT_BODIES;
    uint32_t stressBlocks = TEST_STRESS_DEFAULT_BLOCKS;
    for (int32_t index = 1; index < argc; ++index)
    {
        const char *value = NULL;
        if (strcmp(argv[index], "--stress") == 0)
        {
            stress = true;
        }
        else if (MatchesOption(argv[index], "--ticks", &value))
        {
            if (!ParseUnsigned(value, &ticks))
            {
                TEST_FPRINTF(stderr, "invalid --ticks value: %s\n", value);
                return 2;
            }
            ticksSet = true;
        }
        else if (MatchesOption(argv[index], "--bodies", &value))
        {
            if (!ParseUnsigned(value, &stressBodies))
            {
                TEST_FPRINTF(stderr, "invalid --bodies value: %s\n", value);
                return 2;
            }
        }
        else if (MatchesOption(argv[index], "--blocks", &value))
        {
            if (!ParseUnsigned(value, &stressBlocks))
            {
                TEST_FPRINTF(stderr, "invalid --blocks value: %s\n", value);
                return 2;
            }
        }
        else
        {
            TEST_FPRINTF(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }

    if (stress)
    {
        if (!ticksSet)
        {
            ticks = TEST_STRESS_DEFAULT_TICKS;
        }
        RunStress(ticks, stressBodies, stressBlocks);
    }
    else
    {
        TestGeneratedGeometry();
        TestCavityRaycasts();
        TestComplexCollisionAndReplay(ticks);
    }

    if (failures == 0)
    {
        printf("construct_complex: %s checks passed\n", stress ? "stress" : "default");
        return 0;
    }
    TEST_FPRINTF(stderr, "construct_complex: %d expectation(s) failed\n", failures);
    return 1;
}
