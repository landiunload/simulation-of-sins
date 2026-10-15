#include "sim/table.h"

#include "sim/random.h"

#include <stdlib.h>
#include <string.h>

#define MIN_CAPACITY 16u

struct SosTable
{
    size_t recordSize;
    SosTableKey *keys;
    /* Non-zero for an occupied slot. */
    uint8_t *used;
    unsigned char *records;
    /* Zero or a power of two, at least twice the count. */
    uint32_t capacity;
    uint32_t count;
};

SosTable *SosTableCreate(size_t recordSize)
{
    if (recordSize > SOS_TABLE_MAX_RECORD_SIZE)
    {
        return NULL;
    }
    SosTable *table = calloc(1u, sizeof(*table));
    if (table != NULL)
    {
        table->recordSize = recordSize;
    }
    return table;
}

void SosTableDestroy(SosTable *table)
{
    if (table != NULL)
    {
        free(table->keys);
        free(table->used);
        free(table->records);
        free(table);
    }
}

uint32_t SosTableCount(const SosTable *table)
{
    return table != NULL ? table->count : 0u;
}

static bool KeysEqual(SosTableKey a, SosTableKey b)
{
    return a.first == b.first && a.second == b.second;
}

static uint32_t Home(const SosTable *table, SosTableKey key)
{
    const uint64_t hash = SosHashCombine(SosHash64(key.first), key.second);
    return (uint32_t)(hash & (table->capacity - 1u));
}

static unsigned char *Record(const SosTable *table, uint32_t slot)
{
    /* A set has no record storage; any non-NULL pointer marks presence. */
    return table->records != NULL ? table->records + (size_t)slot * table->recordSize
                                  : (unsigned char *)table->used + slot;
}

/* The slot holding `key`, or the capacity when it is missing. */
static uint32_t Locate(const SosTable *table, SosTableKey key)
{
    if (table->capacity == 0u)
    {
        return 0u;
    }
    const uint32_t mask = table->capacity - 1u;
    for (uint32_t slot = Home(table, key);; slot = (slot + 1u) & mask)
    {
        if (table->used[slot] == 0u)
        {
            return table->capacity;
        }
        if (KeysEqual(table->keys[slot], key))
        {
            return slot;
        }
    }
}

const void *SosTableFind(const SosTable *table, SosTableKey key)
{
    if (table == NULL)
    {
        return NULL;
    }
    const uint32_t slot = Locate(table, key);
    return slot < table->capacity ? Record(table, slot) : NULL;
}

void *SosTableEdit(SosTable *table, SosTableKey key)
{
    return (void *)SosTableFind(table, key);
}

static uint32_t Place(SosTable *table, SosTableKey key)
{
    const uint32_t mask = table->capacity - 1u;
    uint32_t slot = Home(table, key);
    while (table->used[slot] != 0u)
    {
        slot = (slot + 1u) & mask;
    }
    table->used[slot] = 1u;
    table->keys[slot] = key;
    return slot;
}

static bool Rebuild(SosTable *table, uint32_t capacity)
{
    /* calloc rejects sizes that overflow, on 32-bit targets too. */
    SosTableKey *keys = calloc(capacity, sizeof(*keys));
    uint8_t *used = calloc(capacity, sizeof(*used));
    unsigned char *records = table->recordSize != 0u ? calloc(capacity, table->recordSize) : NULL;
    if (keys == NULL || used == NULL || (table->recordSize != 0u && records == NULL))
    {
        free(keys);
        free(used);
        free(records);
        return false;
    }
    SosTable old = *table;
    table->keys = keys;
    table->used = used;
    table->records = records;
    table->capacity = capacity;
    for (uint32_t slot = 0u; slot < old.capacity; ++slot)
    {
        if (old.used[slot] != 0u)
        {
            const uint32_t placed = Place(table, old.keys[slot]);
            if (table->recordSize != 0u)
            {
                memcpy(Record(table, placed), Record(&old, slot), table->recordSize);
            }
        }
    }
    free(old.keys);
    free(old.used);
    free(old.records);
    return true;
}

static uint32_t CapacityFor(uint32_t count)
{
    uint32_t capacity = MIN_CAPACITY;
    while (capacity / 2u < count)
    {
        capacity *= 2u;
    }
    return capacity;
}

bool SosTableReserve(SosTable *table, uint32_t count)
{
    if (table == NULL || count > SOS_TABLE_MAX_COUNT)
    {
        return false;
    }
    const uint32_t capacity = CapacityFor(count);
    return capacity <= table->capacity || Rebuild(table, capacity);
}

void *SosTableInsert(SosTable *table, SosTableKey key, bool *outCreated)
{
    if (outCreated != NULL)
    {
        *outCreated = false;
    }
    if (table == NULL)
    {
        return NULL;
    }
    const uint32_t found = Locate(table, key);
    if (found < table->capacity)
    {
        return Record(table, found);
    }
    if (table->count >= SOS_TABLE_MAX_COUNT || !SosTableReserve(table, table->count + 1u))
    {
        return NULL;
    }
    const uint32_t slot = Place(table, key);
    table->count += 1u;
    unsigned char *record = Record(table, slot);
    if (table->recordSize != 0u)
    {
        memset(record, 0, table->recordSize);
    }
    if (outCreated != NULL)
    {
        *outCreated = true;
    }
    return record;
}

/* Empties `slot` and shifts the records after it back, so every record stays
 * reachable from its home slot without tombstones. */
static void Vacate(SosTable *table, uint32_t slot)
{
    const uint32_t mask = table->capacity - 1u;
    uint32_t hole = slot;
    for (uint32_t next = (hole + 1u) & mask; table->used[next] != 0u; next = (next + 1u) & mask)
    {
        const uint32_t home = Home(table, table->keys[next]);
        /* Movable when the hole lies between its home and its slot. */
        if (((next - home) & mask) >= ((next - hole) & mask))
        {
            table->keys[hole] = table->keys[next];
            if (table->recordSize != 0u)
            {
                memcpy(Record(table, hole), Record(table, next), table->recordSize);
            }
            hole = next;
        }
    }
    table->used[hole] = 0u;
    table->count -= 1u;
}

bool SosTableRemove(SosTable *table, SosTableKey key)
{
    if (table == NULL)
    {
        return false;
    }
    const uint32_t slot = Locate(table, key);
    if (slot >= table->capacity)
    {
        return false;
    }
    Vacate(table, slot);
    return true;
}

uint32_t SosTableRemoveIf(SosTable *table,
                          bool (*drop)(SosTableKey key, const void *record, void *context),
                          void *context)
{
    if (table == NULL || drop == NULL || table->count == 0u)
    {
        return 0u;
    }
    const uint32_t mask = table->capacity - 1u;
    /* Start after an empty slot, which exists because the table is at most
     * half full: shifting then only moves records not yet visited, and the
     * one moved into the current slot is visited next. */
    uint32_t start = 0u;
    while (table->used[start] != 0u)
    {
        start += 1u;
    }
    uint32_t removed = 0u;
    for (uint32_t step = 0u; step < table->capacity;)
    {
        const uint32_t slot = (start + 1u + step) & mask;
        if (table->used[slot] != 0u && drop(table->keys[slot], Record(table, slot), context))
        {
            Vacate(table, slot);
            removed += 1u;
        }
        else
        {
            step += 1u;
        }
    }
    return removed;
}

void SosTableClear(SosTable *table)
{
    if (table != NULL && table->capacity != 0u)
    {
        memset(table->used, 0, table->capacity);
        table->count = 0u;
    }
}

uint32_t SosTableSlotCount(const SosTable *table)
{
    return table != NULL ? table->capacity : 0u;
}

bool SosTableSlot(const SosTable *table, uint32_t slot, SosTableKey *outKey, const void **outRecord)
{
    if (table == NULL || slot >= table->capacity || table->used[slot] == 0u)
    {
        return false;
    }
    if (outKey != NULL)
    {
        *outKey = table->keys[slot];
    }
    if (outRecord != NULL)
    {
        *outRecord = Record(table, slot);
    }
    return true;
}

uint64_t SosTableFold(const SosTable *table,
                      uint64_t (*hashRecord)(SosTableKey key, const void *record, void *context),
                      void *context)
{
    if (table == NULL || hashRecord == NULL)
    {
        return 0u;
    }
    uint64_t sum = 0u;
    for (uint32_t slot = 0u; slot < table->capacity; ++slot)
    {
        if (table->used[slot] != 0u)
        {
            sum += hashRecord(table->keys[slot], Record(table, slot), context);
        }
    }
    return sum;
}
