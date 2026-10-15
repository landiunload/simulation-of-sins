#ifndef SOS_SIM_SPATIAL_H
#define SOS_SIM_SPATIAL_H

#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Spatial hash for neighbour queries among millions of moving beings.
 *
 * Beings move every step and each cares only about the few near it, so the
 * world needs a bucket grid that is cheap to rebuild from scratch. A uniform
 * grid of 2^cellShift-unit cells covers the unbounded integer world: the cell
 * of a position is its floor division by the cell size, so no world size is
 * stored and coordinates near the 64-bit limits stay finite. Cell heads live
 * in one SosTable keyed by the cell, while the entries themselves live in a
 * single pooled array linked per cell, so a dense population costs one
 * allocation and never one per cell. The intended use is clear plus insert-all
 * every step, when every position has changed.
 *
 * Query results never depend on the order the pool was built in: candidates
 * are ranked by ground distance and then by serial, so every machine sees the
 * same nearest-first answer. Distances are measured on the XY ground plane,
 * ignoring Z, and saturate rather than overflow, so positions near INT64_MIN
 * and INT64_MAX are safe. */

/* Largest accepted cell shift. Cells are at least one world unit wide and a
 * shift this large keeps every cell coordinate inside signed 64-bit range. */
#define SOS_SPATIAL_MAX_CELL_SHIFT 62u

typedef struct SosSpatial SosSpatial;

/* A grid of cells 2^cellShift world units per side, or NULL when cellShift
 * exceeds SOS_SPATIAL_MAX_CELL_SHIFT or memory ran out. `capacityHint` is a
 * best-effort preallocation of the entry pool and cell table; failing to
 * allocate it just leaves the index to grow on demand. Shift 0 makes one-unit
 * cells, larger shifts make bigger ones; a cell of about a metre is typical. */
SosSpatial *SosSpatialCreate(unsigned cellShift, uint32_t capacityHint);
void SosSpatialDestroy(SosSpatial *spatial);

/* Forgets every being but keeps the allocation for the next step. */
void SosSpatialClear(SosSpatial *spatial);

/* Adds a being at a position; a serial of 0 (nobody) is rejected. The same
 * serial may be inserted more than once and every copy is found, so the caller
 * must not insert a being twice in one step. False when the arguments are
 * invalid or memory ran out, leaving the previous contents unchanged. */
bool SosSpatialInsert(SosSpatial *spatial, uint64_t serial, SosPosition position);

/* The serials within `radius` of `center` on the ground plane, nearest first
 * and ties by serial. Writes up to `capacity` of them and returns how many
 * there are, which may exceed `capacity`; a truncated result keeps the
 * nearest. `outSerials` may be NULL only when `capacity` is 0. A negative
 * radius, a NULL index or a NULL `outSerials` with non-zero capacity returns
 * 0. */
uint32_t SosSpatialQueryRadius(const SosSpatial *spatial, SosPosition center, int64_t radius,
                               uint64_t *outSerials, uint32_t capacity);

/* The nearest serial within `maxRadius` of `center`, skipping `excludeSerial`
 * (0 skips nobody) and breaking ties by serial. False when there is none or
 * the arguments are invalid, in which case `outSerial` is left untouched. */
bool SosSpatialNearest(const SosSpatial *spatial, SosPosition center, int64_t maxRadius,
                       uint64_t excludeSerial, uint64_t *outSerial);

/* How many beings lie in the ground-plane box [minX, maxX] x [minY, maxY],
 * both inclusive. A box with maxX < minX or maxY < minY, or a NULL index,
 * counts none. Empty cells are skipped when that is cheaper than walking the
 * whole pool, so a box over the whole world costs O(beings), not O(cells). */
uint32_t SosSpatialCountInBox(const SosSpatial *spatial, int64_t minX, int64_t minY, int64_t maxX,
                              int64_t maxY);

/* How many beings the index holds. */
uint32_t SosSpatialCount(const SosSpatial *spatial);

/* Folds every being into one value independently of insertion order, so
 * replays and machines can be compared. A NULL index folds to 0. */
uint64_t SosSpatialHash(const SosSpatial *spatial);

#endif
