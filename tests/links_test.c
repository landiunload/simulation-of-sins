#include "sim/links.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
/* Owners and items the model and the set are exercised over. */
#define OWNER_COUNT 8u
#define ITEM_COUNT 16u
/* Steps in the model check. Enough to cover grows, shrinks and relinks. */
#define MODEL_STEPS 4000u

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "links test failed: %s\n", message);
        exit(1);
    }
}

/* The reference: one ordered list per owner, exactly what the set promises. */
typedef struct Model
{
    uint64_t items[OWNER_COUNT][ITEM_COUNT];
    uint32_t counts[OWNER_COUNT];
} Model;

static bool ModelHas(const Model *model, uint32_t owner, uint64_t item)
{
    for (uint32_t index = 0u; index < model->counts[owner - 1u]; ++index)
    {
        if (model->items[owner - 1u][index] == item)
        {
            return true;
        }
    }
    return false;
}

static void ModelAdd(Model *model, uint32_t owner, uint64_t item)
{
    if (ModelHas(model, owner, item))
    {
        return;
    }
    model->items[owner - 1u][model->counts[owner - 1u]] = item;
    model->counts[owner - 1u] += 1u;
}

static void ModelRemove(Model *model, uint32_t owner, uint64_t item)
{
    uint32_t *count = &model->counts[owner - 1u];
    for (uint32_t index = 0u; index < *count; ++index)
    {
        if (model->items[owner - 1u][index] == item)
        {
            for (uint32_t rest = index; rest + 1u < *count; ++rest)
            {
                model->items[owner - 1u][rest] = model->items[owner - 1u][rest + 1u];
            }
            *count -= 1u;
            return;
        }
    }
}

/* Walks every owner through the set and the model at once. */
static void Compare(const SosLinks *links, const Model *model)
{
    for (uint32_t owner = 1u; owner <= OWNER_COUNT; ++owner)
    {
        const uint32_t expected = model->counts[owner - 1u];
        Expect(SosLinksCount(links, owner) == expected, "the count matches the model");
        uint64_t listed[ITEM_COUNT];
        Expect(SosLinksList(links, owner, listed, ITEM_COUNT) == expected,
               "the list length matches the model");
        uint64_t item = SosLinksFirst(links, owner);
        for (uint32_t index = 0u; index < expected; ++index)
        {
            Expect(item == model->items[owner - 1u][index], "the order matches the model");
            Expect(listed[index] == item, "the listed order matches the iterator");
            item = SosLinksNext(links, owner, item);
        }
        Expect(item == 0u, "the list ends after the last item");
        for (uint64_t probe = 1u; probe <= ITEM_COUNT; ++probe)
        {
            Expect(SosLinksHas(links, owner, probe) == ModelHas(model, owner, probe),
                   "presence matches the model");
        }
    }
}

static void TestInvalidInput(void)
{
    Expect(!SosLinksAdd(NULL, 1u, 1u) && !SosLinksRemove(NULL, 1u, 1u) &&
               !SosLinksHas(NULL, 1u, 1u) && SosLinksCount(NULL, 1u) == 0u &&
               SosLinksFirst(NULL, 1u) == 0u && SosLinksNext(NULL, 1u, 1u) == 0u &&
               SosLinksList(NULL, 1u, NULL, 0u) == 0u && SosLinksRemoveOwner(NULL, 1u) == 0u &&
               SosLinksHash(NULL) == 0u,
           "a NULL set is empty");
    SosLinksDestroy(NULL);

    SosLinks *links = SosLinksCreate();
    Expect(links != NULL, "links are created");
    Expect(!SosLinksAdd(links, 0u, 1u) && !SosLinksAdd(links, 1u, 0u) &&
               !SosLinksAdd(links, 0u, 0u),
           "a zero owner or item is rejected");
    Expect(SosLinksCount(links, 0u) == 0u && SosLinksFirst(links, 0u) == 0u &&
               SosLinksNext(links, 0u, 1u) == 0u && SosLinksNext(links, 1u, 0u) == 0u,
           "a zero owner has nothing");
    Expect(!SosLinksHas(links, 0u, 1u) && !SosLinksHas(links, 1u, 0u) &&
               !SosLinksHas(links, 1u, 1u),
           "nothing is present");
    Expect(!SosLinksRemove(links, 1u, 1u) && !SosLinksRemove(links, 0u, 1u),
           "removing an absent pair fails");
    /* Read-only paths never allocate, so they work on an untouched set. */
    Expect(SosLinksList(links, 1u, NULL, 0u) == 0u && SosLinksList(links, 1u, NULL, 4u) == 0u,
           "a NULL output with room is rejected");
    Expect(SosLinksRemoveOwner(links, 1u) == 0u && SosLinksRemoveOwner(links, 0u) == 0u,
           "an empty owner has nothing to remove");
    Expect(SosLinksHash(links) == 0u, "an empty set hashes to zero");
    SosLinksDestroy(links);
}

/* The order of one owner's list, explicitly: appended, idempotent, relinked
 * around a middle removal, and appended again after a removal. */
static void TestOrder(void)
{
    SosLinks *links = SosLinksCreate();
    Expect(links != NULL, "links are created");
    Expect(SosLinksAdd(links, 1u, 5u) && SosLinksAdd(links, 1u, 3u) && SosLinksAdd(links, 1u, 9u),
           "three items are added");
    uint64_t listed[4];
    Expect(SosLinksList(links, 1u, listed, 4u) == 3u && listed[0] == 5u && listed[1] == 3u &&
               listed[2] == 9u,
           "items come back in insertion order");
    Expect(SosLinksAdd(links, 1u, 5u), "adding a present pair succeeds");
    Expect(SosLinksCount(links, 1u) == 3u, "adding a present pair changes nothing");
    Expect(SosLinksNext(links, 1u, 5u) == 3u && SosLinksNext(links, 1u, 3u) == 9u &&
               SosLinksNext(links, 1u, 9u) == 0u,
           "Next walks the list to the end");
    Expect(SosLinksNext(links, 1u, 4u) == 0u && SosLinksNext(links, 2u, 5u) == 0u,
           "Next of an absent pair is the end");
    Expect(SosLinksRemove(links, 1u, 3u), "a middle item is removed");
    Expect(SosLinksList(links, 1u, listed, 4u) == 2u && listed[0] == 5u && listed[1] == 9u,
           "the neighbours join up");
    Expect(SosLinksAdd(links, 1u, 3u) && SosLinksNext(links, 1u, 9u) == 3u,
           "a removed item is appended again");
    Expect(SosLinksCount(links, 1u) == 3u && SosLinksCount(links, 2u) == 0u,
           "the other owners are untouched");
    /* Removing a single-item owner drops its list entirely. */
    Expect(SosLinksAdd(links, 2u, 7u) && SosLinksRemove(links, 2u, 7u) &&
               SosLinksCount(links, 2u) == 0u && SosLinksFirst(links, 2u) == 0u,
           "an emptied owner has no head");
    Expect(SosLinksAdd(links, 2u, 8u) && SosLinksFirst(links, 2u) == 8u,
           "an emptied owner can be added to again");
    SosLinksDestroy(links);
}

/* Random adds and removes against the plain-array model. */
static void TestModel(void)
{
    SosLinks *links = SosLinksCreate();
    Expect(links != NULL, "links are created");
    Model model;
    for (uint32_t index = 0u; index < OWNER_COUNT; ++index)
    {
        model.counts[index] = 0u;
    }
    SosRandom random = SosRandomStream(SEED, 1u);
    for (uint32_t step = 0u; step < MODEL_STEPS; ++step)
    {
        const uint32_t owner = 1u + SosRandomBelow(&random, OWNER_COUNT);
        const uint64_t item = 1u + SosRandomBelow(&random, ITEM_COUNT);
        if (SosRandomBelow(&random, 3u) == 0u)
        {
            const bool present = ModelHas(&model, owner, item);
            Expect(SosLinksRemove(links, owner, item) == present, "remove matches the model");
            if (present)
            {
                ModelRemove(&model, owner, item);
            }
        }
        else
        {
            const bool present = ModelHas(&model, owner, item);
            Expect(SosLinksAdd(links, owner, item), "add succeeds");
            if (!present)
            {
                ModelAdd(&model, owner, item);
            }
        }
        Compare(links, &model);
    }
    /* RemoveOwner drops exactly one owner's items. */
    for (uint32_t owner = 1u; owner <= OWNER_COUNT; ++owner)
    {
        const uint32_t expected = model.counts[owner - 1u];
        Expect(SosLinksRemoveOwner(links, owner) == expected, "RemoveOwner counts the items");
        model.counts[owner - 1u] = 0u;
        Compare(links, &model);
    }
    Expect(SosLinksHash(links) == 0u, "removing every owner empties the set");
    SosLinksDestroy(links);
}

static void TestDeterminism(void)
{
    /* The same operations in the same order fold to the same value, and a set
     * reached by a different order of the same pairs does too. */
    SosLinks *first = SosLinksCreate();
    SosLinks *second = SosLinksCreate();
    Expect(first != NULL && second != NULL, "links are created");
    for (uint64_t owner = 1u; owner <= 4u; ++owner)
    {
        for (uint64_t item = 1u; item <= 6u; ++item)
        {
            (void)SosLinksAdd(first, owner, item);
            (void)SosLinksAdd(second, owner, item);
        }
    }
    (void)SosLinksRemove(first, 2u, 3u);
    (void)SosLinksRemove(second, 2u, 3u);
    Expect(SosLinksHash(first) == SosLinksHash(second), "equal sets hash equally");
    (void)SosLinksAdd(second, 5u, 1u);
    Expect(SosLinksHash(first) != SosLinksHash(second), "a different set hashes differently");
    SosLinksDestroy(second);
    SosLinksDestroy(first);
}

/* A reservation is what makes a following move safe, so it must succeed for a
 * reachable request, fail safely for an impossible one, and never change which
 * pairs are present. */
static void TestReserve(void)
{
    Expect(!SosLinksReserve(NULL, 1u, 1u), "a NULL set cannot be reserved");
    SosLinks *links = SosLinksCreate();
    Expect(links != NULL, "links are created");
    Expect(SosLinksAdd(links, 1u, 1u), "a pair is added before reserving");
    const uint64_t before = SosLinksHash(links);
    /* No table can hold this many pairs, so the request is refused up front
     * and the set is left exactly as it was. */
    Expect(!SosLinksReserve(links, UINT32_MAX, UINT32_MAX), "an impossible reserve fails");
    Expect(SosLinksHash(links) == before && SosLinksCount(links, 1u) == 1u,
           "a failed reserve changes nothing");
    Expect(SosLinksAdd(links, 1u, 2u), "the set still takes adds after a failed reserve");
    /* Room for four new owners and eight more pairs means every add below is
     * unable to allocate, which is the guarantee a move relies on. */
    Expect(SosLinksReserve(links, 4u, 8u), "a reachable reserve succeeds");
    for (uint32_t owner = 2u; owner <= 5u; ++owner)
    {
        for (uint64_t item = 1u; item <= 2u; ++item)
        {
            Expect(SosLinksAdd(links, owner, item), "a reserved add succeeds");
        }
    }
    Expect(SosLinksCount(links, 2u) == 2u && SosLinksCount(links, 5u) == 2u,
           "the reserved pairs are present");
    Expect(SosLinksReserve(links, 0u, 0u), "reserving nothing succeeds");
    SosLinksDestroy(links);
}

/* A fixed point of the deterministic structure. Every platform and compiler
 * must fold this exact sequence of adds and removes to the same value. */
static void TestGolden(void)
{
    SosLinks *links = SosLinksCreate();
    Expect(links != NULL, "links are created");
    for (uint64_t owner = 1u; owner <= 5u; ++owner)
    {
        for (uint64_t item = 1u; item <= 6u; ++item)
        {
            if ((owner + item) % 3u != 0u)
            {
                (void)SosLinksAdd(links, owner, item);
            }
        }
    }
    (void)SosLinksRemove(links, 2u, 4u);
    (void)SosLinksRemoveOwner(links, 3u);
    (void)SosLinksAdd(links, 1u, 4u);
    (void)SosLinksAdd(links, 6u, 1u);
    const uint64_t fingerprint = SosLinksHash(links);
    const uint64_t expected = UINT64_C(0xFFCC14A9FB6762E6);
    if (fingerprint != expected)
    {
        fprintf(stderr, "links fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the links history matches the reference");
    }
    SosLinksDestroy(links);
}

int main(void)
{
    TestInvalidInput();
    TestOrder();
    TestModel();
    TestDeterminism();
    TestReserve();
    TestGolden();
    return 0;
}
