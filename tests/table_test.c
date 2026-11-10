#include "sim/random.h"
#include "sim/table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "table test failed: %s\n", message);
        exit(1);
    }
}

typedef struct Record
{
    uint64_t value;
    uint32_t touched;
} Record;

static SosTableKey Key(uint64_t index)
{
    /* Pairs that share halves, as pairs of beings do. */
    return (SosTableKey){.first = index % 97u, .second = index / 97u};
}

static void TestBasics(void)
{
    SosTable *table = SosTableCreate(sizeof(Record));
    Expect(table != NULL, "create");
    Expect(SosTableFind(table, Key(1u)) == NULL, "empty table finds nothing");
    bool created = false;
    Record *record = SosTableInsert(table, Key(1u), &created);
    Expect(record != NULL && created, "insert creates");
    Expect(record->value == 0u && record->touched == 0u, "new records are zero");
    record->value = 42u;
    Expect(SosTableInsert(table, Key(1u), &created) == record && !created, "insert finds");
    const Record *found = SosTableFind(table, Key(1u));
    Expect(found != NULL && found->value == 42u, "find");
    Expect(SosTableCount(table) == 1u, "count");
    Expect(SosTableRemove(table, Key(1u)) && !SosTableRemove(table, Key(1u)), "remove once");
    Expect(SosTableCount(table) == 0u && SosTableFind(table, Key(1u)) == NULL, "removed");
    SosTableDestroy(table);
}

static void TestAgainstModel(void)
{
    /* Random inserts and removals checked against a plain array. */
    enum
    {
        KEYS = 4096,
        OPERATIONS = 200000,
    };
    static bool present[KEYS];
    static uint64_t values[KEYS];
    SosTable *table = SosTableCreate(sizeof(Record));
    SosRandom random = SosRandomStream(5u, 0u);
    uint32_t count = 0u;
    for (int operation = 0; operation < OPERATIONS; ++operation)
    {
        const uint32_t index = SosRandomBelow(&random, KEYS);
        if (SosRandomChance(&random, 3u, 5u))
        {
            Record *record = SosTableInsert(table, Key(index), NULL);
            Expect(record != NULL, "insert in the model");
            Expect(present[index] == (record->touched != 0u), "existing records are kept");
            if (!present[index])
            {
                present[index] = true;
                count += 1u;
            }
            record->touched = 1u;
            record->value = SosRandomNext(&random);
            values[index] = record->value;
        }
        else
        {
            Expect(SosTableRemove(table, Key(index)) == present[index], "remove in the model");
            if (present[index])
            {
                present[index] = false;
                count -= 1u;
            }
        }
    }
    Expect(SosTableCount(table) == count, "count follows the model");
    for (uint32_t index = 0u; index < KEYS; ++index)
    {
        const Record *record = SosTableFind(table, Key(index));
        Expect((record != NULL) == present[index], "presence follows the model");
        Expect(record == NULL || record->value == values[index], "values follow the model");
    }
    SosTableDestroy(table);
}

static bool DropOdd(SosTableKey key, const void *record, void *context)
{
    (void)record;
    *(uint32_t *)context += 1u;
    return ((key.first + key.second * 97u) & 1u) != 0u;
}

static void TestRemoveIf(void)
{
    SosTable *table = SosTableCreate(sizeof(Record));
    for (uint64_t index = 0u; index < 10000u; ++index)
    {
        Record *record = SosTableInsert(table, Key(index), NULL);
        Expect(record != NULL, "fill");
        record->value = index;
    }
    uint32_t visits = 0u;
    Expect(SosTableRemoveIf(table, DropOdd, &visits) == 5000u, "remove odd records");
    Expect(visits >= 10000u, "every record is offered");
    Expect(SosTableCount(table) == 5000u, "half remain");
    for (uint64_t index = 0u; index < 10000u; ++index)
    {
        const Record *record = SosTableFind(table, Key(index));
        Expect((record != NULL) == ((index & 1u) == 0u), "the even records remain");
        Expect(record == NULL || record->value == index, "remaining records are intact");
    }

    uint32_t occupied = 0u;
    for (uint32_t slot = 0u; slot < SosTableSlotCount(table); ++slot)
    {
        SosTableKey key;
        const void *record = NULL;
        if (SosTableSlot(table, slot, &key, &record))
        {
            occupied += 1u;
            Expect(SosTableFind(table, key) == record, "slots match lookups");
        }
    }
    Expect(occupied == 5000u, "visiting sees every record");
    Expect(!SosTableSlot(table, SosTableSlotCount(table), NULL, NULL), "past the end");

    SosTableClear(table);
    Expect(SosTableCount(table) == 0u && SosTableFind(table, Key(2u)) == NULL, "clear");
    SosTableDestroy(table);
}

static uint64_t HashRecord(SosTableKey key, const void *record, void *context)
{
    (void)context;
    return SosHashCombine(SosHashCombine(key.first, key.second), ((const Record *)record)->value);
}

static void TestFold(void)
{
    /* The same records inserted in opposite orders fold to the same value. */
    SosTable *forward = SosTableCreate(sizeof(Record));
    SosTable *backward = SosTableCreate(sizeof(Record));
    for (uint64_t index = 0u; index < 1000u; ++index)
    {
        Record *first = SosTableInsert(forward, Key(index), NULL);
        Record *second = SosTableInsert(backward, Key(999u - index), NULL);
        Expect(first != NULL && second != NULL, "fill both");
        first->value = index;
        second->value = 999u - index;
    }
    const uint64_t fold = SosTableFold(forward, HashRecord, NULL);
    Expect(fold == SosTableFold(backward, HashRecord, NULL), "folding ignores order");
    ((Record *)SosTableEdit(backward, Key(5u)))->value = 6u;
    Expect(fold != SosTableFold(backward, HashRecord, NULL), "folding sees changes");
    Expect(SosTableFold(NULL, HashRecord, NULL) == 0u, "a NULL table folds to zero");
    SosTableDestroy(forward);
    SosTableDestroy(backward);
}

static void TestSetsAndLimits(void)
{
    SosTable *set = SosTableCreate(0u);
    Expect(set != NULL, "a set");
    Expect(SosTableInsert(set, Key(3u), NULL) != NULL, "a set inserts");
    Expect(SosTableFind(set, Key(3u)) != NULL && SosTableFind(set, Key(4u)) == NULL,
           "a set tells membership");
    Expect(SosTableReserve(set, 1000u) && SosTableFind(set, Key(3u)) != NULL,
           "reserving keeps members");
    SosTableDestroy(set);

    Expect(SosTableCreate(SOS_TABLE_MAX_RECORD_SIZE + 1u) == NULL, "records have a limit");
    Expect(SosTableCount(NULL) == 0u && SosTableFind(NULL, Key(0u)) == NULL, "NULL tables");
    Expect(SosTableInsert(NULL, Key(0u), NULL) == NULL && !SosTableRemove(NULL, Key(0u)),
           "NULL tables change nothing");
    Expect(SosTableRemoveIf(NULL, DropOdd, NULL) == 0u, "NULL tables remove nothing");
    Expect(!SosTableReserve(NULL, 1u), "NULL tables reserve nothing");
    SosTableDestroy(NULL);
}

int main(void)
{
    TestBasics();
    TestAgainstModel();
    TestRemoveIf();
    TestFold();
    TestSetsAndLimits();
    return 0;
}
