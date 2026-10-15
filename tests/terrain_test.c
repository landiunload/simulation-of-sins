#include "sim/terrain.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "terrain test failed: %s\n", message);
        exit(1);
    }
}

static int64_t Absolute(int64_t value)
{
    return value < 0 ? -value : value;
}

/* Fixed points of the generator. Every platform and compiler must produce
 * exactly these values; a difference means two machines would see different
 * worlds from the same seed. */
static void TestGoldenValues(const SosTerrain *terrain)
{
    static const struct
    {
        int64_t x;
        int64_t y;
    } points[] = {
        {0, 0},
        {SOS_METRES(1000), SOS_METRES(-250)},
        {SOS_METRES(-123456), SOS_METRES(98765)},
        {INT64_C(1) << 60, -(INT64_C(1) << 60)},
    };
    static const int64_t expectedHeights[] = {113062, 86447, -111639, -165206};
    int mismatch = 0;
    for (size_t index = 0u; index < sizeof(points) / sizeof(points[0]); ++index)
    {
        const int64_t height = SosTerrainSurfaceZ(terrain, points[index].x, points[index].y);
        if (height != expectedHeights[index])
        {
            fprintf(stderr, "height %zu: %" PRId64 "\n", index, height);
            mismatch = 1;
        }
    }
    /* 64 m of ground from 30 m below the surface near the origin to 34 m above
     * it: topsoil, earth, stone and the first caves. */
    const SosTerrainGrid grid = {
        .cellX = -40,
        .cellY = 17,
        .cellZ = 160,
        .stride = 1u,
        .countX = 16u,
        .countY = 16u,
        .countZ = 128u,
    };
    static SosTerrainSample samples[16u * 16u * 128u];
    Expect(SosTerrainSampleGrid(terrain, &grid, samples), "a valid grid is sampled");
    uint64_t gridFingerprint = 0u;
    for (size_t index = 0u; index < sizeof(samples) / sizeof(samples[0]); ++index)
    {
        gridFingerprint = gridFingerprint * 1099511628211u + (uint32_t)samples[index].density;
        gridFingerprint = gridFingerprint * 1099511628211u + samples[index].material;
    }
    const uint64_t expectedGrid = UINT64_C(0x607C2E535C8614BA);
    if (gridFingerprint != expectedGrid)
    {
        fprintf(stderr, "grid fingerprint: 0x%016" PRIX64 "\n", gridFingerprint);
        mismatch = 1;
    }
    Expect(!mismatch, "generated terrain matches the reference values");
}

static void TestContinuity(const SosTerrain *terrain)
{
    /* Neighbouring half-metre columns never jump more than a few metres: the
     * steepest octave climbs at most about 1.6 m per half metre. */
    int64_t previous = SosTerrainSurfaceZ(terrain, SOS_METRES(-2000), SOS_METRES(300));
    int64_t lowest = previous;
    int64_t highest = previous;
    for (int64_t step = 1; step < 8000; ++step)
    {
        const int64_t x = SOS_METRES(-2000) + step * SOS_TERRAIN_CELL_UNITS;
        const int64_t height = SosTerrainSurfaceZ(terrain, x, SOS_METRES(300));
        Expect(Absolute(height - previous) < SOS_METRES(2), "the surface is continuous");
        previous = height;
        lowest = height < lowest ? height : lowest;
        highest = height > highest ? height : highest;
    }
    Expect(highest - lowest > SOS_METRES(5), "four kilometres of terrain are not flat");
    Expect(Absolute(lowest) < SOS_METRES(700) && Absolute(highest) < SOS_METRES(700),
           "heights stay within the octave amplitudes");
}

static void TestSamples(const SosTerrain *terrain)
{
    const int64_t x = SOS_METRES(37);
    const int64_t y = SOS_METRES(-12);
    const int64_t surface = SosTerrainSurfaceZ(terrain, x, y);
    const SosTerrainSample above =
        SosTerrainSampleAt(terrain, (SosPosition){x, y, surface + SOS_METRES(2)});
    const SosTerrainSample below =
        SosTerrainSampleAt(terrain, (SosPosition){x, y, surface - SOS_METRES(1) / 4});
    Expect(above.density < 0 && above.material == SOS_MATERIAL_AIR, "air above the surface");
    Expect(below.density > 0 && below.material != SOS_MATERIAL_AIR, "ground below the surface");
    const SosTerrainSample deep =
        SosTerrainSampleAt(terrain, (SosPosition){x, y, surface - SOS_METRES(2000)});
    Expect(deep.material == SOS_MATERIAL_STONE || deep.material == SOS_MATERIAL_AIR,
           "deep ground is stone or a cave");

    /* A grid sample has the density of the single sample at its lattice
     * point. Materials agree at full resolution; a coarser grid thickens the
     * top layer, since each of its samples stands for more ground. */
    for (uint32_t stride = 1u; stride <= 2u; ++stride)
    {
        const SosTerrainGrid grid = {
            .cellX = 74,
            .cellY = -24,
            .cellZ = surface / SOS_TERRAIN_CELL_UNITS - 8,
            .stride = stride,
            .countX = 3u,
            .countY = 3u,
            .countZ = 9u,
        };
        SosTerrainSample samples[3u * 3u * 9u];
        Expect(SosTerrainSampleGrid(terrain, &grid, samples), "a grid is sampled");
        for (uint32_t k = 0u; k < grid.countZ; ++k)
        {
            for (uint32_t j = 0u; j < grid.countY; ++j)
            {
                for (uint32_t i = 0u; i < grid.countX; ++i)
                {
                    const SosPosition position = {
                        (grid.cellX + (int64_t)i * grid.stride) * SOS_TERRAIN_CELL_UNITS,
                        (grid.cellY + (int64_t)j * grid.stride) * SOS_TERRAIN_CELL_UNITS,
                        (grid.cellZ + (int64_t)k * grid.stride) * SOS_TERRAIN_CELL_UNITS,
                    };
                    const SosTerrainSample single = SosTerrainSampleAt(terrain, position);
                    const SosTerrainSample gridded = samples[(k * 3u + j) * 3u + i];
                    Expect(single.density == gridded.density, "grid and single densities agree");
                    Expect(stride != 1u || single.material == gridded.material,
                           "full-resolution grid and single materials agree");
                }
            }
        }
    }

    /* Negative and enormous coordinates work like any other. */
    const int64_t far = INT64_C(1) << 61;
    const SosTerrainGrid distant = {
        .cellX = -far / SOS_TERRAIN_CELL_UNITS,
        .cellY = far / SOS_TERRAIN_CELL_UNITS,
        .cellZ = -4,
        .stride = 64u,
        .countX = 4u,
        .countY = 4u,
        .countZ = 4u,
    };
    SosTerrainSample distantSamples[64];
    Expect(SosTerrainSampleGrid(terrain, &distant, distantSamples), "distant terrain is sampled");
}

static void TestInvalidGrids(const SosTerrain *terrain)
{
    SosTerrainSample sample;
    const SosTerrainGrid valid = {.stride = 1u, .countX = 1u, .countY = 1u, .countZ = 1u};
    SosTerrainGrid grid = valid;
    grid.stride = 3u;
    Expect(!SosTerrainSampleGrid(terrain, &grid, &sample), "a stride must be a power of two");
    grid = valid;
    grid.countZ = 0u;
    Expect(!SosTerrainSampleGrid(terrain, &grid, &sample), "an empty grid is rejected");
    grid = valid;
    grid.countX = SOS_TERRAIN_GRID_MAX_SAMPLES;
    grid.countY = 2u;
    Expect(!SosTerrainSampleGrid(terrain, &grid, &sample), "an oversized grid is rejected");
    grid = valid;
    grid.cellX = INT64_MAX;
    Expect(!SosTerrainSampleGrid(terrain, &grid, &sample), "an out-of-range grid is rejected");
    Expect(!SosTerrainSampleGrid(terrain, NULL, &sample) &&
               !SosTerrainSampleGrid(terrain, &valid, NULL),
           "missing arguments are rejected");
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, 20261015u);
    TestContinuity(&terrain);
    TestSamples(&terrain);
    TestInvalidGrids(&terrain);
    TestGoldenValues(&terrain);
    return 0;
}
