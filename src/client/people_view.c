#include "client/people_view.h"

#include "sim/calendar.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FIGURE_VERTICES 36u
#define FIGURE_HALF_WIDTH 0.22f
#define FIGURE_HALF_DEPTH 0.15f
#define FIGURE_HEIGHT 1.75f
/* Beyond this distance a figure grows linearly with it. */
#define FIGURE_TRUE_SIZE_DISTANCE 120.0
#define INSTANCE_BATCH 1024u

struct SosPeopleView
{
    LaiueGraphicsDeviceV2 *device;
    LaiueGraphicsHandle figures[2];
    bool instanced;
    LaiueGraphicsInstanceV2 instances[2][INSTANCE_BATCH];
};

static uint32_t Shade(const float color[3], float light)
{
    uint32_t packed = UINT32_C(0xFF000000);
    for (uint32_t channel = 0u; channel < 3u; ++channel)
    {
        float value = color[channel] * light;
        value = value > 1.0f ? 1.0f : value;
        packed |= (uint32_t)lrintf(value * 255.0f) << (8u * channel);
    }
    return packed;
}

/* A box standing on its base at the origin, faces lit by a fixed sun. */
static void BuildFigure(const float color[3], LaiueGraphicsVertexV2 vertices[FIGURE_VERTICES])
{
    const float x[2] = {-FIGURE_HALF_WIDTH, FIGURE_HALF_WIDTH};
    const float y[2] = {-FIGURE_HALF_DEPTH, FIGURE_HALF_DEPTH};
    const float z[2] = {0.0f, FIGURE_HEIGHT};
    /* Each face: four corners as (x, y, z) bit indices, counter-clockwise from
     * outside, and its brightness. */
    static const struct
    {
        uint8_t corners[4];
        float light;
    } faces[6] = {
        {{0, 1, 3, 2}, 0.45f}, /* bottom */
        {{4, 6, 7, 5}, 1.00f}, /* top */
        {{0, 4, 5, 1}, 0.70f}, /* -y */
        {{2, 3, 7, 6}, 0.55f}, /* +y */
        {{0, 2, 6, 4}, 0.60f}, /* -x */
        {{1, 5, 7, 3}, 0.85f}, /* +x */
    };
    static const uint8_t triangle[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    uint32_t written = 0u;
    for (uint32_t face = 0u; face < 6u; ++face)
    {
        const uint32_t packed = Shade(color, faces[face].light);
        for (uint32_t index = 0u; index < 6u; ++index)
        {
            const uint8_t corner = faces[face].corners[triangle[index]];
            vertices[written++] = (LaiueGraphicsVertexV2){
                .position = {x[corner & 1u], y[(corner >> 1) & 1u], z[(corner >> 2) & 1u]},
                .colorRGBA = packed,
            };
        }
    }
}

static bool DeviceHas(const LaiueGraphicsDeviceV2 *device, size_t offset, size_t size)
{
    return (size_t)device->structSize >= offset + size;
}

SosPeopleView *SosPeopleViewCreate(LaiueGraphicsDeviceV2 *device)
{
    if (device == NULL || device->createBuffer == NULL || device->uploadBuffer == NULL ||
        device->destroyHandle == NULL || device->submit == NULL)
    {
        return NULL;
    }
    SosPeopleView *view = calloc(1u, sizeof(*view));
    if (view == NULL)
    {
        return NULL;
    }
    view->device = device;
    view->instanced =
        DeviceHas(device, offsetof(LaiueGraphicsDeviceV2, submitInstances),
                  sizeof(device->submitInstances)) &&
        device->getCapabilities != NULL && device->submitInstances != NULL &&
        (device->getCapabilities(device) & LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES) != 0u;
    static const float colors[2][3] = {
        [SOS_SEX_FEMALE] = {0.86f, 0.25f, 0.22f},
        [SOS_SEX_MALE] = {0.22f, 0.42f, 0.86f},
    };
    for (uint32_t sex = 0u; sex < 2u; ++sex)
    {
        LaiueGraphicsVertexV2 vertices[FIGURE_VERTICES];
        BuildFigure(colors[sex], vertices);
        const LaiueGraphicsBufferDescV1 description = {
            .structSize = sizeof(description),
            .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
            .sizeBytes = sizeof(vertices),
        };
        if (device->createBuffer(device, &description, &view->figures[sex]) == 0u)
        {
            SosPeopleViewDestroy(view);
            return NULL;
        }
        const LaiueGraphicsBufferUploadV1 upload = {
            .structSize = sizeof(upload),
            .buffer = view->figures[sex],
            .data = vertices,
            .sizeBytes = sizeof(vertices),
        };
        if (device->uploadBuffer(device, &upload) == 0u)
        {
            SosPeopleViewDestroy(view);
            return NULL;
        }
    }
    return view;
}

void SosPeopleViewDestroy(SosPeopleView *view)
{
    if (view == NULL)
    {
        return;
    }
    for (uint32_t sex = 0u; sex < 2u; ++sex)
    {
        if (view->figures[sex] != 0u)
        {
            view->device->destroyHandle(view->device, view->figures[sex]);
        }
    }
    free(view);
}

static void Flush(SosPeopleView *view, uint32_t sex, uint32_t count)
{
    if (count == 0u)
    {
        return;
    }
    const LaiueGraphicsDrawItemV2 draw = {
        .structSize = LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE,
        .vertexBuffer = view->figures[sex],
        .indexCount = FIGURE_VERTICES,
        .scale = 1.0f,
    };
    (void)view->device->submitInstances(view->device, &draw, view->instances[sex], count);
}

uint32_t SosPeopleViewDraw(SosPeopleView *view, const SosBeings *beings, uint64_t tick,
                           const SosCameraView *camera)
{
    if (view == NULL || beings == NULL || camera == NULL)
    {
        return 0u;
    }
    uint32_t pending[2] = {0u, 0u};
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
        const float scale = (float)(growth * visibility);
        const uint32_t sex = being.sex == SOS_SEX_MALE ? 1u : 0u;
        if (view->instanced)
        {
            view->instances[sex][pending[sex]++] = (LaiueGraphicsInstanceV2){
                .originRelative = {origin[0], origin[1], origin[2]},
                .scale = scale,
            };
            if (pending[sex] == INSTANCE_BATCH)
            {
                Flush(view, sex, pending[sex]);
                pending[sex] = 0u;
            }
        }
        else
        {
            const LaiueGraphicsDrawItemV2 draw = {
                .structSize = LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE,
                .vertexBuffer = view->figures[sex],
                .indexCount = FIGURE_VERTICES,
                .originRelative = {origin[0], origin[1], origin[2]},
                .scale = scale,
            };
            (void)view->device->submit(view->device, &draw, 1u);
        }
        ++drawn;
    }
    for (uint32_t sex = 0u; sex < 2u; ++sex)
    {
        if (view->instanced)
        {
            Flush(view, sex, pending[sex]);
        }
    }
    return drawn;
}
