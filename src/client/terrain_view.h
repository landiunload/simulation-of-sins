#ifndef SOS_CLIENT_TERRAIN_VIEW_H
#define SOS_CLIENT_TERRAIN_VIEW_H

#include "client/camera.h"
#include "graphics/graphics_device_v2.h"
#include "mesh/mesher_service.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* The visible terrain. The ground is divided into square columns on a
 * quadtree of detail levels: a column of level L spans 32 lattice samples
 * spaced 2^L cells apart, so the column under the camera shows half-metre
 * detail and the ones on the horizon span kilometres. Each column is meshed
 * from the terrain density field on demand, a few per frame and nearest
 * first, and kept while it stays in use. */

typedef struct SosTerrainView SosTerrainView;

typedef struct SosTerrainViewStats
{
    uint32_t visibleColumns;
    uint32_t drawnColumns;
    uint32_t cachedColumns;
    uint32_t builtThisFrame;
} SosTerrainViewStats;

SosTerrainView *SosTerrainViewCreate(LaiueGraphicsDeviceV2 *device,
                                     const LaiueMesherServiceV1 *mesher,
                                     uint32_t mesherServiceSize);
/* Releases every GPU resource; call between frames. */
void SosTerrainViewDestroy(SosTerrainView *view);

/* Chooses the columns for this camera and meshes missing ones within the
 * frame budget. Call before the frame begins: uploads happen here. */
void SosTerrainViewUpdate(SosTerrainView *view, const SosTerrain *terrain,
                          const SosCameraView *camera);

/* Submits the chosen columns inside the current frame. */
void SosTerrainViewDraw(SosTerrainView *view, const SosCameraView *camera);

SosTerrainViewStats SosTerrainViewGetStats(const SosTerrainView *view);

#endif
