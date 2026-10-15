#include "client/people_view.h"

#include "client/markers.h"
#include "sim/calendar.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

/* Beyond this distance a figure grows linearly with it. */
#define FIGURE_TRUE_SIZE_DISTANCE 120.0

struct SosPeopleView
{
    SosMarkers *figures;
};

SosPeopleView *SosPeopleViewCreate(LaiueGraphicsDeviceV2 *device)
{
    /* A column the height of a person, indexed by SosSex. */
    static const SosMarkerKind kinds[2] = {
        [SOS_SEX_FEMALE] = {0.22f, 0.15f, 1.75f, {0.86f, 0.25f, 0.22f}},
        [SOS_SEX_MALE] = {0.22f, 0.15f, 1.75f, {0.22f, 0.42f, 0.86f}},
    };
    SosMarkers *figures = SosMarkersCreate(device, kinds, 2u);
    if (figures == NULL)
    {
        return NULL;
    }
    SosPeopleView *view = calloc(1u, sizeof(*view));
    if (view == NULL)
    {
        SosMarkersDestroy(figures);
        return NULL;
    }
    view->figures = figures;
    return view;
}

void SosPeopleViewDestroy(SosPeopleView *view)
{
    if (view != NULL)
    {
        SosMarkersDestroy(view->figures);
        free(view);
    }
}

uint32_t SosPeopleViewDraw(SosPeopleView *view, const SosBeings *beings, uint64_t tick,
                           const SosCameraView *camera)
{
    if (view == NULL || beings == NULL || camera == NULL)
    {
        return 0u;
    }
    uint32_t drawn = 0u;
    for (uint32_t slot = 0u; slot < SosBeingsSlotCount(beings); ++slot)
    {
        SosBeingView being;
        if (!SosBeingsView(beings, slot, &being))
        {
            continue;
        }
        const float origin[3] = {
            (float)((double)(being.position.x - camera->renderOrigin.x) / SOS_UNITS_PER_METRE),
            (float)((double)(being.position.y - camera->renderOrigin.y) / SOS_UNITS_PER_METRE),
            (float)((double)(being.position.z - camera->renderOrigin.z) / SOS_UNITS_PER_METRE),
        };
        const double dx = (double)origin[0] - camera->eye[0];
        const double dy = (double)origin[1] - camera->eye[1];
        const double dz = (double)origin[2] - camera->eye[2];
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        /* Children grow from half height at birth to full height at sixteen. */
        const double age = (double)((int64_t)tick - being.birthTick) / (double)SOS_TICKS_PER_YEAR;
        const double growth = age >= 16.0 ? 1.0 : 0.5 + 0.5 * (age < 0.0 ? 0.0 : age) / 16.0;
        const double visibility = fmax(1.0, distance / FIGURE_TRUE_SIZE_DISTANCE);
        SosMarkersAdd(view->figures, being.sex == SOS_SEX_MALE ? 1u : 0u, origin,
                      (float)(growth * visibility));
        ++drawn;
    }
    SosMarkersFlush(view->figures);
    return drawn;
}
