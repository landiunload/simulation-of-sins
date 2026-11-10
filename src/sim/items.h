#ifndef SOS_SIM_ITEMS_H
#define SOS_SIM_ITEMS_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Material culture: the things people carry, make and trade.
 *
 * A kind is an open-ended 32-bit id. The catalog below curates the materials
 * and tools of the first cultures; every other nonzero id still has
 * properties, derived from the id itself, so kinds multiply without a registry
 * and without storing them. A stack remembers only what changed when it was
 * made, and freshness follows from the elapsed time, so food spoils lazily and
 * nothing is touched while the world runs. */

/* Slots one inventory holds: what fits two hands and a basket. */
#define SOS_INVENTORY_SLOTS 8u
/* Piles whose qualities fall in the same eighth of the scale stack together. */
#define SOS_INVENTORY_QUALITY_BUCKETS 8u
/* Freshness below which food is spoiled: three half-lives leave an eighth of
 * the original, less than a meal worth carrying. Fresher food is still edible,
 * so spoilage happens at three half-lives rather than when nothing is left. */
#define SOS_ITEM_SPOIL_FRESHNESS (SOS_Q16_ONE / 8)

typedef enum SosItemKindId
{
    SOS_ITEM_NONE = 0,
    SOS_ITEM_BERRY,
    SOS_ITEM_MEAT,
    SOS_ITEM_FISH,
    SOS_ITEM_HIDE,
    SOS_ITEM_FLINT,
    SOS_ITEM_STONE_TOOL,
    SOS_ITEM_SPEAR,
    SOS_ITEM_WOOD,
    SOS_ITEM_FIREWOOD,
    SOS_ITEM_CLAY_POT,
    SOS_ITEM_CORD,
    SOS_ITEM_BASKET,
    SOS_ITEM_CLOTHING,
    /* Ids below this are curated; ids at or above it are derived from the id. */
    SOS_ITEM_BUILTIN_COUNT,
} SosItemKindId;

/* What one kind is worth carrying. */
typedef struct SosItemKind
{
    uint32_t kind;           /* The id, echoed back. */
    int32_t massGrams;       /* Mass of one item in grams; at least one. */
    uint64_t perishHalfLife; /* Ticks for half the freshness to fade; 0 keeps. */
    uint32_t stackLimit;     /* Most of the kind in one slot; at least one. */
    int32_t edibleEnergy;    /* Nourishment when eaten; 0 is inedible. */
    bool tool;               /* Whether the kind is an implement. */
} SosItemKind;

/* One pile of identical items; an empty slot has kind 0. */
typedef struct SosInventorySlot
{
    uint32_t kind;     /* 0 when the slot is free. */
    uint32_t count;    /* Items in the pile, never above the stack limit. */
    SosQ16 quality;    /* Making quality in [0, SOS_Q16_ONE]. */
    uint64_t madeTick; /* When the pile was made, for freshness. */
} SosInventorySlot;

/* What somebody carries. Plain data, so a being, a chest and a trade offer all
 * hold it the same way and it copies, compares and hashes by value. */
typedef struct SosInventory
{
    SosInventorySlot slots[SOS_INVENTORY_SLOTS];
} SosInventory;

/* Writes the properties of `kind` and returns true. False for kind 0 or a NULL
 * `out`. Every nonzero kind is known: the catalog, or a durable object derived
 * from the id. */
bool SosItemsKind(uint32_t kind, SosItemKind *out);

/* Puts up to `count` items into the first pile of the same kind and quality
 * bucket, or a free slot, and returns how many fit. `quality` is clamped to
 * [0, SOS_Q16_ONE]; `tick` is the making time of a new pile, while stacking
 * keeps the pile's existing time. `carryLimitGrams` caps the inventory's total
 * mass and a negative value means no cap. A NULL inventory, kind 0 or count 0
 * fits nothing. */
uint32_t SosInventoryAdd(SosInventory *inventory, uint32_t kind, uint32_t count, SosQ16 quality,
                         uint64_t tick, int32_t carryLimitGrams);

/* Takes up to `count` items of `kind`, poorest quality first, and returns how
 * many were removed. A NULL inventory, kind 0 or count 0 removes nothing. */
uint32_t SosInventoryRemove(SosInventory *inventory, uint32_t kind, uint32_t count);

/* Items of `kind` in the inventory; kind 0 and a NULL inventory count zero. */
uint32_t SosInventoryCount(const SosInventory *inventory, uint32_t kind);

/* Total mass in grams, capped at INT32_MAX; a NULL inventory mass zero. */
int32_t SosInventoryMass(const SosInventory *inventory);

/* Drops every food whose freshness at `tick` has fallen below
 * SOS_ITEM_SPOIL_FRESHNESS and returns how many items were dropped. Kinds that
 * keep never spoil, and a NULL inventory drops nothing. */
uint32_t SosInventorySpoil(SosInventory *inventory, uint64_t tick);

/* How much of the pile in `slot` is still good at `tick`: SOS_Q16_ONE when it
 * was just made, halving every `perishHalfLife` ticks after that, so it is the
 * fraction SosDecay leaves of a full pile. Kinds that keep stay at
 * SOS_Q16_ONE; an empty slot, a slot index outside the inventory or a NULL
 * inventory has freshness 0. */
SosQ16 SosInventoryFreshness(const SosInventory *inventory, uint32_t slot, uint64_t tick);

/* Moves up to `count` items of `kind` from `from` to `to`, keeping each pile's
 * quality and making time no later than `tick`, and returns how many moved.
 * Items that do not fit stay in `from`, so the two together always hold the
 * same items. A NULL inventory, kind 0, count 0, or `from` equal to `to` moves
 * nothing. */
uint32_t SosInventoryTransfer(SosInventory *from, SosInventory *to, uint32_t kind, uint32_t count,
                              uint64_t tick, int32_t carryLimitGrams);

/* Folds the contents into one value, the same however the piles are ordered in
 * the slots, so replays and machines can be compared. A NULL inventory hashes
 * to zero. */
uint64_t SosInventoryHash(const SosInventory *inventory);

#endif
