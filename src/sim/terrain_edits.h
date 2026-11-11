#ifndef SOS_SIM_TERRAIN_EDITS_H
#define SOS_SIM_TERRAIN_EDITS_H

#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Changes to the ground.
 *
 * The terrain is a function of the seed; what beings and the god dig away is
 * a sparse layer over it, the pattern every changeable part of the world
 * follows. An edit replaces the density and material of one 0.5 m lattice
 * sample. Edits are stored in chunks of 8 x 8 x 8 samples (4 m cubes), so a
 * dug pit costs a few chunks and the untouched unbounded world costs nothing.
 * A terrain sees the edits once its `edits` field points at them: sampling,
 * the surface beings stand on and raycasts then include every change.
 *
 * Edits are kept within SOS_TERRAIN_EDITS_MAX_CHUNK chunks of the origin
 * along Y and Z (digs within about 4 million km); along X they are
 * unbounded. Further away the ground cannot be changed. */

#define SOS_TERRAIN_EDITS_CHUNK_SHIFT 3u
#define SOS_TERRAIN_EDITS_CHUNK_CELLS (1 << SOS_TERRAIN_EDITS_CHUNK_SHIFT)
#define SOS_TERRAIN_EDITS_MAX_CHUNK (INT64_C(1) << 31)
/* A single dig removes a sphere of this radius at most. */
#define SOS_TERRAIN_DIG_MIN_RADIUS (SOS_METRES(1) / 2)
#define SOS_TERRAIN_DIG_MAX_RADIUS SOS_METRES(16)

/* A box in world units that a change touched; empty when min > max. */
typedef struct SosTerrainBox
{
    SosPosition min;
    SosPosition max;
} SosTerrainBox;

SosTerrainEdits *SosTerrainEditsCreate(void);
void SosTerrainEditsDestroy(SosTerrainEdits *edits);

/* Number of edited chunks. */
uint32_t SosTerrainEditsChunkCount(const SosTerrainEdits *edits);

/* The edited sample at a lattice cell; false when the function's own sample
 * stands there. */
bool SosTerrainEditsSample(const SosTerrainEdits *edits, int64_t cellX, int64_t cellY,
                           int64_t cellZ, SosTerrainSample *outSample);

/* Whether any chunk in the column of chunks holding the lattice cell (cellX,
 * cellY) is edited, and the lowest and highest edited chunk there. The fast
 * test that keeps untouched ground at the cost of the plain function. */
bool SosTerrainEditsColumn(const SosTerrainEdits *edits, int64_t cellX, int64_t cellY,
                           int64_t *outLowestChunkZ, int64_t *outHighestChunkZ);

/* Removes a sphere of ground around `center`: every lattice sample whose
 * distance to the sphere is less than its density becomes the sphere's
 * signed distance, air inside and a smooth wall around it. The radius is
 * clamped to [SOS_TERRAIN_DIG_MIN_RADIUS, SOS_TERRAIN_DIG_MAX_RADIUS].
 * `terrain` supplies the ground before the dig and must see these edits.
 * Returns how many samples changed and the box they span (may be NULL).
 * Zero when nothing was solid there, the place is out of range, or memory
 * ran out; in that case nothing changed. */
uint32_t SosTerrainEditsDig(SosTerrainEdits *edits, const SosTerrain *terrain, SosPosition center,
                            int64_t radius, SosTerrainBox *outChanged);

/* Folds every edit into one value, independent of the order of digs that
 * produced the same ground; zero without edits. */
uint64_t SosTerrainEditsHash(const SosTerrainEdits *edits);

#endif
