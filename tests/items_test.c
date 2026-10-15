#include "sim/calendar.h"
#include "sim/items.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DAY SOS_TICKS_PER_DAY

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "items test failed: %s\n", message);
        exit(1);
    }
}

static void TestCatalog(void)
{
    SosItemKind berry;
    Expect(SosItemsKind(SOS_ITEM_BERRY, &berry), "berries are known");
    Expect(berry.kind == SOS_ITEM_BERRY && berry.massGrams > 0 && berry.stackLimit > 0,
           "a berry has mass and a stack limit");
    Expect(berry.perishHalfLife > 0u && berry.edibleEnergy > 0 && !berry.tool,
           "a berry is food and not a tool");

    SosItemKind tool;
    Expect(SosItemsKind(SOS_ITEM_STONE_TOOL, &tool), "stone tools are known");
    Expect(tool.tool && tool.perishHalfLife == 0u && tool.edibleEnergy == 0,
           "a stone tool is a durable implement");

    Expect(!SosItemsKind(0u, &berry) && !SosItemsKind(SOS_ITEM_BERRY, NULL),
           "no kind and no output are rejected");

    SosItemKind derived;
    Expect(SosItemsKind(100000u, &derived), "an uncurated kind is known");
    Expect(derived.kind == 100000u && derived.massGrams > 0 && derived.stackLimit > 0,
           "an uncurated kind still has properties");
    SosItemKind again;
    Expect(SosItemsKind(100000u, &again) && again.massGrams == derived.massGrams &&
               again.stackLimit == derived.stackLimit,
           "derived properties are a function of the id");
}

static void TestStacking(void)
{
    SosInventory inventory = {0};
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 10u, SOS_Q16_ONE / 2, 100u, -1) == 10u,
           "berries are added");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_BERRY) == 10u, "berries are counted");
    Expect(inventory.slots[0].kind == SOS_ITEM_BERRY && inventory.slots[0].count == 10u &&
               inventory.slots[0].quality == SOS_Q16_ONE / 2 && inventory.slots[0].madeTick == 100u,
           "the first pile keeps kind, count, quality and time");

    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 5u, SOS_Q16_ONE / 2, 200u, -1) == 5u,
           "a matching pile stacks");
    Expect(inventory.slots[0].count == 15u && inventory.slots[1].kind == 0u,
           "stacking fills the existing pile before a new slot");
    Expect(inventory.slots[0].madeTick == 100u, "stacking keeps the pile's older time");

    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 3u, SOS_Q16_ONE - 1, 200u, -1) == 3u,
           "another quality bucket is still berries");
    Expect(inventory.slots[1].kind == SOS_ITEM_BERRY && inventory.slots[1].count == 3u,
           "another quality bucket gets its own pile");

    Expect(SosInventoryAdd(&inventory, SOS_ITEM_MEAT, 4u, SOS_Q16_ONE, 200u, -1) == 4u,
           "another kind is added");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_BERRY) == 18u &&
               SosInventoryCount(&inventory, SOS_ITEM_MEAT) == 4u,
           "counts add up per kind");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_WOOD) == 0u, "an absent kind counts zero");
}

static void TestLimits(void)
{
    SosInventory inventory = {0};
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 150u, SOS_Q16_ONE, 0u, -1) == 150u,
           "a big pile is added");
    Expect(inventory.slots[0].count == 100u && inventory.slots[1].count == 50u,
           "a pile is capped at the stack limit");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_BERRY) == 150u, "the whole pile is stored");

    SosInventory tools = {0};
    Expect(SosInventoryAdd(&tools, SOS_ITEM_STONE_TOOL, 100u, SOS_Q16_ONE, 0u, -1) == 32u,
           "only the slots hold the tools");
    Expect(SosInventoryCount(&tools, SOS_ITEM_STONE_TOOL) == 32u, "no tool is lost");

    SosInventory limited = {0};
    Expect(SosInventoryAdd(&limited, SOS_ITEM_WOOD, 10u, SOS_Q16_ONE, 0u, 4000) == 2u,
           "the carry limit caps the count");
    Expect(SosInventoryMass(&limited) == 3000, "the mass stays within the limit");
    Expect(SosInventoryAdd(&limited, SOS_ITEM_WOOD, 10u, SOS_Q16_ONE, 0u, 3000) == 0u,
           "a full carry limit takes nothing");
    Expect(SosInventoryAdd(&limited, SOS_ITEM_FLINT, 1u, SOS_Q16_ONE, 0u, 3200) == 1u,
           "a lighter item still fits");
    Expect(SosInventoryAdd(&limited, SOS_ITEM_WOOD, 1u, SOS_Q16_ONE, 0u, -1) == 1u,
           "a negative limit is unlimited");
}

static void TestRemove(void)
{
    SosInventory inventory = {0};
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 30u, SOS_Q16_ONE - 1, 0u, -1) == 30u,
           "good berries are added");
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 20u, SOS_Q16_ONE / 8, 0u, -1) == 20u,
           "poor berries are added");

    Expect(SosInventoryRemove(&inventory, SOS_ITEM_BERRY, 10u) == 10u, "berries are removed");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_BERRY) == 40u, "the removed berries are gone");
    Expect(inventory.slots[0].count == 30u && inventory.slots[1].count == 10u,
           "the poorest pile is used first");

    Expect(SosInventoryRemove(&inventory, SOS_ITEM_BERRY, 100u) == 40u,
           "removal stops at what is there");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_BERRY) == 0u, "an emptied inventory counts zero");
    Expect(SosInventoryRemove(&inventory, SOS_ITEM_BERRY, 1u) == 0u,
           "nothing left removes nothing");
    Expect(SosInventoryRemove(&inventory, SOS_ITEM_WOOD, 1u) == 0u &&
               SosInventoryRemove(NULL, 1u, 1u) == 0u,
           "absent kinds and no inventory remove nothing");
}

static void TestSpoil(void)
{
    SosInventory inventory = {0};
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_MEAT, 5u, SOS_Q16_ONE, 0u, -1) == 5u,
           "meat is added");
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_STONE_TOOL, 2u, SOS_Q16_ONE, 0u, -1) == 2u,
           "tools are added");

    Expect(SosInventorySpoil(&inventory, 0u) == 0u, "fresh food does not spoil");
    Expect(SosInventorySpoil(&inventory, (uint64_t)DAY) == 0u, "one half-life leaves food fit");
    Expect(SosInventorySpoil(&inventory, (uint64_t)(3 * DAY)) == 0u,
           "food is edible down to the spoil threshold");
    Expect(SosInventorySpoil(&inventory, (uint64_t)(4 * DAY)) == 5u,
           "food past the spoil threshold is dropped");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_MEAT) == 0u, "the spoiled pile is gone");
    Expect(SosInventoryCount(&inventory, SOS_ITEM_STONE_TOOL) == 2u, "tools never spoil");
    Expect(SosInventorySpoil(&inventory, UINT64_MAX) == 0u, "nothing more spoils");
    Expect(SosInventorySpoil(NULL, 100u) == 0u, "no inventory spoils nothing");
}

static void TestFreshness(void)
{
    SosInventory inventory = {0};
    Expect(SosInventoryAdd(&inventory, SOS_ITEM_MEAT, 5u, SOS_Q16_ONE, 100u, -1) == 5u,
           "meat is added");
    Expect(SosInventoryFreshness(&inventory, 0u, 100u) == SOS_Q16_ONE, "fresh meat is whole");
    Expect(SosInventoryFreshness(&inventory, 0u, 100u + (uint64_t)DAY) == SOS_Q16_ONE / 2,
           "one half-life halves freshness");
    Expect(SosInventoryFreshness(&inventory, 0u, 100u + (uint64_t)(2 * DAY)) == SOS_Q16_ONE / 4,
           "two half-lives leave a quarter");
    Expect(SosInventoryFreshness(&inventory, 0u, 100u + (uint64_t)(3 * DAY)) ==
               SOS_ITEM_SPOIL_FRESHNESS,
           "three half-lives reach the spoil threshold exactly");
    Expect(SosInventoryFreshness(&inventory, 0u, 100u + (uint64_t)(4 * DAY)) <
               SOS_ITEM_SPOIL_FRESHNESS,
           "four half-lives are below the spoil threshold");
    Expect(SosInventoryFreshness(&inventory, 0u, 0u) == SOS_Q16_ONE,
           "a making time after the tick is still fresh");

    Expect(SosInventoryAdd(&inventory, SOS_ITEM_STONE_TOOL, 1u, SOS_Q16_ONE, 100u, -1) == 1u,
           "a tool is added");
    Expect(SosInventoryFreshness(&inventory, 1u, UINT64_MAX) == SOS_Q16_ONE,
           "a kind that keeps never fades");

    Expect(SosInventoryFreshness(&inventory, 2u, 100u) == 0, "an empty slot has no freshness");
    Expect(SosInventoryFreshness(&inventory, SOS_INVENTORY_SLOTS, 100u) == 0,
           "a slot outside the inventory has no freshness");
    Expect(SosInventoryFreshness(NULL, 0u, 100u) == 0, "no inventory has no freshness");
}

static void TestTransfer(void)
{
    SosInventory from = {0};
    SosInventory to = {0};
    Expect(SosInventoryAdd(&from, SOS_ITEM_BERRY, 20u, SOS_Q16_ONE / 2, 0u, -1) == 20u,
           "a pile to give");
    Expect(SosInventoryTransfer(&from, &to, SOS_ITEM_BERRY, 8u, 5000u, -1) == 8u,
           "berries are given");
    Expect(SosInventoryCount(&from, SOS_ITEM_BERRY) == 12u &&
               SosInventoryCount(&to, SOS_ITEM_BERRY) == 8u,
           "a transfer conserves the items");
    Expect(to.slots[0].quality == SOS_Q16_ONE / 2 && to.slots[0].madeTick == 0u,
           "a gift keeps its quality and making time");

    Expect(SosInventoryTransfer(&from, &to, SOS_ITEM_BERRY, 12u, 5000u, 0) == 0u,
           "a full receiver takes nothing");
    Expect(SosInventoryCount(&from, SOS_ITEM_BERRY) == 12u, "the giver keeps what does not fit");

    Expect(SosInventoryTransfer(&from, &to, SOS_ITEM_BERRY, 5u, 5000u, -1) == 5u,
           "the rest is given freely");
    Expect(SosInventoryCount(&from, SOS_ITEM_BERRY) == 7u &&
               SosInventoryCount(&to, SOS_ITEM_BERRY) == 13u,
           "the pair together still holds every item");

    Expect(SosInventoryTransfer(&from, &to, SOS_ITEM_WOOD, 1u, 5000u, -1) == 0u &&
               SosInventoryTransfer(&from, &from, SOS_ITEM_BERRY, 1u, 5000u, -1) == 0u &&
               SosInventoryTransfer(NULL, &to, SOS_ITEM_BERRY, 1u, 5000u, -1) == 0u,
           "empty and self transfers move nothing");
}

static void TestDeterminism(void)
{
    SosInventory first = {0};
    SosInventory second = {0};
    Expect(SosInventoryAdd(&first, SOS_ITEM_BERRY, 5u, SOS_Q16_ONE / 2, 10u, -1) == 5u &&
               SosInventoryAdd(&first, SOS_ITEM_WOOD, 3u, SOS_Q16_ONE, 10u, -1) == 3u,
           "one inventory is built");
    Expect(SosInventoryAdd(&second, SOS_ITEM_WOOD, 3u, SOS_Q16_ONE, 10u, -1) == 3u &&
               SosInventoryAdd(&second, SOS_ITEM_BERRY, 5u, SOS_Q16_ONE / 2, 10u, -1) == 5u,
           "the same contents in another order are built");

    Expect(SosInventoryHash(&first) == SosInventoryHash(&second),
           "the hash does not depend on slot order");
    const SosInventory copy = first;
    Expect(SosInventoryHash(&first) == SosInventoryHash(&copy), "equal inventories hash equally");
    Expect(SosInventoryHash(&first) != SosInventoryHash(&(SosInventory){0}),
           "an inventory differs from an empty one");
    Expect(SosInventoryHash(NULL) == 0u, "no inventory hashes to zero");
}

/* Fixed point of the contents. Every platform and compiler must fold this
 * inventory to exactly this value. */
static void TestGolden(void)
{
    SosInventory inventory = {0};
    (void)SosInventoryAdd(&inventory, SOS_ITEM_BERRY, 7u, SOS_Q16_ONE / 2, 1000u, -1);
    (void)SosInventoryAdd(&inventory, SOS_ITEM_MEAT, 3u, SOS_Q16_ONE, 1000u, -1);
    (void)SosInventoryAdd(&inventory, SOS_ITEM_STONE_TOOL, 2u, SOS_Q16_ONE / 4, 2000u, -1);

    const uint64_t hash = SosInventoryHash(&inventory);
    const uint64_t expected = UINT64_C(0x8316A2AA927E032C);
    if (hash != expected)
    {
        fprintf(stderr, "inventory hash: 0x%016" PRIX64 " mass %d\n", hash,
                SosInventoryMass(&inventory));
        Expect(0, "the inventory matches the reference");
    }
    Expect(inventory.slots[0].count == 7u &&
               SosInventoryMass(&inventory) == 7 * 4 + 3 * 300 + 2 * 900,
           "the golden inventory holds what was added");
}

int main(void)
{
    TestCatalog();
    TestStacking();
    TestLimits();
    TestRemove();
    TestSpoil();
    TestFreshness();
    TestTransfer();
    TestDeterminism();
    TestGolden();
    return 0;
}
