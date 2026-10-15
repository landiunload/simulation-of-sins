#ifndef SOS_CLIENT_PEOPLE_VIEW_H
#define SOS_CLIENT_PEOPLE_VIEW_H

#include "client/camera.h"
#include "graphics/graphics_device_v2.h"
#include "sim/beings.h"

#include <stdint.h>

/* Mock-up figures for the living: a column the height of a person, red for
 * women and blue for men, smaller for children. Far away a figure grows so
 * that people stay visible from kilometres up. */

typedef struct SosPeopleView SosPeopleView;

SosPeopleView *SosPeopleViewCreate(LaiueGraphicsDeviceV2 *device);
void SosPeopleViewDestroy(SosPeopleView *view);
/* Submits every living being inside the current frame; returns how many. */
uint32_t SosPeopleViewDraw(SosPeopleView *view, const SosBeings *beings, uint64_t tick,
                           const SosCameraView *camera);

#endif
