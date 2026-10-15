#ifndef SOS_CLIENT_PLANTS_VIEW_H
#define SOS_CLIENT_PLANTS_VIEW_H

#include "client/camera.h"
#include "graphics/graphics_device_v2.h"
#include "sim/plants.h"
#include "sim/terrain.h"

#include <stdint.h>

/* Mock-up plants: a berry bush is a low green box, brown once eaten bare.
 * Plants are small and countless, so only those near the eye are drawn. */

typedef struct SosPlantsView SosPlantsView;

SosPlantsView *SosPlantsViewCreate(LaiueGraphicsDeviceV2 *device);
void SosPlantsViewDestroy(SosPlantsView *view);
/* Submits the plants near the eye inside the current frame; returns how
 * many. */
uint32_t SosPlantsViewDraw(SosPlantsView *view, const SosPlants *plants, const SosTerrain *terrain,
                           uint64_t tick, const SosCameraView *camera);

#endif
