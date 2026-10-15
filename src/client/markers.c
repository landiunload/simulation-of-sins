#include "client/markers.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#define BOX_VERTICES 36u
#define INSTANCE_BATCH 1024u

typedef struct MarkerMesh
{
    LaiueGraphicsHandle buffer;
    uint32_t pending;
    LaiueGraphicsInstanceV2 *instances;
} MarkerMesh;

struct SosMarkers
{
    LaiueGraphicsDeviceV2 *device;
    bool instanced;
    uint32_t kindCount;
    MarkerMesh meshes[];
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
static void BuildBox(const SosMarkerKind *kind, LaiueGraphicsVertexV2 vertices[BOX_VERTICES])
{
    const float x[2] = {-kind->halfWidth, kind->halfWidth};
    const float y[2] = {-kind->halfDepth, kind->halfDepth};
    const float z[2] = {0.0f, kind->height};
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
        const uint32_t packed = Shade(kind->color, faces[face].light);
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

SosMarkers *SosMarkersCreate(LaiueGraphicsDeviceV2 *device, const SosMarkerKind *kinds,
                             uint32_t kindCount)
{
    if (device == NULL || kinds == NULL || kindCount == 0u || device->createBuffer == NULL ||
        device->uploadBuffer == NULL || device->destroyHandle == NULL || device->submit == NULL)
    {
        return NULL;
    }
    SosMarkers *markers = calloc(1u, sizeof(*markers) + (size_t)kindCount * sizeof(MarkerMesh));
    if (markers == NULL)
    {
        return NULL;
    }
    markers->device = device;
    markers->kindCount = kindCount;
    markers->instanced =
        DeviceHas(device, offsetof(LaiueGraphicsDeviceV2, submitInstances),
                  sizeof(device->submitInstances)) &&
        device->getCapabilities != NULL && device->submitInstances != NULL &&
        (device->getCapabilities(device) & LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES) != 0u;
    for (uint32_t kind = 0u; kind < kindCount; ++kind)
    {
        MarkerMesh *mesh = &markers->meshes[kind];
        if (markers->instanced)
        {
            mesh->instances = malloc(INSTANCE_BATCH * sizeof(*mesh->instances));
            if (mesh->instances == NULL)
            {
                SosMarkersDestroy(markers);
                return NULL;
            }
        }
        LaiueGraphicsVertexV2 vertices[BOX_VERTICES];
        BuildBox(&kinds[kind], vertices);
        const LaiueGraphicsBufferDescV1 description = {
            .structSize = sizeof(description),
            .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
            .sizeBytes = sizeof(vertices),
        };
        if (device->createBuffer(device, &description, &mesh->buffer) == 0u)
        {
            SosMarkersDestroy(markers);
            return NULL;
        }
        const LaiueGraphicsBufferUploadV1 upload = {
            .structSize = sizeof(upload),
            .buffer = mesh->buffer,
            .data = vertices,
            .sizeBytes = sizeof(vertices),
        };
        if (device->uploadBuffer(device, &upload) == 0u)
        {
            SosMarkersDestroy(markers);
            return NULL;
        }
    }
    return markers;
}

void SosMarkersDestroy(SosMarkers *markers)
{
    if (markers == NULL)
    {
        return;
    }
    for (uint32_t kind = 0u; kind < markers->kindCount; ++kind)
    {
        MarkerMesh *mesh = &markers->meshes[kind];
        if (mesh->buffer != 0u)
        {
            markers->device->destroyHandle(markers->device, mesh->buffer);
        }
        free(mesh->instances);
    }
    free(markers);
}

static void FlushKind(SosMarkers *markers, MarkerMesh *mesh)
{
    if (mesh->pending == 0u)
    {
        return;
    }
    const LaiueGraphicsDrawItemV2 draw = {
        .structSize = LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE,
        .vertexBuffer = mesh->buffer,
        .indexCount = BOX_VERTICES,
        .scale = 1.0f,
    };
    (void)markers->device->submitInstances(markers->device, &draw, mesh->instances, mesh->pending);
    mesh->pending = 0u;
}

void SosMarkersAdd(SosMarkers *markers, uint32_t kind, const float origin[3], float scale)
{
    if (markers == NULL || kind >= markers->kindCount)
    {
        return;
    }
    MarkerMesh *mesh = &markers->meshes[kind];
    if (!markers->instanced)
    {
        const LaiueGraphicsDrawItemV2 draw = {
            .structSize = LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE,
            .vertexBuffer = mesh->buffer,
            .indexCount = BOX_VERTICES,
            .originRelative = {origin[0], origin[1], origin[2]},
            .scale = scale,
        };
        (void)markers->device->submit(markers->device, &draw, 1u);
        return;
    }
    mesh->instances[mesh->pending++] = (LaiueGraphicsInstanceV2){
        .originRelative = {origin[0], origin[1], origin[2]},
        .scale = scale,
    };
    if (mesh->pending == INSTANCE_BATCH)
    {
        FlushKind(markers, mesh);
    }
}

void SosMarkersFlush(SosMarkers *markers)
{
    if (markers == NULL || !markers->instanced)
    {
        return;
    }
    for (uint32_t kind = 0u; kind < markers->kindCount; ++kind)
    {
        FlushKind(markers, &markers->meshes[kind]);
    }
}
