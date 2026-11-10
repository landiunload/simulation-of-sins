#include "sim/items.h"

#include "sim/calendar.h"
#include "sim/random.h"

#include <stddef.h>

/* Three bits make the eight SOS_INVENTORY_QUALITY_BUCKETS buckets. */
#define QUALITY_BUCKET_BITS 3u
/* A kind nobody curated is a durable object of modest size: one gram to five
 * kilograms, one to sixty-four to a slot. */
#define DERIVED_MASS_MAX_GRAMS 5000u
#define DERIVED_STACK_MAX 64u

static const SosItemKind catalog[SOS_ITEM_BUILTIN_COUNT] = {
    [SOS_ITEM_NONE] = {.kind = SOS_ITEM_NONE},
    /* Foods: light while fresh, rich in energy, and they rot. */
    [SOS_ITEM_BERRY] = {.kind = SOS_ITEM_BERRY,
                        .massGrams = 4,
                        .perishHalfLife = (uint64_t)(2 * SOS_TICKS_PER_DAY),
                        .stackLimit = 100u,
                        .edibleEnergy = 6,
                        .tool = false},
    [SOS_ITEM_MEAT] = {.kind = SOS_ITEM_MEAT,
                       .massGrams = 300,
                       .perishHalfLife = (uint64_t)SOS_TICKS_PER_DAY,
                       .stackLimit = 20u,
                       .edibleEnergy = 90,
                       .tool = false},
    [SOS_ITEM_FISH] = {.kind = SOS_ITEM_FISH,
                       .massGrams = 250,
                       .perishHalfLife = (uint64_t)SOS_TICKS_PER_DAY,
                       .stackLimit = 20u,
                       .edibleEnergy = 70,
                       .tool = false},
    /* Raw materials: they keep and are not food. */
    [SOS_ITEM_HIDE] = {.kind = SOS_ITEM_HIDE, .massGrams = 800, .stackLimit = 8u},
    [SOS_ITEM_FLINT] = {.kind = SOS_ITEM_FLINT, .massGrams = 150, .stackLimit = 40u},
    [SOS_ITEM_WOOD] = {.kind = SOS_ITEM_WOOD, .massGrams = 1500, .stackLimit = 20u},
    [SOS_ITEM_FIREWOOD] = {.kind = SOS_ITEM_FIREWOOD, .massGrams = 1200, .stackLimit = 30u},
    [SOS_ITEM_CORD] = {.kind = SOS_ITEM_CORD, .massGrams = 40, .stackLimit = 100u},
    /* Implements: heavy, few to a slot, and marked as tools. */
    [SOS_ITEM_STONE_TOOL] = {.kind = SOS_ITEM_STONE_TOOL,
                             .massGrams = 900,
                             .stackLimit = 4u,
                             .tool = true},
    [SOS_ITEM_SPEAR] = {.kind = SOS_ITEM_SPEAR, .massGrams = 1100, .stackLimit = 4u, .tool = true},
    [SOS_ITEM_CLAY_POT] = {.kind = SOS_ITEM_CLAY_POT,
                           .massGrams = 600,
                           .stackLimit = 8u,
                           .tool = true},
    [SOS_ITEM_BASKET] = {.kind = SOS_ITEM_BASKET, .massGrams = 500, .stackLimit = 8u, .tool = true},
    [SOS_ITEM_CLOTHING] = {.kind = SOS_ITEM_CLOTHING, .massGrams = 350, .stackLimit = 6u},
};

/* A kind nobody curated: a durable object whose mass and stack limit follow
 * from the id, so the catalog is unbounded without a table. */
static SosItemKind Derive(uint32_t kind)
{
    const uint64_t hash = SosHash64(kind);
    return (SosItemKind){
        .kind = kind,
        .massGrams = (int32_t)(1u + hash % DERIVED_MASS_MAX_GRAMS),
        .perishHalfLife = 0u,
        .stackLimit = 1u + (uint32_t)((hash >> 32u) % DERIVED_STACK_MAX),
        .edibleEnergy = 0,
        .tool = false,
    };
}

bool SosItemsKind(uint32_t kind, SosItemKind *out)
{
    if (out == NULL || kind == 0u)
    {
        return false;
    }
    *out = kind < SOS_ITEM_BUILTIN_COUNT ? catalog[kind] : Derive(kind);
    return true;
}

static uint32_t QualityBucket(SosQ16 quality)
{
    const uint32_t bucket = (uint32_t)quality >> (SOS_Q16_BITS - QUALITY_BUCKET_BITS);
    return bucket < SOS_INVENTORY_QUALITY_BUCKETS ? bucket : SOS_INVENTORY_QUALITY_BUCKETS - 1u;
}

/* Mass of everything the inventory holds, before the INT32 clamp of the public
 * call, so the carry limit is not fooled by a very heavy load. */
static int64_t TotalMass(const SosInventory *inventory)
{
    int64_t total = 0;
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS; ++index)
    {
        const SosInventorySlot *slot = &inventory->slots[index];
        if (slot->kind == 0u || slot->count == 0u)
        {
            continue;
        }
        SosItemKind item;
        if (SosItemsKind(slot->kind, &item) && item.massGrams > 0)
        {
            total += (int64_t)item.massGrams * slot->count;
        }
    }
    return total;
}

/* Most of `wanted` that fits in `remainingMass` grams. */
static uint32_t FitByMass(int64_t remainingMass, int32_t itemMass, uint32_t wanted)
{
    if (itemMass <= 0)
    {
        return wanted;
    }
    const int64_t fits = remainingMass / itemMass;
    if (fits <= 0)
    {
        return 0u;
    }
    return fits >= (int64_t)wanted ? wanted : (uint32_t)fits;
}

/* The one place items enter an inventory, used by adding and by transfer. It
 * takes the making time directly so a gift keeps the age of what was given. */
static uint32_t Insert(SosInventory *inventory, uint32_t kind, uint32_t count, SosQ16 quality,
                       uint64_t madeTick, int32_t carryLimitGrams)
{
    SosItemKind item;
    if (inventory == NULL || count == 0u || !SosItemsKind(kind, &item))
    {
        return 0u;
    }
    const uint32_t bucket = QualityBucket(quality);
    int64_t remainingMass =
        carryLimitGrams < 0 ? INT64_MAX : (int64_t)carryLimitGrams - TotalMass(inventory);
    if (remainingMass < 0)
    {
        remainingMass = 0;
    }
    uint32_t left = count;
    uint32_t added = 0u;
    /* Fill matching piles first, so one more berry does not spend a slot. */
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS && left > 0u; ++index)
    {
        SosInventorySlot *slot = &inventory->slots[index];
        if (slot->kind != kind || QualityBucket(slot->quality) != bucket ||
            slot->count >= item.stackLimit)
        {
            continue;
        }
        uint32_t amount = item.stackLimit - slot->count;
        amount = amount < left ? amount : left;
        amount = FitByMass(remainingMass, item.massGrams, amount);
        slot->count += amount;
        remainingMass -= (int64_t)amount * item.massGrams;
        left -= amount;
        added += amount;
    }
    /* Then open new piles in free slots. */
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS && left > 0u; ++index)
    {
        SosInventorySlot *slot = &inventory->slots[index];
        if (slot->kind != 0u)
        {
            continue;
        }
        uint32_t amount = item.stackLimit < left ? item.stackLimit : left;
        amount = FitByMass(remainingMass, item.massGrams, amount);
        if (amount == 0u)
        {
            break;
        }
        *slot = (SosInventorySlot){
            .kind = kind, .count = amount, .quality = quality, .madeTick = madeTick};
        remainingMass -= (int64_t)amount * item.massGrams;
        left -= amount;
        added += amount;
    }
    return added;
}

uint32_t SosInventoryAdd(SosInventory *inventory, uint32_t kind, uint32_t count, SosQ16 quality,
                         uint64_t tick, int32_t carryLimitGrams)
{
    if (inventory == NULL)
    {
        return 0u;
    }
    return Insert(inventory, kind, count, SosQ16Clamp(quality, 0, SOS_Q16_ONE), tick,
                  carryLimitGrams);
}

uint32_t SosInventoryRemove(SosInventory *inventory, uint32_t kind, uint32_t count)
{
    if (inventory == NULL || kind == 0u || count == 0u)
    {
        return 0u;
    }
    uint32_t left = count;
    uint32_t removed = 0u;
    while (left > 0u)
    {
        /* The poorest pile first: good items are worth keeping. */
        uint32_t chosen = SOS_INVENTORY_SLOTS;
        uint32_t chosenBucket = 0u;
        for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS; ++index)
        {
            const SosInventorySlot *slot = &inventory->slots[index];
            if (slot->kind != kind || slot->count == 0u)
            {
                continue;
            }
            const uint32_t bucket = QualityBucket(slot->quality);
            if (chosen == SOS_INVENTORY_SLOTS || bucket < chosenBucket)
            {
                chosen = index;
                chosenBucket = bucket;
            }
        }
        if (chosen == SOS_INVENTORY_SLOTS)
        {
            break;
        }
        SosInventorySlot *slot = &inventory->slots[chosen];
        const uint32_t amount = slot->count < left ? slot->count : left;
        slot->count -= amount;
        if (slot->count == 0u)
        {
            *slot = (SosInventorySlot){0};
        }
        left -= amount;
        removed += amount;
    }
    return removed;
}

uint32_t SosInventoryCount(const SosInventory *inventory, uint32_t kind)
{
    if (inventory == NULL || kind == 0u)
    {
        return 0u;
    }
    uint64_t total = 0u;
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS; ++index)
    {
        if (inventory->slots[index].kind == kind)
        {
            total += inventory->slots[index].count;
        }
    }
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

int32_t SosInventoryMass(const SosInventory *inventory)
{
    if (inventory == NULL)
    {
        return 0;
    }
    const int64_t total = TotalMass(inventory);
    if (total <= 0)
    {
        return 0;
    }
    return total > INT32_MAX ? INT32_MAX : (int32_t)total;
}

SosQ16 SosInventoryFreshness(const SosInventory *inventory, uint32_t slot, uint64_t tick)
{
    if (inventory == NULL || slot >= SOS_INVENTORY_SLOTS)
    {
        return 0;
    }
    const SosInventorySlot *entry = &inventory->slots[slot];
    if (entry->kind == 0u || entry->count == 0u)
    {
        return 0;
    }
    SosItemKind item;
    if (!SosItemsKind(entry->kind, &item) || item.perishHalfLife == 0u)
    {
        /* A kind that keeps is always whole, however old it is. */
        return SOS_Q16_ONE;
    }
    return (SosQ16)SosDecaySince(SOS_Q16_ONE, entry->madeTick, tick, item.perishHalfLife);
}

uint32_t SosInventorySpoil(SosInventory *inventory, uint64_t tick)
{
    if (inventory == NULL)
    {
        return 0u;
    }
    uint32_t spoiled = 0u;
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS; ++index)
    {
        SosInventorySlot *slot = &inventory->slots[index];
        if (slot->kind == 0u || slot->count == 0u)
        {
            continue;
        }
        if (SosInventoryFreshness(inventory, index, tick) < SOS_ITEM_SPOIL_FRESHNESS)
        {
            spoiled += slot->count;
            *slot = (SosInventorySlot){0};
        }
    }
    return spoiled;
}

uint32_t SosInventoryTransfer(SosInventory *from, SosInventory *to, uint32_t kind, uint32_t count,
                              uint64_t tick, int32_t carryLimitGrams)
{
    if (from == NULL || to == NULL || from == to || kind == 0u || count == 0u)
    {
        return 0u;
    }
    uint32_t left = count;
    uint32_t moved = 0u;
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS && left > 0u; ++index)
    {
        SosInventorySlot *slot = &from->slots[index];
        if (slot->kind != kind || slot->count == 0u)
        {
            continue;
        }
        const uint32_t amount = slot->count < left ? slot->count : left;
        /* No item can have been made after now. */
        const uint64_t madeTick = slot->madeTick < tick ? slot->madeTick : tick;
        const uint32_t added = Insert(to, kind, amount, slot->quality, madeTick, carryLimitGrams);
        slot->count -= added;
        if (slot->count == 0u)
        {
            *slot = (SosInventorySlot){0};
        }
        left -= added;
        moved += added;
        if (added < amount)
        {
            /* `to` cannot take more, so no later pile can either. */
            break;
        }
    }
    return moved;
}

uint64_t SosInventoryHash(const SosInventory *inventory)
{
    if (inventory == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on which slot holds which pile. */
    uint64_t sum = 0u;
    for (uint32_t index = 0u; index < SOS_INVENTORY_SLOTS; ++index)
    {
        const SosInventorySlot *slot = &inventory->slots[index];
        if (slot->kind == 0u)
        {
            continue;
        }
        uint64_t value = SosHashCombine((uint64_t)slot->kind, (uint64_t)slot->count);
        value = SosHashCombine(value, (uint64_t)(uint32_t)slot->quality);
        value = SosHashCombine(value, slot->madeTick);
        sum += SosHash64(value);
    }
    return sum;
}
