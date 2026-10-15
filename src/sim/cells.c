#include "sim/cells.h"

#include <stddef.h>

/* |a - b| exactly, even across the whole 64-bit range: subtracting the larger
 * from the smaller in unsigned arithmetic cannot overflow. */
static uint64_t AxisDistance(int64_t a, int64_t b)
{
    return a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
}

int64_t SosCellsIndex(int64_t coordinate, unsigned shift)
{
    return SosFloorShift(coordinate, shift > SOS_CELLS_MAX_SHIFT ? SOS_CELLS_MAX_SHIFT : shift);
}

uint64_t SosCellsSquared(uint64_t value)
{
    return value > (uint64_t)UINT32_MAX ? UINT64_MAX : value * value;
}

uint64_t SosCellsGroundDistanceSquared(SosPosition from, SosPosition to)
{
    const uint64_t dx = SosCellsSquared(AxisDistance(to.x, from.x));
    const uint64_t dy = SosCellsSquared(AxisDistance(to.y, from.y));
    return dx > UINT64_MAX - dy ? UINT64_MAX : dx + dy;
}

bool SosCellsVisitRings(SosPosition around, int64_t radius, unsigned shift, uint32_t maxCells,
                        uint64_t *best, SosCellsVisitor visit, void *context)
{
    if (visit == NULL || best == NULL || radius < 0 || shift == 0u || shift > SOS_CELLS_MAX_SHIFT ||
        maxCells < 4u)
    {
        return false;
    }
    const int64_t cellUnits = INT64_C(1) << shift;
    /* One ring at worst covers half the bound per side; ring 0 is the centre,
     * so the bound allows this many rings beyond it. */
    const int64_t ringsPerSide = (int64_t)(maxCells / 2u) - 1;
    if (cellUnits > INT64_MAX / ringsPerSide)
    {
        return false;
    }
    const int64_t maxRadius = ringsPerSide * cellUnits;
    if (radius > maxRadius)
    {
        return false;
    }
    const uint64_t radiusSquared = SosCellsSquared((uint64_t)radius);
    const int64_t rings = radius / cellUnits + 1;
    const int64_t centerX = SosCellsIndex(around.x, shift);
    const int64_t centerY = SosCellsIndex(around.y, shift);
    for (int64_t ring = 0; ring <= rings; ++ring)
    {
        /* The nearest a cell of this ring can be is (ring - 1) cells away; once
         * that cannot beat the best hit so far, so can no later ring. */
        const uint64_t gap = (uint64_t)(ring - 1) * (uint64_t)cellUnits;
        if (ring > 1 && SosCellsSquared(gap) > (*best < radiusSquared ? *best : radiusSquared))
        {
            break;
        }
        for (int64_t dy = -ring; dy <= ring; ++dy)
        {
            /* Edge rows are walked cell by cell; an inner row only has its two
             * ends, since the rest of it was covered by the ring before. */
            const bool edgeRow = dy == -ring || dy == ring;
            for (int64_t dx = -ring; dx <= ring; dx += edgeRow ? 1 : 2 * ring)
            {
                visit(centerX + dx, centerY + dy, context);
            }
        }
    }
    return true;
}

bool SosCellsVisitBox(int64_t minX, int64_t minY, int64_t maxX, int64_t maxY, unsigned shift,
                      uint32_t maxCells, SosCellsVisitor visit, void *context)
{
    if (visit == NULL || maxCells == 0u || maxX < minX || maxY < minY)
    {
        return false;
    }
    const int64_t firstX = SosCellsIndex(minX, shift);
    const int64_t lastX = SosCellsIndex(maxX, shift);
    const int64_t firstY = SosCellsIndex(minY, shift);
    const int64_t lastY = SosCellsIndex(maxY, shift);
    /* The span is measured unsigned so a box across the whole world cannot
     * overflow the difference of its end cells. */
    if ((uint64_t)lastX - (uint64_t)firstX >= maxCells ||
        (uint64_t)lastY - (uint64_t)firstY >= maxCells)
    {
        return false;
    }
    for (int64_t cellY = firstY; cellY <= lastY; ++cellY)
    {
        for (int64_t cellX = firstX; cellX <= lastX; ++cellX)
        {
            visit(cellX, cellY, context);
        }
    }
    return true;
}
