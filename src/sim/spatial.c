#include "sim/spatial.h"

#include "sim/cells.h"
#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>
#include <string.h>

/* Results up to this many are collected on the stack; a larger capacity
 * allocates once per query. Neighbour queries are usually small. */
#define STACK_CANDIDATES 64u
/* The pool starts small and doubles; the creation hint normally sizes it. */
#define INITIAL_ENTRIES 64u

/* One being in the pool. `next` is the index plus one of the next entry in the
 * same cell, or 0 at the end of the chain. */
typedef struct SpatialEntry
{
    uint64_t serial;
    SosPosition position;
    uint32_t next;
} SpatialEntry;

/* One nearest-first result before it is written out. */
typedef struct Candidate
{
    uint64_t distanceSquared;
    uint64_t serial;
} Candidate;

struct SosSpatial
{
    unsigned cellShift;
    /* A cell key to the head of its entry chain (index plus one, 0 empty). */
    SosTable *cells;
    SpatialEntry *entries;
    uint32_t entryCapacity;
    uint32_t entryCount;
};

/* a * b, saturated at UINT64_MAX. */
static uint64_t MulClamp(uint64_t a, uint64_t b)
{
    return a != 0u && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}

/* a + b, saturated at the signed limits. */
static int64_t SaturatingAdd(int64_t a, int64_t b)
{
    if (b > 0 && a > INT64_MAX - b)
    {
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b)
    {
        return INT64_MIN;
    }
    return a + b;
}

/* a - b, saturated at the signed limits. */
static int64_t SaturatingSub(int64_t a, int64_t b)
{
    if (b < 0 && a > INT64_MAX + b)
    {
        return INT64_MAX;
    }
    if (b > 0 && a < INT64_MIN + b)
    {
        return INT64_MIN;
    }
    return a - b;
}

/* last - first + 1, held as an unsigned count so a range spanning the whole
 * ladder of cells never overflows. */
static uint64_t CellCount(int64_t first, int64_t last)
{
    if (last < first)
    {
        return 0u;
    }
    const uint64_t span = (uint64_t)last - (uint64_t)first;
    return span == UINT64_MAX ? UINT64_MAX : span + 1u;
}

/* True when `a` should be dropped before `b`: farther, or equally far but a
 * larger serial. The heap root is therefore the worst result kept. */
static bool Worse(Candidate a, Candidate b)
{
    if (a.distanceSquared != b.distanceSquared)
    {
        return a.distanceSquared > b.distanceSquared;
    }
    return a.serial > b.serial;
}

static void SwapCandidate(Candidate *a, Candidate *b)
{
    const Candidate temporary = *a;
    *a = *b;
    *b = temporary;
}

static void HeapSiftUp(Candidate *heap, uint32_t index)
{
    while (index != 0u)
    {
        const uint32_t parent = (index - 1u) / 2u;
        if (!Worse(heap[index], heap[parent]))
        {
            break;
        }
        SwapCandidate(&heap[index], &heap[parent]);
        index = parent;
    }
}

static void HeapSiftDown(Candidate *heap, uint32_t size)
{
    uint32_t index = 0u;
    for (;;)
    {
        /* Widened so a heap near UINT32_MAX cannot overflow the double index. */
        const uint64_t left = (uint64_t)index * 2u + 1u;
        if (left >= (uint64_t)size)
        {
            break;
        }
        uint32_t worst = (uint32_t)left;
        const uint32_t right = (uint32_t)(left + 1u);
        if (right < size && Worse(heap[right], heap[left]))
        {
            worst = right;
        }
        if (!Worse(heap[worst], heap[index]))
        {
            break;
        }
        SwapCandidate(&heap[index], &heap[worst]);
        index = worst;
    }
}

/* Keeps the `bound` smallest candidates seen so far in a max-heap. */
static void HeapOffer(Candidate *heap, uint32_t *size, uint32_t bound, Candidate candidate)
{
    if (*size < bound)
    {
        heap[*size] = candidate;
        *size += 1u;
        HeapSiftUp(heap, *size - 1u);
    }
    else if (bound != 0u && Worse(heap[0], candidate))
    {
        heap[0] = candidate;
        HeapSiftDown(heap, *size);
    }
}

/* Grows the entry pool so it holds at least `needed` entries. On failure the
 * object keeps its old pool. */
static bool EnsureCapacity(SosSpatial *spatial, uint32_t needed)
{
    if (needed <= spatial->entryCapacity)
    {
        return true;
    }
    uint32_t capacity = spatial->entryCapacity == 0u ? INITIAL_ENTRIES : spatial->entryCapacity;
    while (capacity < needed)
    {
        if (capacity > UINT32_MAX / 2u)
        {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    SpatialEntry *entries = malloc((size_t)capacity * sizeof(*entries));
    if (entries == NULL)
    {
        return false;
    }
    if (spatial->entryCount != 0u)
    {
        memcpy(entries, spatial->entries, (size_t)spatial->entryCount * sizeof(*entries));
    }
    free(spatial->entries);
    spatial->entries = entries;
    spatial->entryCapacity = capacity;
    return true;
}

/* Best-effort preallocation of room for `count` beings. */
static bool Reserve(SosSpatial *spatial, uint32_t count)
{
    return EnsureCapacity(spatial, count) && SosTableReserve(spatial->cells, count);
}

SosSpatial *SosSpatialCreate(unsigned cellShift, uint32_t capacityHint)
{
    if (cellShift > SOS_SPATIAL_MAX_CELL_SHIFT)
    {
        return NULL;
    }
    SosSpatial *spatial = calloc(1u, sizeof(*spatial));
    if (spatial == NULL)
    {
        return NULL;
    }
    spatial->cellShift = cellShift;
    spatial->cells = SosTableCreate(sizeof(uint32_t));
    if (spatial->cells == NULL)
    {
        free(spatial);
        return NULL;
    }
    if (capacityHint != 0u)
    {
        /* A hint that cannot be met only makes the next inserts grow instead. */
        (void)Reserve(spatial, capacityHint);
    }
    return spatial;
}

void SosSpatialDestroy(SosSpatial *spatial)
{
    if (spatial != NULL)
    {
        SosTableDestroy(spatial->cells);
        free(spatial->entries);
        free(spatial);
    }
}

void SosSpatialClear(SosSpatial *spatial)
{
    if (spatial != NULL)
    {
        spatial->entryCount = 0u;
        SosTableClear(spatial->cells);
    }
}

bool SosSpatialInsert(SosSpatial *spatial, uint64_t serial, SosPosition position)
{
    if (spatial == NULL || serial == 0u || spatial->entryCount == UINT32_MAX)
    {
        return false;
    }
    if (!EnsureCapacity(spatial, spatial->entryCount + 1u))
    {
        return false;
    }
    const int64_t cellX = SosCellsIndex(position.x, spatial->cellShift);
    const int64_t cellY = SosCellsIndex(position.y, spatial->cellShift);
    uint32_t *head = SosTableInsert(
        spatial->cells, (SosTableKey){.first = (uint64_t)cellX, .second = (uint64_t)cellY}, NULL);
    if (head == NULL)
    {
        return false;
    }
    const uint32_t index = spatial->entryCount;
    spatial->entries[index] = (SpatialEntry){.serial = serial, .position = position, .next = *head};
    *head = index + 1u;
    spatial->entryCount += 1u;
    return true;
}

/* Offers every entry of one cell whose ground distance is within reach. */
static void VisitRadius(const SosSpatial *spatial, uint32_t head, SosPosition center,
                        uint64_t radiusSquared, uint64_t exclude, Candidate *candidates,
                        uint32_t *size, uint32_t bound, uint32_t *found)
{
    for (uint32_t link = head; link != 0u; link = spatial->entries[link - 1u].next)
    {
        const SpatialEntry *entry = &spatial->entries[link - 1u];
        if (entry->serial != exclude)
        {
            const uint64_t distance = SosCellsGroundDistanceSquared(center, entry->position);
            if (distance <= radiusSquared)
            {
                *found += 1u;
                HeapOffer(candidates, size, bound,
                          (Candidate){.distanceSquared = distance, .serial = entry->serial});
            }
        }
    }
}

/* Counts every entry of one cell inside a ground-plane box. */
static void VisitBox(const SosSpatial *spatial, uint32_t head, int64_t minX, int64_t minY,
                     int64_t maxX, int64_t maxY, uint32_t *found)
{
    for (uint32_t link = head; link != 0u; link = spatial->entries[link - 1u].next)
    {
        const SosPosition position = spatial->entries[link - 1u].position;
        if (position.x >= minX && position.x <= maxX && position.y >= minY && position.y <= maxY)
        {
            *found += 1u;
        }
    }
}

static uint32_t QueryRadius(const SosSpatial *spatial, SosPosition center, int64_t radius,
                            uint64_t *outSerials, uint32_t capacity, uint64_t exclude)
{
    const uint64_t radiusSquared = SosCellsSquared((uint64_t)radius);
    const int64_t firstX = SosCellsIndex(SaturatingSub(center.x, radius), spatial->cellShift);
    const int64_t lastX = SosCellsIndex(SaturatingAdd(center.x, radius), spatial->cellShift);
    const int64_t firstY = SosCellsIndex(SaturatingSub(center.y, radius), spatial->cellShift);
    const int64_t lastY = SosCellsIndex(SaturatingAdd(center.y, radius), spatial->cellShift);
    const uint64_t columns = CellCount(firstX, lastX);
    const uint64_t rows = CellCount(firstY, lastY);
    const uint32_t bound = capacity < spatial->entryCount ? capacity : spatial->entryCount;
    Candidate stackCandidates[STACK_CANDIDATES];
    Candidate *candidates = stackCandidates;
    if (bound > STACK_CANDIDATES)
    {
        candidates = malloc((size_t)bound * sizeof(*candidates));
        if (candidates == NULL)
        {
            return 0u;
        }
    }
    uint32_t size = 0u;
    uint32_t found = 0u;
    /* Walking the pool beats visiting a huge box, and visiting a small box
     * beats walking a pool of millions: pick the cheaper traversal. */
    if (MulClamp(columns, rows) <= (uint64_t)SosTableCount(spatial->cells))
    {
        for (uint64_t row = 0u; row < rows; ++row)
        {
            const int64_t cellY = (int64_t)((uint64_t)firstY + row);
            for (uint64_t column = 0u; column < columns; ++column)
            {
                const int64_t cellX = (int64_t)((uint64_t)firstX + column);
                const uint32_t *head =
                    SosTableFind(spatial->cells, (SosTableKey){.first = (uint64_t)cellX,
                                                               .second = (uint64_t)cellY});
                if (head != NULL)
                {
                    VisitRadius(spatial, *head, center, radiusSquared, exclude, candidates, &size,
                                bound, &found);
                }
            }
        }
    }
    else
    {
        const uint32_t slots = SosTableSlotCount(spatial->cells);
        for (uint32_t slot = 0u; slot < slots; ++slot)
        {
            const void *record = NULL;
            if (SosTableSlot(spatial->cells, slot, NULL, &record))
            {
                VisitRadius(spatial, *(const uint32_t *)record, center, radiusSquared, exclude,
                            candidates, &size, bound, &found);
            }
        }
    }
    /* Max-heap sort into ascending (nearest-first) order. */
    uint32_t remaining = size;
    while (remaining > 1u)
    {
        SwapCandidate(&candidates[0], &candidates[remaining - 1u]);
        remaining -= 1u;
        HeapSiftDown(candidates, remaining);
    }
    for (uint32_t index = 0u; index < size && index < capacity; ++index)
    {
        outSerials[index] = candidates[index].serial;
    }
    if (candidates != stackCandidates)
    {
        free(candidates);
    }
    return found;
}

uint32_t SosSpatialQueryRadius(const SosSpatial *spatial, SosPosition center, int64_t radius,
                               uint64_t *outSerials, uint32_t capacity)
{
    if (spatial == NULL || radius < 0 || (outSerials == NULL && capacity != 0u))
    {
        return 0u;
    }
    return QueryRadius(spatial, center, radius, outSerials, capacity, 0u);
}

bool SosSpatialNearest(const SosSpatial *spatial, SosPosition center, int64_t maxRadius,
                       uint64_t excludeSerial, uint64_t *outSerial)
{
    if (spatial == NULL || outSerial == NULL || maxRadius < 0)
    {
        return false;
    }
    uint64_t nearest = 0u;
    if (QueryRadius(spatial, center, maxRadius, &nearest, 1u, excludeSerial) == 0u)
    {
        return false;
    }
    *outSerial = nearest;
    return true;
}

uint32_t SosSpatialCountInBox(const SosSpatial *spatial, int64_t minX, int64_t minY, int64_t maxX,
                              int64_t maxY)
{
    if (spatial == NULL || maxX < minX || maxY < minY)
    {
        return 0u;
    }
    const int64_t firstX = SosCellsIndex(minX, spatial->cellShift);
    const int64_t lastX = SosCellsIndex(maxX, spatial->cellShift);
    const int64_t firstY = SosCellsIndex(minY, spatial->cellShift);
    const int64_t lastY = SosCellsIndex(maxY, spatial->cellShift);
    const uint64_t columns = CellCount(firstX, lastX);
    const uint64_t rows = CellCount(firstY, lastY);
    uint32_t found = 0u;
    if (MulClamp(columns, rows) <= (uint64_t)SosTableCount(spatial->cells))
    {
        for (uint64_t row = 0u; row < rows; ++row)
        {
            const int64_t cellY = (int64_t)((uint64_t)firstY + row);
            for (uint64_t column = 0u; column < columns; ++column)
            {
                const int64_t cellX = (int64_t)((uint64_t)firstX + column);
                const uint32_t *head =
                    SosTableFind(spatial->cells, (SosTableKey){.first = (uint64_t)cellX,
                                                               .second = (uint64_t)cellY});
                if (head != NULL)
                {
                    VisitBox(spatial, *head, minX, minY, maxX, maxY, &found);
                }
            }
        }
    }
    else
    {
        const uint32_t slots = SosTableSlotCount(spatial->cells);
        for (uint32_t slot = 0u; slot < slots; ++slot)
        {
            const void *record = NULL;
            if (SosTableSlot(spatial->cells, slot, NULL, &record))
            {
                VisitBox(spatial, *(const uint32_t *)record, minX, minY, maxX, maxY, &found);
            }
        }
    }
    return found;
}

uint32_t SosSpatialCount(const SosSpatial *spatial)
{
    return spatial != NULL ? spatial->entryCount : 0u;
}

uint64_t SosSpatialHash(const SosSpatial *spatial)
{
    if (spatial == NULL)
    {
        return 0u;
    }
    /* A sum makes the fold independent of the order the pool was built in. */
    uint64_t sum = 0u;
    for (uint32_t index = 0u; index < spatial->entryCount; ++index)
    {
        const SpatialEntry *entry = &spatial->entries[index];
        uint64_t folded = SosHashCombine(SosHash64(entry->serial), (uint64_t)entry->position.x);
        folded = SosHashCombine(folded, (uint64_t)entry->position.y);
        folded = SosHashCombine(folded, (uint64_t)entry->position.z);
        sum += folded;
    }
    return SosHashCombine(SosHashCombine((uint64_t)spatial->cellShift, sum),
                          (uint64_t)spatial->entryCount);
}
