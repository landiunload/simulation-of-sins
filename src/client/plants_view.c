#include "client/plants_view.h"

#include "client/markers.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define DRAW_RADIUS_METRES 150
#define DRAW_RADIUS ((double)DRAW_RADIUS_METRES)
/* Cells a query box spans per side, with one more for its unaligned edges. */
#define DRAW_CELLS ((uint32_t)(2 * SOS_METRES(DRAW_RADIUS_METRES) / SOS_PLANT_CELL_UNITS) + 2u)

/* Two kinds per species: bearing food and bare. */
#define KIND_COUNT (2u * (SOS_PLANT_SPECIES_COUNT - 1u))

struct SosPlantsView
{
    SosMarkers *markers;
    SosPlant found[DRAW_CELLS * DRAW_CELLS];
};

SosPlantsView *SosPlantsViewCreate(LaiueGraphicsDeviceV2 *device)
{
    static const SosMarkerKind kinds[KIND_COUNT] = {
        /* Berry bush. */
        {0.45f, 0.45f, 0.9f, {0.10f, 0.34f, 0.10f}},
        {0.45f, 0.45f, 0.9f, {0.45f, 0.36f, 0.24f}},
    };
    SosMarkers *markers = SosMarkersCreate(device, kinds, KIND_COUNT);
    if (markers == NULL)
    {
        return NULL;
    }
    SosPlantsView *view = calloc(1u, sizeof(*view));
    if (view == NULL)
    {
        SosMarkersDestroy(markers);
        return NULL;
    }
    view->markers = markers;
    return view;
}

void SosPlantsViewDestroy(SosPlantsView *view)
{
    if (view != NULL)
    {
        SosMarkersDestroy(view->markers);
        free(view);
    }
}

uint32_t SosPlantsViewDraw(SosPlantsView *view, const SosPlants *plants, const SosTerrain *terrain,
                           uint64_t tick, const SosCameraView *camera)
{
    if (view == NULL || plants == NULL || terrain == NULL || camera == NULL)
    {
        return 0u;
    }
    const SosPosition eye = {
        camera->renderOrigin.x + (int64_t)llround((double)camera->eye[0] * SOS_UNITS_PER_METRE),
        camera->renderOrigin.y + (int64_t)llround((double)camera->eye[1] * SOS_UNITS_PER_METRE),
        camera->renderOrigin.z + (int64_t)llround((double)camera->eye[2] * SOS_UNITS_PER_METRE),
    };
    const int64_t radius = SOS_METRES(DRAW_RADIUS_METRES);
    if (eye.z - SosTerrainSurfaceZ(terrain, eye.x, eye.y) > radius)
    {
        return 0u;
    }
    const uint32_t count =
        SosPlantsQuery(plants, terrain, eye.x - radius, eye.y - radius, eye.x + radius,
                       eye.y + radius, tick, view->found, DRAW_CELLS * DRAW_CELLS);
    uint32_t drawn = 0u;
    for (uint32_t index = 0u; index < count && index < DRAW_CELLS * DRAW_CELLS; ++index)
    {
        const SosPlant *plant = &view->found[index];
        const float origin[3] = {
            (float)((double)(plant->position.x - camera->renderOrigin.x) / SOS_UNITS_PER_METRE),
            (float)((double)(plant->position.y - camera->renderOrigin.y) / SOS_UNITS_PER_METRE),
            (float)((double)(plant->position.z - camera->renderOrigin.z) / SOS_UNITS_PER_METRE),
        };
        const double dx = (double)origin[0] - camera->eye[0];
        const double dy = (double)origin[1] - camera->eye[1];
        const double dz = (double)origin[2] - camera->eye[2];
        if (dx * dx + dy * dy + dz * dz > DRAW_RADIUS * DRAW_RADIUS)
        {
            continue;
        }
        const uint32_t kind = 2u * ((uint32_t)plant->species - 1u) + (plant->ripe ? 0u : 1u);
        SosMarkersAdd(view->markers, kind, origin, 1.0f);
        ++drawn;
    }
    SosMarkersFlush(view->markers);
    return drawn;
}
