#include "sim/cells.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GOLDEN UINT64_C(0x8944612BA00E2D4E)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "cells test failed: %s\n", message);
        exit(1);
    }
}

/* Records the cells a walk visits, so a test can check the order and bounds. */
#define RECORD_CAPACITY 256u

typedef struct Recorder
{
    int64_t cellsX[RECORD_CAPACITY];
    int64_t cellsY[RECORD_CAPACITY];
    uint32_t count;
} Recorder;

static void Record(int64_t cellX, int64_t cellY, void *context)
{
    Recorder *recorder = context;
    if (recorder->count < RECORD_CAPACITY)
    {
        recorder->cellsX[recorder->count] = cellX;
        recorder->cellsY[recorder->count] = cellY;
    }
    recorder->count += 1u;
}

static void ExpectCell(const Recorder *recorder, uint32_t index, int64_t cellX, int64_t cellY,
                       const char *message)
{
    Expect(index < recorder->count && recorder->cellsX[index] == cellX &&
               recorder->cellsY[index] == cellY,
           message);
}

static void TestIndex(void)
{
    Expect(SosCellsIndex(0, 12u) == 0 && SosCellsIndex(4095, 12u) == 0 &&
               SosCellsIndex(4096, 12u) == 1,
           "a cell holds its whole span");
    /* Floor division keeps a negative coordinate in the cell below. */
    Expect(SosCellsIndex(-1, 12u) == -1 && SosCellsIndex(-4096, 12u) == -1 &&
               SosCellsIndex(-4097, 12u) == -2,
           "negative coordinates floor to the cell below");
    Expect(SosCellsIndex(INT64_MAX, 62u) == 1 && SosCellsIndex(INT64_MIN, 62u) == -2,
           "the world's extremes stay finite");
    Expect(SosCellsIndex(123, 100u) == SosCellsIndex(123, SOS_CELLS_MAX_SHIFT),
           "a shift above the maximum is clamped");
}

static void TestSquared(void)
{
    Expect(SosCellsSquared(0u) == 0u && SosCellsSquared(7u) == 49u, "small squares are exact");
    Expect(SosCellsSquared(UINT64_C(100000)) == UINT64_C(10000000000), "a large square still fits");
    Expect(SosCellsSquared((uint64_t)UINT32_MAX) == (uint64_t)UINT32_MAX * (uint64_t)UINT32_MAX,
           "the largest square that fits is exact");
    Expect(SosCellsSquared((uint64_t)UINT32_MAX + 1u) == UINT64_MAX &&
               SosCellsSquared(UINT64_MAX) == UINT64_MAX,
           "a square that cannot fit saturates");
}

static void TestGroundDistance(void)
{
    const SosPosition origin = {0, 0, 0};
    Expect(SosCellsGroundDistanceSquared(origin, origin) == 0u, "equal positions are zero");
    Expect(SosCellsGroundDistanceSquared(origin, (SosPosition){3, 4, 0}) == 25u, "a 3-4-5 step");
    Expect(SosCellsGroundDistanceSquared(origin, (SosPosition){0, 0, 12345}) == 0u,
           "height is ignored");
    Expect(SosCellsGroundDistanceSquared((SosPosition){-2, -3, 5}, (SosPosition){1, 1, -5}) == 25u,
           "signs do not matter");
    /* Separations whose squares cannot fit saturate instead of wrapping. */
    Expect(SosCellsGroundDistanceSquared((SosPosition){INT64_MIN, 0, 0},
                                         (SosPosition){INT64_MAX, 0, 0}) == UINT64_MAX,
           "the widest separation saturates");
    Expect(SosCellsGroundDistanceSquared(origin, (SosPosition){UINT64_C(1) << 32, 0, 0}) ==
               UINT64_MAX,
           "a separation whose square overflows saturates");
}

static void TestRingOrder(void)
{
    Recorder recorder = {0};
    uint64_t best = UINT64_MAX;
    /* Shift 1 makes a cell two units wide, so radius 3 covers rings 0 to 2. */
    Expect(SosCellsVisitRings((SosPosition){1, 1, 0}, 3, 1u, 1024u, &best, Record, &recorder),
           "a ring walk runs");
    Expect(recorder.count == 25u, "three rings hold one, eight and sixteen cells");
    ExpectCell(&recorder, 0u, 0, 0, "ring zero is the centre cell");
    ExpectCell(&recorder, 1u, -1, -1, "the top row of a ring runs left to right");
    ExpectCell(&recorder, 3u, 1, -1, "the top row ends at its right edge");
    ExpectCell(&recorder, 4u, -1, 0, "an inner row has only its ends");
    ExpectCell(&recorder, 5u, 1, 0, "an inner row ends on its right edge");
    ExpectCell(&recorder, 6u, -1, 1, "the bottom row starts at its left edge");
    ExpectCell(&recorder, 8u, 1, 1, "the bottom row ends at its right edge");
    ExpectCell(&recorder, 9u, -2, -2, "ring two starts with its top-left cell");
    ExpectCell(&recorder, 13u, 2, -2, "ring two's top row ends at its right edge");
    ExpectCell(&recorder, 14u, -2, -1, "ring two's inner rows start at their left ends");
    ExpectCell(&recorder, 24u, 2, 2, "the last cell is the far corner");
}

static void TestRingEarlyStop(void)
{
    Recorder recorder = {0};
    /* A hit at distance zero means no ring beyond the first can improve it. */
    uint64_t best = 0u;
    Expect(SosCellsVisitRings((SosPosition){1, 1, 0}, 100, 1u, 1024u, &best, Record, &recorder),
           "an early-stop walk runs");
    Expect(recorder.count == 9u, "the walk stops before a ring that cannot beat the best");
}

static void TestRingBounds(void)
{
    Recorder recorder = {0};
    uint64_t best = UINT64_MAX;
    /* maxCells 4 allows one cell per side beyond the centre: a radius of two
     * units (one cell) just fits, three units does not. */
    Expect(!SosCellsVisitRings((SosPosition){0, 0, 0}, 3, 1u, 4u, &best, Record, &recorder) &&
               recorder.count == 0u,
           "a radius past the cell bound visits nothing");
    Expect(SosCellsVisitRings((SosPosition){0, 0, 0}, 2, 1u, 4u, &best, Record, &recorder) &&
               recorder.count == 25u,
           "a radius within the bound is walked");
    recorder.count = 0u;
    Expect(
        !SosCellsVisitRings((SosPosition){0, 0, 0}, -1, 1u, 1024u, &best, Record, &recorder) &&
            !SosCellsVisitRings((SosPosition){0, 0, 0}, 1, 0u, 1024u, &best, Record, &recorder) &&
            !SosCellsVisitRings((SosPosition){0, 0, 0}, 1, 1u, 0u, &best, Record, &recorder) &&
            !SosCellsVisitRings((SosPosition){0, 0, 0}, 1, 1u, 1024u, NULL, Record, &recorder) &&
            !SosCellsVisitRings((SosPosition){0, 0, 0}, 1, 1u, 1024u, &best, NULL, &recorder),
        "an unusable walk visits nothing");
}

static void TestBox(void)
{
    Recorder recorder = {0};
    Expect(SosCellsVisitBox(0, 0, 3, 3, 1u, 1024u, Record, &recorder), "a box walk runs");
    Expect(recorder.count == 4u, "a two-by-two box holds four cells");
    ExpectCell(&recorder, 0u, 0, 0, "the box starts at its first cell");
    ExpectCell(&recorder, 1u, 1, 0, "X runs fastest");
    ExpectCell(&recorder, 2u, 0, 1, "then Y advances");
    ExpectCell(&recorder, 3u, 1, 1, "the box ends at its last cell");

    recorder.count = 0u;
    Expect(SosCellsVisitBox(-3, -3, 0, 0, 1u, 1024u, Record, &recorder) && recorder.count == 9u,
           "a box across the origin holds negative cells");
    ExpectCell(&recorder, 0u, -2, -2, "negative cells floor down");
    ExpectCell(&recorder, 8u, 0, 0, "and reach the origin last");

    recorder.count = 0u;
    Expect(!SosCellsVisitBox(3, 0, 0, 3, 1u, 1024u, Record, &recorder) &&
               !SosCellsVisitBox(0, 3, 3, 0, 1u, 1024u, Record, &recorder),
           "a reversed box visits nothing");
    Expect(!SosCellsVisitBox(0, 0, 8, 8, 1u, 4u, Record, &recorder) && recorder.count == 0u,
           "a box past the cell bound visits nothing");
    Expect(!SosCellsVisitBox(0, 0, 1, 1, 1u, 0u, Record, &recorder),
           "a zero cell bound visits nothing");
}

static void TestDeterminism(void)
{
    Recorder first = {0};
    Recorder second = {0};
    uint64_t firstBest = UINT64_MAX;
    uint64_t secondBest = UINT64_MAX;
    const SosPosition around = {17, -3, 0};
    Expect(SosCellsVisitRings(around, 80, 4u, 1024u, &firstBest, Record, &first) &&
               SosCellsVisitRings(around, 80, 4u, 1024u, &secondBest, Record, &second),
           "the same walk runs twice");
    Expect(first.count == second.count, "the same walk counts the same cells");
    for (uint32_t index = 0u; index < first.count; ++index)
    {
        Expect(first.cellsX[index] == second.cellsX[index] &&
                   first.cellsY[index] == second.cellsY[index],
               "the same walk visits the same cells in the same order");
    }
}

/* Fixed points of a deterministic walk. Every platform and compiler must visit
 * exactly these cells. */
static void TestGolden(void)
{
    Recorder recorder = {0};
    uint64_t best = UINT64_MAX;
    Expect(SosCellsVisitRings((SosPosition){17, -3, 0}, 80, 4u, 1024u, &best, Record, &recorder),
           "the golden walk runs");
    uint64_t fingerprint = recorder.count;
    for (uint32_t index = 0u; index < recorder.count; ++index)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)recorder.cellsX[index]);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)recorder.cellsY[index]);
    }
    if (fingerprint != GOLDEN)
    {
        fprintf(stderr, "cells fingerprint: 0x%016" PRIX64 " (%u cells)\n", fingerprint,
                recorder.count);
        Expect(0, "the ring walk matches the reference");
    }
}

int main(void)
{
    TestIndex();
    TestSquared();
    TestGroundDistance();
    TestRingOrder();
    TestRingEarlyStop();
    TestRingBounds();
    TestBox();
    TestDeterminism();
    TestGolden();
    return 0;
}
