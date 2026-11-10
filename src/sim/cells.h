#ifndef SOS_SIM_CELLS_H
#define SOS_SIM_CELLS_H

#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Shared ground-cell geometry.
 *
 * Every layer that covers the ground divides it into a uniform grid of square
 * cells: plants, water, deposits and fauna each own a cell size, and the
 * spatial index buckets moving beings. All of them need the same small pieces
 * of arithmetic, and all of them must agree on them, so that a query visits
 * the same cells and measures the same distances on every machine. This module
 * holds that arithmetic once.
 *
 * A cell is 2^shift world units per side, so a position belongs to the cell
 * holding its floor division by that size; the floor keeps negative
 * coordinates in the cell below and stays finite across the whole signed
 * 64-bit range. Distances are measured on the XY ground plane and saturate
 * rather than overflow, so positions anywhere in the world compare safely.
 * The walk helpers visit the cells of a box or of the rings around a centre in
 * a fixed order, the order nearest-first searches depend on, and stop as soon
 * as the next ring cannot beat the best distance the caller has found. */

/* Largest accepted cell shift: 2^shift stays a positive int64 and a cell's
 * coordinates stay finite. A larger shift is clamped to this one. */
#define SOS_CELLS_MAX_SHIFT 62u

/* The cell coordinate that holds world coordinate `coordinate` at 2^shift
 * units per cell: floor division, so a negative coordinate lies in the cell
 * below. A shift above SOS_CELLS_MAX_SHIFT is clamped to it, so the result is
 * always defined. */
int64_t SosCellsIndex(int64_t coordinate, unsigned shift);

/* `value * value`, saturated at UINT64_MAX: a value whose square does not fit
 * is reported as the farthest possible, which compares as unreachable instead
 * of wrapping around. Zero for zero. */
uint64_t SosCellsSquared(uint64_t value);

/* The squared XY distance between two positions, saturated at UINT64_MAX so no
 * pair of positions can overflow it. Z is ignored: cells live on the ground
 * plane. Equal positions give zero. */
uint64_t SosCellsGroundDistanceSquared(SosPosition from, SosPosition to);

/* Called once per visited cell with the caller's own `context`. */
typedef void (*SosCellsVisitor)(int64_t cellX, int64_t cellY, void *context);

/* Walks the cell rings around `around`, ring 0 (its own cell) first, calling
 * `visit` for every cell of each ring in a fixed order. Every cell of ring r
 * lies at least r - 1 cells from the centre, so a nearest-first search can
 * stop as soon as a whole ring is too far to improve on its best hit. The walk
 * itself lowers nothing: the caller keeps the best squared distance found so
 * far in `*best` (UINT64_MAX while nothing is found, and the visitor lowers
 * it), and the walk stops after the first ring that cannot beat
 * min(*best, radius^2).
 *
 * `radius` is the reach in world units and `maxCells` the cell bound per side
 * the caller allows. Returns false and visits nothing when an argument is
 * NULL, `radius` is negative, `maxCells` is below four, `shift` is zero or
 * above SOS_CELLS_MAX_SHIFT, or `radius` reaches further than (maxCells / 2 -
 * 1) cells; true once the walk ran, whether or not it called `visit`. */
bool SosCellsVisitRings(SosPosition around, int64_t radius, unsigned shift, uint32_t maxCells,
                        uint64_t *best, SosCellsVisitor visit, void *context);

/* Walks every cell that overlaps the box [minX, maxX] x [minY, maxY] at
 * 2^shift units per cell, Y slowest and X fastest, so results come out in the
 * order a box query stores them. Visits nothing and returns false when the box
 * is reversed or spans `maxCells` or more cells on either axis, or when the
 * arguments are otherwise unusable; true once the walk ran. */
bool SosCellsVisitBox(int64_t minX, int64_t minY, int64_t maxX, int64_t maxY, unsigned shift,
                      uint32_t maxCells, SosCellsVisitor visit, void *context);

#endif
