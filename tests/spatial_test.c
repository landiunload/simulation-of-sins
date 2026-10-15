#include "sim/cells.h"
#include "sim/random.h"
#include "sim/spatial.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
/* One world unit is 1/1024 m; shift 10 makes metre-wide cells. */
#define CELL_SHIFT 10u
#define BRUTE_COUNT 4000u
#define LARGE_COUNT 100000u
#define TRIALS 40
#define GOLDEN UINT64_C(0xFD0D07301D1C82AE)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "spatial test failed: %s\n", message);
        exit(1);
    }
}

static uint64_t bruteSerials[BRUTE_COUNT];
static SosPosition brutePositions[BRUTE_COUNT];
static uint64_t largeSerials[LARGE_COUNT];
static SosPosition largePositions[LARGE_COUNT];
static uint64_t results[LARGE_COUNT];

/* A matched being for brute-force comparison. */
typedef struct BruteMatch
{
    uint64_t distance;
    uint64_t serial;
} BruteMatch;

static BruteMatch matches[BRUTE_COUNT];

static int CompareMatches(const void *left, const void *right)
{
    const BruteMatch *a = left;
    const BruteMatch *b = right;
    if (a->distance != b->distance)
    {
        return a->distance < b->distance ? -1 : 1;
    }
    if (a->serial != b->serial)
    {
        return a->serial < b->serial ? -1 : 1;
    }
    return 0;
}

/* Every being within reach, sorted nearest first, ties by serial. */
static uint32_t BruteRadius(SosPosition center, int64_t radius, uint64_t exclude)
{
    const uint64_t radiusSquared = SosCellsSquared((uint64_t)radius);
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < BRUTE_COUNT; ++index)
    {
        if (bruteSerials[index] == exclude)
        {
            continue;
        }
        const uint64_t distance = SosCellsGroundDistanceSquared(center, brutePositions[index]);
        if (distance <= radiusSquared)
        {
            matches[count] = (BruteMatch){.distance = distance, .serial = bruteSerials[index]};
            ++count;
        }
    }
    qsort(matches, count, sizeof(*matches), CompareMatches);
    return count;
}

static void TestBasics(void)
{
    SosSpatial *spatial = SosSpatialCreate(CELL_SHIFT, 4u);
    Expect(spatial != NULL, "an index is created");
    Expect(SosSpatialCount(spatial) == 0u, "a new index is empty");
    Expect(SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, SOS_METRES(10), results, 8u) ==
               0u,
           "an empty index finds nothing");

    /* Three beings on a line at 1 m, 2 m and 3 m, plus one behind at -2 m. */
    Expect(SosSpatialInsert(spatial, 10u, (SosPosition){SOS_METRES(1), 0, 0}), "insert 10");
    Expect(SosSpatialInsert(spatial, 20u, (SosPosition){SOS_METRES(2), 0, 0}), "insert 20");
    Expect(SosSpatialInsert(spatial, 30u, (SosPosition){SOS_METRES(3), 0, 0}), "insert 30");
    Expect(SosSpatialInsert(spatial, 40u, (SosPosition){SOS_METRES(-2), 0, 0}), "insert 40");
    Expect(SosSpatialCount(spatial) == 4u, "the index counts every insert");

    const uint32_t found =
        SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, SOS_METRES(4), results, 8u);
    Expect(found == 4u, "all four are in reach");
    Expect(results[0] == 10u && results[1] == 20u && results[2] == 40u && results[3] == 30u,
           "results are nearest first, ties by serial");

    uint64_t nearest = 0u;
    Expect(SosSpatialNearest(spatial, (SosPosition){0, 0, 0}, SOS_METRES(4), 0u, &nearest) &&
               nearest == 10u,
           "the nearest is found");
    Expect(SosSpatialNearest(spatial, (SosPosition){0, 0, 0}, SOS_METRES(4), 10u, &nearest) &&
               nearest == 20u,
           "the nearest excluding a serial is found");
    Expect(!SosSpatialNearest(spatial, (SosPosition){SOS_METRES(100), 0, 0}, SOS_METRES(1), 0u,
                              &nearest),
           "nothing is found when nothing is near");

    Expect(SosSpatialCountInBox(spatial, SOS_METRES(-3), -1, SOS_METRES(2), 1) == 3u,
           "a box counts both sides of the origin");
    Expect(SosSpatialCountInBox(spatial, SOS_METRES(1), -1, SOS_METRES(1), 1) == 1u,
           "a narrow box counts one");
    Expect(SosSpatialCountInBox(spatial, 1, 0, 0, 0) == 0u, "an empty box counts none");

    SosSpatialClear(spatial);
    Expect(SosSpatialCount(spatial) == 0u &&
               SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, SOS_METRES(4), results, 8u) ==
                   0u,
           "clearing forgets everyone");
    Expect(SosSpatialInsert(spatial, 50u, (SosPosition){0, 0, 0}), "the index is reusable");
    SosSpatialDestroy(spatial);
}

static void TestTruncation(void)
{
    SosSpatial *spatial = SosSpatialCreate(CELL_SHIFT, 0u);
    Expect(spatial != NULL, "an index is created");
    for (uint32_t step = 1u; step <= 20u; ++step)
    {
        Expect(SosSpatialInsert(spatial, step, (SosPosition){SOS_METRES(step), 0, 0}),
               "insert on a line");
    }
    /* Capacity 3 must keep the three nearest, in order, while reporting all. */
    const uint32_t found =
        SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, SOS_METRES(100), results, 3u);
    Expect(found == 20u, "the total is reported even when truncated");
    Expect(results[0] == 1u && results[1] == 2u && results[2] == 3u,
           "truncation keeps the nearest");

    /* Capacity 0 is a pure count. */
    Expect(SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, SOS_METRES(100), NULL, 0u) == 20u,
           "a zero capacity only counts");
    /* Ties break by serial whatever order the pool was built in. */
    Expect(SosSpatialInsert(spatial, 5u, (SosPosition){SOS_METRES(3), 0, 0}), "insert a tie");
    const uint32_t tied = SosSpatialQueryRadius(spatial, (SosPosition){SOS_METRES(3), 0, 0},
                                                SOS_METRES(1) / 2, results, 8u);
    Expect(tied == 2u && results[0] == 3u && results[1] == 5u, "a tie falls back to serial");
    SosSpatialDestroy(spatial);
}

static void TestEdges(void)
{
    Expect(SosSpatialCreate(SOS_SPATIAL_MAX_CELL_SHIFT + 1u, 0u) == NULL,
           "an oversized cell shift is rejected");
    SosSpatial *largest = SosSpatialCreate(SOS_SPATIAL_MAX_CELL_SHIFT, 0u);
    Expect(largest != NULL, "the largest cell shift is accepted");
    SosSpatialDestroy(largest);
    Expect(!SosSpatialInsert(NULL, 1u, (SosPosition){0, 0, 0}), "insert rejects a NULL index");
    Expect(SosSpatialQueryRadius(NULL, (SosPosition){0, 0, 0}, 1, results, 1u) == 0u,
           "a query rejects a NULL index");
    Expect(SosSpatialCount(NULL) == 0u && SosSpatialHash(NULL) == 0u, "count and hash handle NULL");

    SosSpatial *spatial = SosSpatialCreate(CELL_SHIFT, 0u);
    Expect(spatial != NULL, "an index is created");
    Expect(!SosSpatialInsert(spatial, 0u, (SosPosition){0, 0, 0}), "serial zero is nobody");
    SosSpatial *twin = SosSpatialCreate(CELL_SHIFT, 0u);
    Expect(twin != NULL && SosSpatialHash(spatial) == SosSpatialHash(twin),
           "equal empty indexes hash equally");
    SosSpatialDestroy(twin);

    Expect(SosSpatialInsert(spatial, 1u, (SosPosition){INT64_MIN, INT64_MIN, INT64_MIN}),
           "an extreme corner is inserted");
    Expect(SosSpatialInsert(spatial, 2u, (SosPosition){INT64_MAX, INT64_MAX, INT64_MAX}),
           "the opposite extreme is inserted");
    Expect(SosSpatialInsert(spatial, 3u, (SosPosition){0, 0, 0}), "the origin is inserted");

    /* An unbounded query must not overflow while measuring huge separations. */
    const uint32_t found =
        SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, INT64_MAX, results, LARGE_COUNT);
    Expect(found == 3u && results[0] == 3u, "the origin is nearest among extremes");
    uint64_t nearest = 0u;
    Expect(SosSpatialNearest(spatial, (SosPosition){INT64_MIN, INT64_MIN, 0}, INT64_MAX, 0u,
                             &nearest) &&
               nearest == 1u,
           "nearest works at an extreme corner");
    Expect(SosSpatialCountInBox(spatial, INT64_MIN, INT64_MIN, INT64_MAX, INT64_MAX) == 3u,
           "the whole world counts everyone");
    Expect(SosSpatialCountInBox(spatial, INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN) == 0u,
           "an inverted box counts none");
    Expect(SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, -1, results, LARGE_COUNT) == 0u,
           "a negative radius finds nothing");
    Expect(SosSpatialQueryRadius(spatial, (SosPosition){0, 0, 0}, 1, NULL, 4u) == 0u,
           "a NULL output with capacity is rejected");
    SosSpatialDestroy(spatial);

    /* A hint too large to honour must still leave a usable index. */
    SosSpatial *hinted = SosSpatialCreate(CELL_SHIFT, UINT32_MAX);
    Expect(hinted != NULL, "an unmeetable hint still creates an index");
    Expect(SosSpatialInsert(hinted, 7u, (SosPosition){0, 0, 0}), "the hinted index inserts");
    SosSpatialDestroy(hinted);
}

static void TestBruteForce(void)
{
    SosSpatial *spatial = SosSpatialCreate(CELL_SHIFT, BRUTE_COUNT);
    Expect(spatial != NULL, "an index is created");
    SosRandom random = SosRandomStream(SEED, 11u);
    for (uint32_t index = 0u; index < BRUTE_COUNT; ++index)
    {
        bruteSerials[index] = index + 1u;
        brutePositions[index] = (SosPosition){
            .x = SosRandomRange(&random, -SOS_METRES(5000), SOS_METRES(5000)),
            .y = SosRandomRange(&random, -SOS_METRES(5000), SOS_METRES(5000)),
            .z = 0,
        };
        Expect(SosSpatialInsert(spatial, bruteSerials[index], brutePositions[index]),
               "a random being is inserted");
    }

    for (int trial = 0; trial < TRIALS; ++trial)
    {
        const SosPosition center = {
            .x = SosRandomRange(&random, -SOS_METRES(6000), SOS_METRES(6000)),
            .y = SosRandomRange(&random, -SOS_METRES(6000), SOS_METRES(6000)),
            .z = 0,
        };
        const int64_t radius = SosRandomRange(&random, 0, SOS_METRES(400));
        const uint64_t exclude = SosRandomBelow(&random, 4u) == 0u
                                     ? (uint64_t)SosRandomBelow(&random, BRUTE_COUNT) + 1u
                                     : 0u;

        const uint32_t expected = BruteRadius(center, radius, 0u);
        for (uint32_t capacity = 1u; capacity <= 16u; capacity += 5u)
        {
            const uint32_t found =
                SosSpatialQueryRadius(spatial, center, radius, results, capacity);
            Expect(found == expected, "the query count matches brute force");
            const uint32_t written = expected < capacity ? expected : capacity;
            for (uint32_t index = 0u; index < written; ++index)
            {
                Expect(results[index] == matches[index].serial,
                       "the query order matches brute force");
            }
        }

        const uint32_t excluded = BruteRadius(center, radius, exclude);
        uint64_t nearest = 0u;
        const bool any = SosSpatialNearest(spatial, center, radius, exclude, &nearest);
        Expect(any == (excluded != 0u), "nearest exists exactly when brute force finds one");
        if (any)
        {
            Expect(nearest == matches[0].serial, "nearest matches brute force");
        }

        const int64_t minX = center.x - radius;
        const int64_t maxX = center.x + radius;
        const int64_t minY = center.y - radius;
        const int64_t maxY = center.y + radius;
        uint32_t boxed = 0u;
        for (uint32_t index = 0u; index < BRUTE_COUNT; ++index)
        {
            if (brutePositions[index].x >= minX && brutePositions[index].x <= maxX &&
                brutePositions[index].y >= minY && brutePositions[index].y <= maxY)
            {
                ++boxed;
            }
        }
        Expect(SosSpatialCountInBox(spatial, minX, minY, maxX, maxY) == boxed,
               "the box count matches brute force");
    }
    SosSpatialDestroy(spatial);
}

/* Inserting the same beings in another order must give the same hash and the
 * same query answers. */
static void TestDeterminism(void)
{
    SosSpatial *forward = SosSpatialCreate(CELL_SHIFT, 0u);
    SosSpatial *backward = SosSpatialCreate(CELL_SHIFT, 0u);
    Expect(forward != NULL && backward != NULL, "two indexes are created");
    SosRandom random = SosRandomStream(SEED, 23u);
    for (uint32_t index = 0u; index < BRUTE_COUNT; ++index)
    {
        bruteSerials[index] = index + 1u;
        brutePositions[index] = (SosPosition){
            .x = SosRandomRange(&random, -SOS_METRES(900), SOS_METRES(900)),
            .y = SosRandomRange(&random, -SOS_METRES(900), SOS_METRES(900)),
            .z = 0,
        };
        Expect(SosSpatialInsert(forward, bruteSerials[index], brutePositions[index]),
               "a being is inserted forward");
    }
    for (uint32_t index = BRUTE_COUNT; index > 0u; --index)
    {
        Expect(SosSpatialInsert(backward, bruteSerials[index - 1u], brutePositions[index - 1u]),
               "a being is inserted backward");
    }
    Expect(SosSpatialHash(forward) == SosSpatialHash(backward),
           "the hash does not depend on insertion order");

    const SosPosition center = {SOS_METRES(30), SOS_METRES(-40), 0};
    uint64_t firstOut[32];
    uint64_t againOut[32];
    uint64_t otherOut[32];
    const uint32_t first = SosSpatialQueryRadius(forward, center, SOS_METRES(200), firstOut, 32u);
    const uint32_t again = SosSpatialQueryRadius(forward, center, SOS_METRES(200), againOut, 32u);
    Expect(first == again, "a repeated query gives the same count");
    const uint32_t written = first < 32u ? first : 32u;
    for (uint32_t index = 0u; index < written; ++index)
    {
        Expect(firstOut[index] == againOut[index], "a repeated query gives the same order");
    }
    const uint32_t other = SosSpatialQueryRadius(backward, center, SOS_METRES(200), otherOut, 32u);
    Expect(other == first, "another insertion order gives the same count");
    for (uint32_t index = 0u; index < written; ++index)
    {
        Expect(firstOut[index] == otherOut[index], "another insertion order gives the same order");
    }
    uint64_t nearestForward = 0u;
    uint64_t nearestBackward = 0u;
    Expect(SosSpatialNearest(forward, center, SOS_METRES(200), 0u, &nearestForward) ==
                   SosSpatialNearest(backward, center, SOS_METRES(200), 0u, &nearestBackward) &&
               nearestForward == nearestBackward,
           "another insertion order gives the same nearest");
    SosSpatialDestroy(forward);
    SosSpatialDestroy(backward);
}

static uint64_t Fingerprint(const SosSpatial *spatial)
{
    uint64_t fingerprint = SosSpatialHash(spatial);
    fingerprint = SosHashCombine(fingerprint, SosSpatialCount(spatial));
    const SosPosition centers[3] = {
        {0, 0, 0},
        {SOS_METRES(1000), SOS_METRES(-2000), 0},
        {-SOS_METRES(3000), SOS_METRES(4000), 0},
    };
    for (int query = 0; query < 3; ++query)
    {
        const uint32_t found =
            SosSpatialQueryRadius(spatial, centers[query], SOS_METRES(250), results, 16u);
        fingerprint = SosHashCombine(fingerprint, found);
        const uint32_t written = found < 16u ? found : 16u;
        for (uint32_t index = 0u; index < written; ++index)
        {
            fingerprint = SosHashCombine(fingerprint, results[index]);
        }
        uint64_t nearest = 0u;
        if (SosSpatialNearest(spatial, centers[query], SOS_METRES(500), 0u, &nearest))
        {
            fingerprint = SosHashCombine(fingerprint, nearest);
        }
        fingerprint = SosHashCombine(
            fingerprint, SosSpatialCountInBox(spatial, centers[query].x - SOS_METRES(100),
                                              centers[query].y - SOS_METRES(100),
                                              centers[query].x + SOS_METRES(100),
                                              centers[query].y + SOS_METRES(100)));
    }
    return fingerprint;
}

/* A hundred thousand beings, built and queried inside the time budget. */
static void TestLarge(void)
{
    SosSpatial *spatial = SosSpatialCreate(CELL_SHIFT, LARGE_COUNT);
    Expect(spatial != NULL, "a large index is created");
    SosRandom random = SosRandomStream(SEED, 41u);
    for (uint32_t index = 0u; index < LARGE_COUNT; ++index)
    {
        largeSerials[index] = index + 1u;
        largePositions[index] = (SosPosition){
            .x = SosRandomRange(&random, -SOS_METRES(20000), SOS_METRES(20000)),
            .y = SosRandomRange(&random, -SOS_METRES(20000), SOS_METRES(20000)),
            .z = 0,
        };
        Expect(SosSpatialInsert(spatial, largeSerials[index], largePositions[index]),
               "a large being is inserted");
    }
    Expect(SosSpatialCount(spatial) == LARGE_COUNT, "every large being is counted");
    const uint64_t fingerprint = Fingerprint(spatial);
    if (fingerprint != GOLDEN)
    {
        fprintf(stderr, "spatial fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the spatial index matches the reference");
    }
    SosSpatialDestroy(spatial);
}

int main(void)
{
    TestBasics();
    TestTruncation();
    TestEdges();
    TestBruteForce();
    TestDeterminism();
    TestLarge();
    return 0;
}
