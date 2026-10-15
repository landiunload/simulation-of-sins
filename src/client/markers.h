#ifndef SOS_CLIENT_MARKERS_H
#define SOS_CLIENT_MARKERS_H

#include "graphics/graphics_device_v2.h"

#include <stdbool.h>
#include <stdint.h>

/* Mock-up markers: lit boxes standing on their base, one mesh per kind,
 * drawn many at a time. People, plants and later animals and buildings share
 * them until real models replace them. Instanced drawing is used when the
 * device offers it, single draws otherwise. */

typedef struct SosMarkerKind
{
    float halfWidth;
    float halfDepth;
    float height;
    float color[3];
} SosMarkerKind;

typedef struct SosMarkers SosMarkers;

/* NULL when the device lacks buffers or drawing, or memory runs out. */
SosMarkers *SosMarkersCreate(LaiueGraphicsDeviceV2 *device, const SosMarkerKind *kinds,
                             uint32_t kindCount);
void SosMarkersDestroy(SosMarkers *markers);
/* Queues one marker inside the current frame; `origin` is relative to the
 * render origin and `scale` multiplies its size. */
void SosMarkersAdd(SosMarkers *markers, uint32_t kind, const float origin[3], float scale);
/* Submits every queued marker. */
void SosMarkersFlush(SosMarkers *markers);

#endif
