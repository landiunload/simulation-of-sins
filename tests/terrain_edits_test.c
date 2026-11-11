#include "sim/terrain.h"
#include "sim/terrain_edits.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "terrain edits test failed: %s\n", message);
        exit(1);
    }
}

static int64_t Absolute(int64_t value)
{
    return value < 0 ? -value : value;
}

typedef struct Ground
{
    SosTerrain terrain;
    SosTerrainEdits *edits;
} Ground;

static Ground CreateGround(uint64_t seed)
{
    Ground ground;
    SosTerrainInit(&ground.terrain, seed);
    ground.edits = SosTerrainEditsCreate();
    Expect(ground.edits != NULL, "edits are created");
    ground.terrain.edits = ground.edits;
    return ground;
}

static SosPosition OnSurface(const SosTerrain *terrain, int64_t x, int64_t y)
{
    return (SosPosition){x, y, SosTerrainSurfaceZ(terrain, x, y)};
}

/* A meadow: grass over dirt, where a pit shows the layers. */
static SosPosition FindGrass(const SosTerrain *terrain)
{
    for (int64_t step = 0; step < 4096; ++step)
    {
        const int64_t x = step * SOS_METRES(997);
        const int64_t y = step * SOS_METRES(-613);
        if (SosTerrainSurfaceAt(terrain, x, y).material == SOS_MATERIAL_GRASS)
        {
            return OnSurface(terrain, x, y);
        }
    }
    Expect(0, "the world has a meadow");
    return (SosPosition){0, 0, 0};
}

static void TestUntouchedGroundIsTheFunction(void)
{
    SosTerrain plain;
    SosTerrainInit(&plain, 11u);
    Ground ground = CreateGround(11u);
    for (int64_t index = 0; index < 50; ++index)
    {
        const SosPosition point = {index * SOS_METRES(37) - SOS_METRES(900),
                                   index * SOS_METRES(-13) + 77, index * SOS_METRES(3) - 5000};
        Expect(SosTerrainSurfaceZ(&plain, point.x, point.y) ==
                   SosTerrainSurfaceZ(&ground.terrain, point.x, point.y),
               "an empty layer keeps the surface");
        const SosTerrainSample before = SosTerrainSampleAt(&plain, point);
        const SosTerrainSample after = SosTerrainSampleAt(&ground.terrain, point);
        Expect(before.density == after.density && before.material == after.material,
               "an empty layer keeps every sample");
    }
    Expect(SosTerrainEditsHash(ground.edits) == 0u, "no edits hash to zero");
    SosTerrainEditsDestroy(ground.edits);
}

static void TestDigging(void)
{
    Ground ground = CreateGround(5u);
    const SosPosition center = FindGrass(&ground.terrain);
    const SosPosition far = OnSurface(&ground.terrain, center.x + SOS_METRES(30), center.y);
    const SosTerrainSample farBefore = SosTerrainSampleAt(&ground.terrain, far);

    SosTerrainBox box;
    const uint32_t changed =
        SosTerrainEditsDig(ground.edits, &ground.terrain, center, SOS_METRES(2), &box);
    Expect(changed > 0u, "digging changes the ground");
    Expect(box.min.x <= center.x && box.max.x >= center.x && box.min.y <= center.y &&
               box.max.y >= center.y && box.min.z < center.z &&
               box.max.z <= center.z + SOS_TERRAIN_CELL_UNITS,
           "the changed box spans the ground under the centre");
    Expect(box.max.x - box.min.x <= SOS_METRES(7), "the changed box stays near the sphere");
    Expect(SosTerrainEditsChunkCount(ground.edits) > 0u, "chunks are stored");

    Expect(SosTerrainSampleAt(&ground.terrain, center).density < 0, "the centre is air");
    const SosTerrainSurface bottom = SosTerrainSurfaceAt(&ground.terrain, center.x, center.y);
    Expect(Absolute(bottom.z - (center.z - SOS_METRES(2))) < SOS_METRES(1) / 4,
           "the surface sinks by the radius");
    Expect(bottom.material == SOS_MATERIAL_DIRT, "the pit floor is dirt below the grass");

    const SosTerrainSample farAfter = SosTerrainSampleAt(&ground.terrain, far);
    Expect(farAfter.density == farBefore.density, "ground far away is untouched");
    Expect(SosTerrainSurfaceZ(&ground.terrain, far.x, far.y) == far.z,
           "the surface far away is untouched");

    const SosTerrainGrid grid = {.cellX = SosFloorShift(center.x, SOS_TERRAIN_CELL_SHIFT),
                                 .cellY = SosFloorShift(center.y, SOS_TERRAIN_CELL_SHIFT),
                                 .cellZ = SosFloorShift(center.z, SOS_TERRAIN_CELL_SHIFT),
                                 .stride = 1u,
                                 .countX = 1u,
                                 .countY = 1u,
                                 .countZ = 1u};
    SosTerrainSample sample;
    Expect(SosTerrainSampleGrid(&ground.terrain, &grid, &sample), "a grid samples dug ground");
    const SosTerrainSample cell =
        SosTerrainSampleCell(&ground.terrain, grid.cellX, grid.cellY, grid.cellZ);
    Expect(sample.density == cell.density && sample.density < 0, "grids see the edits");

    const uint64_t hash = SosTerrainEditsHash(ground.edits);
    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain, center, SOS_METRES(2), NULL) == 0u,
           "digging the same sphere again changes nothing");
    Expect(SosTerrainEditsHash(ground.edits) == hash, "a repeated dig keeps the hash");
    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain,
                              (SosPosition){center.x, center.y, bottom.z}, SOS_METRES(1),
                              NULL) > 0u,
           "digging the floor deepens the pit");
    Expect(SosTerrainSurfaceZ(&ground.terrain, center.x, center.y) < bottom.z - SOS_METRES(1) / 2,
           "the pit is deeper");
    SosTerrainEditsDestroy(ground.edits);
}

static void TestOrderDoesNotMatter(void)
{
    Ground first = CreateGround(9u);
    Ground second = CreateGround(9u);
    const SosPosition a = OnSurface(&first.terrain, 0, 0);
    const SosPosition b = OnSurface(&first.terrain, SOS_METRES(2), SOS_METRES(1));
    Expect(SosTerrainEditsDig(first.edits, &first.terrain, a, SOS_METRES(2), NULL) > 0u &&
               SosTerrainEditsDig(first.edits, &first.terrain, b, SOS_METRES(3), NULL) > 0u,
           "two digs");
    Expect(SosTerrainEditsDig(second.edits, &second.terrain, b, SOS_METRES(3), NULL) > 0u &&
               SosTerrainEditsDig(second.edits, &second.terrain, a, SOS_METRES(2), NULL) > 0u,
           "the same digs reversed");
    Expect(SosTerrainEditsHash(first.edits) == SosTerrainEditsHash(second.edits),
           "the order of digs does not change the ground");
    SosTerrainEditsDestroy(first.edits);
    SosTerrainEditsDestroy(second.edits);
}

static void TestRaycast(void)
{
    Ground ground = CreateGround(3u);
    const SosPosition target = OnSurface(&ground.terrain, SOS_METRES(-30), SOS_METRES(20));
    const SosPosition above = {target.x, target.y, target.z + SOS_METRES(100)};
    const SosPosition down = {0, 0, -1};
    SosPosition hit;
    Expect(SosTerrainRaycast(&ground.terrain, above, down, SOS_METRES(500), &hit),
           "a ray straight down hits the ground");
    Expect(Absolute(hit.z - target.z) < SOS_METRES(1) / 20 && hit.x == target.x,
           "it hits the surface");

    const SosPosition slanted = {SOS_METRES(-40), SOS_METRES(25), -SOS_METRES(100)};
    Expect(SosTerrainRaycast(&ground.terrain, above, slanted, SOS_METRES(500), &hit),
           "a slanted ray hits the ground");
    Expect(Absolute(hit.z - SosTerrainSurfaceZ(&ground.terrain, hit.x, hit.y)) < SOS_METRES(1) / 10,
           "a slanted hit lies on the surface");

    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain, target, SOS_METRES(3), NULL) > 0u,
           "dig under the ray");
    Expect(SosTerrainRaycast(&ground.terrain, above, down, SOS_METRES(500), &hit),
           "the ray finds the pit");
    Expect(Absolute(hit.z - (target.z - SOS_METRES(3))) < SOS_METRES(1) / 4,
           "the ray reaches the pit floor");

    const SosPosition up = {0, 0, 1};
    const SosPosition none = {0, 0, 0};
    Expect(!SosTerrainRaycast(&ground.terrain, above, up, SOS_METRES(500), &hit),
           "a ray to the sky misses");
    Expect(!SosTerrainRaycast(&ground.terrain, above, none, SOS_METRES(500), &hit),
           "a zero direction is rejected");
    Expect(!SosTerrainRaycast(&ground.terrain, above, down, SOS_METRES(50), &hit),
           "a short ray stops in the air");
    const SosPosition buried = {target.x, target.y, target.z - SOS_METRES(10)};
    Expect(SosTerrainRaycast(&ground.terrain, buried, down, SOS_METRES(5), &hit) &&
               hit.z == buried.z,
           "a ray starting in rock hits at once");
    SosTerrainEditsDestroy(ground.edits);
}

static void TestLimitsAndGolden(void)
{
    Ground ground = CreateGround(1u);
    const SosPosition remote = {0, INT64_C(1) << 50, 0};
    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain, remote, SOS_METRES(2), NULL) == 0u,
           "ground beyond the edit range cannot be dug");
    Expect(SosTerrainEditsDig(NULL, &ground.terrain, remote, SOS_METRES(2), NULL) == 0u &&
               SosTerrainEditsDig(ground.edits, NULL, remote, SOS_METRES(2), NULL) == 0u,
           "missing objects dig nothing");
    SosTerrain stranger;
    SosTerrainInit(&stranger, 1u);
    Expect(SosTerrainEditsDig(ground.edits, &stranger, OnSurface(&stranger, 0, 0), SOS_METRES(2),
                              NULL) == 0u,
           "a terrain that does not see the edits cannot dig into them");
    const SosPosition sky = {0, 0, SOS_METRES(5000)};
    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain, sky, SOS_METRES(2), NULL) == 0u,
           "digging the air changes nothing");

    Expect(SosTerrainEditsDig(ground.edits, &ground.terrain, OnSurface(&ground.terrain, 0, 0),
                              SOS_METRES(100), NULL) > 0u,
           "a huge radius is clamped, not refused");
    /* Pinned: the exact ground a clamped dig leaves on seed 1. */
    Expect(SosTerrainEditsHash(ground.edits) == UINT64_C(0x6B313E72C8242540),
           "the dug ground matches its golden hash");
    SosTerrainEditsDestroy(ground.edits);
    SosTerrainEditsDestroy(NULL);
    Expect(SosTerrainEditsChunkCount(NULL) == 0u && SosTerrainEditsHash(NULL) == 0u,
           "NULL edits are empty");
}

int main(void)
{
    TestUntouchedGroundIsTheFunction();
    TestDigging();
    TestOrderDoesNotMatter();
    TestRaycast();
    TestLimitsAndGolden();
    return 0;
}
