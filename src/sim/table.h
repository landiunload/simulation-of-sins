#ifndef SOS_SIM_TABLE_H
#define SOS_SIM_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A sparse table of records.
 *
 * The unbounded world is a function of its seed, and everything beings change
 * in it is a sparse layer over that function: a harvested bush, an opinion of
 * a neighbour, a learnt skill. This table holds such a layer. A key is a pair
 * of 64-bit values, so a being, a pair of beings or a being and a skill is one
 * key; a record is a fixed-size block of plain data, zero-filled when it is
 * created.
 *
 * Open addressing with linear probing over a power-of-two array at most half
 * full; removal shifts the following records back, so no tombstones pile up.
 * Records move when the table changes: a pointer into it lasts until the next
 * insert or removal. The slot order depends on the history of inserts, so
 * whatever must agree across machines either visits records in key order or
 * folds them order-independently. */

typedef struct SosTableKey
{
    uint64_t first;
    uint64_t second;
} SosTableKey;

typedef struct SosTable SosTable;

/* Largest record a table holds, in bytes. */
#define SOS_TABLE_MAX_RECORD_SIZE 4096u
/* Most records a table holds. */
#define SOS_TABLE_MAX_COUNT (UINT32_C(1) << 30)

/* A table of `recordSize`-byte records, or NULL when the size exceeds
 * SOS_TABLE_MAX_RECORD_SIZE or memory ran out. A zero size makes a set of
 * keys. Nothing is allocated for records until the first insert. */
SosTable *SosTableCreate(size_t recordSize);
void SosTableDestroy(SosTable *table);

uint32_t SosTableCount(const SosTable *table);

/* The record of `key`, or NULL when there is none. For a set, any non-NULL
 * pointer means present and must not be dereferenced. */
const void *SosTableFind(const SosTable *table, SosTableKey key);
void *SosTableEdit(SosTable *table, SosTableKey key);

/* The record of `key`, created zero-filled when missing; `outCreated` (may be
 * NULL) tells which. NULL when memory ran out or the table is full, leaving
 * the table unchanged. */
void *SosTableInsert(SosTable *table, SosTableKey key, bool *outCreated);

/* False when there was no record. Never allocates. */
bool SosTableRemove(SosTable *table, SosTableKey key);

/* Removes every record for which `drop` returns true, for example records
 * that decayed to nothing, and returns how many. The callback must not change
 * the table. Never allocates. */
uint32_t SosTableRemoveIf(SosTable *table,
                          bool (*drop)(SosTableKey key, const void *record, void *context),
                          void *context);

void SosTableClear(SosTable *table);

/* Makes room for `count` records, so inserts up to it cannot run out of
 * memory. False when memory ran out, leaving the table unchanged. */
bool SosTableReserve(SosTable *table, uint32_t count);

/* Visiting: slots are numbered [0, SosTableSlotCount); an empty slot returns
 * false. The order is not stable across inserts and removals. */
uint32_t SosTableSlotCount(const SosTable *table);
bool SosTableSlot(const SosTable *table, uint32_t slot, SosTableKey *outKey,
                  const void **outRecord);

/* Folds every record into one value that does not depend on slot order: the
 * sum of `hashRecord` over the records, so equal sets of records give equal
 * values however they were inserted. What a world hash is built from. */
uint64_t SosTableFold(const SosTable *table,
                      uint64_t (*hashRecord)(SosTableKey key, const void *record, void *context),
                      void *context);

#endif
