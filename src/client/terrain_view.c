#include "client/terrain_view.h"

#include "app/log.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Lattice samples spanned by one column; meshing adds one sample of overlap
 * on each side so neighbouring columns share their border cells. */
#define COLUMN_CELLS 32u
#define SAMPLES_XY (COLUMN_CELLS + 2u)
#define MAX_SAMPLES_Z 66u
#define MAX_SAMPLES (SAMPLES_XY * SAMPLES_XY * MAX_SAMPLES_Z)
#define MAX_CELLS ((SAMPLES_XY - 1u) * (SAMPLES_XY - 1u) * (MAX_SAMPLES_Z - 1u))
#define MAX_SURFACE_VERTICES MAX_CELLS
#define MAX_SURFACE_INDICES (MAX_CELLS * 6u)
/* The coarsest column spans 65 km; the view reaches a few of them. */
#define MAX_LEVEL 12
/* A column is split while the eye is closer than this many column widths. */
#define SPLIT_FACTOR 2.5
#define MAX_VIEW_RADIUS 200000.0
#define MAX_VISIBLE 2048u
/* Each cached column holds up to two GPU handles; the device has 4096. */
#define CACHE_CAPACITY 1500u
#define HASH_CAPACITY 4096u
#define BUILDS_PER_FRAME 6u
/* A missing leaf is first covered by its ancestor this many levels up. */
#define COVER_LEVELS 3

typedef struct ColumnKey
{
    int32_t level;
    int64_t x;
    int64_t y;
} ColumnKey;

typedef struct Column
{
    ColumnKey key;
    bool used;
    bool empty;
    bool indexed;
    LaiueGraphicsHandle vertexBuffer;
    LaiueGraphicsHandle indexBuffer;
    uint32_t elementCount;
    /* World position of the mesh's local origin. */
    SosPosition origin;
    /* The last frame that needed the column; recent ones are not evicted. */
    uint64_t lastUsedFrame;
    /* The last frame that put it on the draw list. */
    uint64_t drawnFrame;
} Column;

typedef struct Leaf
{
    ColumnKey key;
    double distance;
} Leaf;

struct SosTerrainView
{
    LaiueGraphicsDeviceV2 *device;
    uint32_t (*buildSurfaceMesh)(LaiueSurfaceMeshRequestV1 *request);
    bool nativeIndices;
    uint64_t frame;
    Column columns[CACHE_CAPACITY];
    int32_t hash[HASH_CAPACITY];
    Leaf leaves[MAX_VISIBLE];
    uint32_t leafCount;
    uint32_t drawList[MAX_VISIBLE];
    uint32_t drawCount;
    SosTerrainViewStats stats;
    SosTerrainSample *samples;
    float *values;
    uint8_t *materials;
    uint32_t *cellScratch;
    LaiueSurfaceVertexV1 *surfaceVertices;
    uint32_t *surfaceIndices;
    LaiueGraphicsVertexV2 *vertices;
};

static bool DeviceHas(const LaiueGraphicsDeviceV2 *device, size_t offset, size_t size)
{
    return device != NULL && (size_t)device->structSize >= offset + size;
}

SosTerrainView *SosTerrainViewCreate(LaiueGraphicsDeviceV2 *device,
                                     const LaiueMesherServiceV1 *mesher, uint32_t mesherServiceSize)
{
    if (device == NULL || mesher == NULL ||
        mesherServiceSize < LAIUE_MESHER_SERVICE_V1_SURFACE_SIZE ||
        mesher->structSize < LAIUE_MESHER_SERVICE_V1_SURFACE_SIZE ||
        mesher->buildSurfaceMesh == NULL ||
        !DeviceHas(device, offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                   sizeof(device->destroyHandle)) ||
        device->createBuffer == NULL || device->uploadBuffer == NULL ||
        device->destroyHandle == NULL || device->submit == NULL)
    {
        return NULL;
    }
    SosTerrainView *view = calloc(1u, sizeof(*view));
    if (view == NULL)
    {
        return NULL;
    }
    view->device = device;
    view->buildSurfaceMesh = mesher->buildSurfaceMesh;
    view->nativeIndices =
        DeviceHas(device, offsetof(LaiueGraphicsDeviceV2, getCapabilities),
                  sizeof(device->getCapabilities)) &&
        device->getCapabilities != NULL &&
        (device->getCapabilities(device) & LAIUE_GRAPHICS_CAP_NATIVE_INDICES) != 0u;
    view->samples = malloc((size_t)MAX_SAMPLES * sizeof(*view->samples));
    view->values = malloc((size_t)MAX_SAMPLES * sizeof(*view->values));
    view->materials = malloc((size_t)MAX_SAMPLES * sizeof(*view->materials));
    view->cellScratch = malloc((size_t)MAX_CELLS * sizeof(*view->cellScratch));
    view->surfaceVertices = malloc((size_t)MAX_SURFACE_VERTICES * sizeof(*view->surfaceVertices));
    view->surfaceIndices = malloc((size_t)MAX_SURFACE_INDICES * sizeof(*view->surfaceIndices));
    view->vertices = malloc((size_t)MAX_SURFACE_INDICES * sizeof(*view->vertices));
    if (view->samples == NULL || view->values == NULL || view->materials == NULL ||
        view->cellScratch == NULL || view->surfaceVertices == NULL ||
        view->surfaceIndices == NULL || view->vertices == NULL)
    {
        SosTerrainViewDestroy(view);
        return NULL;
    }
    return view;
}

static void ReleaseColumn(SosTerrainView *view, Column *column)
{
    if (column->vertexBuffer != 0u)
    {
        view->device->destroyHandle(view->device, column->vertexBuffer);
    }
    if (column->indexBuffer != 0u)
    {
        view->device->destroyHandle(view->device, column->indexBuffer);
    }
    memset(column, 0, sizeof(*column));
}

void SosTerrainViewDestroy(SosTerrainView *view)
{
    if (view == NULL)
    {
        return;
    }
    for (uint32_t index = 0u; index < CACHE_CAPACITY; ++index)
    {
        if (view->columns[index].used)
        {
            ReleaseColumn(view, &view->columns[index]);
        }
    }
    free(view->samples);
    free(view->values);
    free(view->materials);
    free(view->cellScratch);
    free(view->surfaceVertices);
    free(view->surfaceIndices);
    free(view->vertices);
    free(view);
}

static uint32_t HashKey(ColumnKey key)
{
    uint64_t value = (uint64_t)key.x * UINT64_C(0x9E3779B97F4A7C15);
    value ^= (uint64_t)key.y * UINT64_C(0xC2B2AE3D27D4EB4F);
    value ^= (uint64_t)(uint32_t)key.level * UINT64_C(0x165667B19E3779F9);
    value ^= value >> 29;
    return (uint32_t)value & (HASH_CAPACITY - 1u);
}

static bool SameKey(ColumnKey a, ColumnKey b)
{
    return a.level == b.level && a.x == b.x && a.y == b.y;
}

/* The table is rebuilt from the cache every frame: with at most 1500 columns
 * that is cheaper than maintaining deletions. */
static void RebuildHash(SosTerrainView *view)
{
    for (uint32_t slot = 0u; slot < HASH_CAPACITY; ++slot)
    {
        view->hash[slot] = -1;
    }
    for (uint32_t index = 0u; index < CACHE_CAPACITY; ++index)
    {
        if (!view->columns[index].used)
        {
            continue;
        }
        uint32_t slot = HashKey(view->columns[index].key);
        while (view->hash[slot] >= 0)
        {
            slot = (slot + 1u) & (HASH_CAPACITY - 1u);
        }
        view->hash[slot] = (int32_t)index;
    }
}

static Column *FindColumn(SosTerrainView *view, ColumnKey key)
{
    uint32_t slot = HashKey(key);
    while (view->hash[slot] >= 0)
    {
        Column *column = &view->columns[view->hash[slot]];
        if (SameKey(column->key, key))
        {
            return column;
        }
        slot = (slot + 1u) & (HASH_CAPACITY - 1u);
    }
    return NULL;
}

static double ColumnSize(int32_t level)
{
    return (double)COLUMN_CELLS * 0.5 * (double)(INT64_C(1) << level);
}

static double Metres(int64_t units)
{
    return (double)units / SOS_UNITS_PER_METRE;
}

static void AddLeaf(SosTerrainView *view, ColumnKey key, double distance)
{
    if (view->leafCount < MAX_VISIBLE)
    {
        view->leaves[view->leafCount++] = (Leaf){.key = key, .distance = distance};
    }
}

/* Walks the quadtree below one root column: a column near the eye splits into
 * its four children, a far one becomes a leaf. The explicit stack holds at
 * most three pending siblings per level. */
static void Visit(SosTerrainView *view, ColumnKey root, const double eye[3], double height,
                  double radius)
{
    ColumnKey stack[4 * (MAX_LEVEL + 1)];
    uint32_t depth = 0u;
    stack[depth++] = root;
    while (depth != 0u)
    {
        const ColumnKey key = stack[--depth];
        const double size = ColumnSize(key.level);
        const double minimumX = (double)key.x * size;
        const double minimumY = (double)key.y * size;
        const double dx = fmax(0.0, fmax(minimumX - eye[0], eye[0] - (minimumX + size)));
        const double dy = fmax(0.0, fmax(minimumY - eye[1], eye[1] - (minimumY + size)));
        const double flat = sqrt(dx * dx + dy * dy);
        if (flat > radius)
        {
            continue;
        }
        const double distance = sqrt(flat * flat + height * height);
        if (key.level == 0 || distance >= SPLIT_FACTOR * size)
        {
            AddLeaf(view, key, distance);
            continue;
        }
        for (int64_t child = 0; child < 4; ++child)
        {
            stack[depth++] = (ColumnKey){
                .level = key.level - 1,
                .x = key.x * 2 + (child & 1),
                .y = key.y * 2 + (child >> 1),
            };
        }
    }
}

static int CompareLeaves(const void *left, const void *right)
{
    const double a = ((const Leaf *)left)->distance;
    const double b = ((const Leaf *)right)->distance;
    return (a > b) - (a < b);
}

static void SelectLeaves(SosTerrainView *view, const SosTerrain *terrain,
                         const SosCameraView *camera)
{
    const SosPosition eyeUnits = {
        camera->renderOrigin.x + (int64_t)llround((double)camera->eye[0] * SOS_UNITS_PER_METRE),
        camera->renderOrigin.y + (int64_t)llround((double)camera->eye[1] * SOS_UNITS_PER_METRE),
        camera->renderOrigin.z + (int64_t)llround((double)camera->eye[2] * SOS_UNITS_PER_METRE),
    };
    const double eye[3] = {Metres(eyeUnits.x), Metres(eyeUnits.y), Metres(eyeUnits.z)};
    const double ground = Metres(SosTerrainSurfaceZ(terrain, eyeUnits.x, eyeUnits.y));
    const double height = fmax(1.0, eye[2] - ground);
    const double radius = fmin((double)camera->farPlane, MAX_VIEW_RADIUS);
    const double rootSize = ColumnSize(MAX_LEVEL);
    const int64_t firstX = (int64_t)floor((eye[0] - radius) / rootSize);
    const int64_t lastX = (int64_t)floor((eye[0] + radius) / rootSize);
    const int64_t firstY = (int64_t)floor((eye[1] - radius) / rootSize);
    const int64_t lastY = (int64_t)floor((eye[1] + radius) / rootSize);
    view->leafCount = 0u;
    for (int64_t y = firstY; y <= lastY; ++y)
    {
        for (int64_t x = firstX; x <= lastX; ++x)
        {
            Visit(view, (ColumnKey){.level = MAX_LEVEL, .x = x, .y = y}, eye, height, radius);
        }
    }
    qsort(view->leaves, view->leafCount, sizeof(view->leaves[0]), CompareLeaves);
}

static uint32_t PackColor(float red, float green, float blue)
{
    const float channels[3] = {red, green, blue};
    uint32_t packed = UINT32_C(0xFF000000);
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        const float value =
            channels[index] < 0.0f ? 0.0f : (channels[index] > 1.0f ? 1.0f : channels[index]);
        packed |= (uint32_t)lrintf(value * 255.0f) << (8u * index);
    }
    return packed;
}

/* Mock-up shading baked into vertex colours: a fixed sun from the south-east
 * and a material palette, with steep grass showing bare earth. */
static uint32_t ShadeVertex(const LaiueSurfaceVertexV1 *vertex)
{
    static const float palette[SOS_MATERIAL_COUNT][3] = {
        [SOS_MATERIAL_AIR] = {1.0f, 0.0f, 1.0f},     [SOS_MATERIAL_GRASS] = {0.33f, 0.52f, 0.22f},
        [SOS_MATERIAL_DIRT] = {0.45f, 0.34f, 0.22f}, [SOS_MATERIAL_STONE] = {0.48f, 0.47f, 0.46f},
        [SOS_MATERIAL_SAND] = {0.80f, 0.74f, 0.54f}, [SOS_MATERIAL_SNOW] = {0.94f, 0.95f, 0.97f},
    };
    const uint32_t material = vertex->material < SOS_MATERIAL_COUNT ? vertex->material : 0u;
    float base[3] = {palette[material][0], palette[material][1], palette[material][2]};
    const float upward = vertex->normal[2];
    if (material == SOS_MATERIAL_GRASS && upward < 0.75f)
    {
        const float rock = upward < 0.55f ? 1.0f : (0.75f - upward) / 0.2f;
        for (uint32_t channel = 0u; channel < 3u; ++channel)
        {
            base[channel] += (palette[SOS_MATERIAL_DIRT][channel] - base[channel]) * rock;
        }
    }
    static const float sun[3] = {0.36f, -0.46f, 0.81f};
    const float lambert =
        vertex->normal[0] * sun[0] + vertex->normal[1] * sun[1] + vertex->normal[2] * sun[2];
    const float light = 0.38f + 0.62f * (lambert > 0.0f ? lambert : 0.0f);
    return PackColor(base[0] * light, base[1] * light, base[2] * light);
}

static bool Upload(SosTerrainView *view, uint32_t usage, const void *data, uint64_t size,
                   LaiueGraphicsHandle *outHandle)
{
    const LaiueGraphicsBufferDescV1 description = {
        .structSize = sizeof(description),
        .usageFlags = usage,
        .sizeBytes = size,
    };
    *outHandle = 0u;
    if (view->device->createBuffer(view->device, &description, outHandle) == 0u)
    {
        return false;
    }
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload),
        .buffer = *outHandle,
        .data = data,
        .sizeBytes = size,
    };
    if (view->device->uploadBuffer(view->device, &upload) == 0u)
    {
        view->device->destroyHandle(view->device, *outHandle);
        *outHandle = 0u;
        return false;
    }
    return true;
}

static int64_t FloorDivide(int64_t value, int64_t divisor)
{
    const int64_t quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

/* Samples, meshes and uploads one column. False only when the column could
 * not be stored; an empty column is a success. */
static bool BuildColumn(SosTerrainView *view, const SosTerrain *terrain, Column *column)
{
    const int64_t stride = INT64_C(1) << column->key.level;
    const int64_t cellX = column->key.x * (int64_t)COLUMN_CELLS * stride - stride;
    const int64_t cellY = column->key.y * (int64_t)COLUMN_CELLS * stride - stride;
    const int64_t step = stride * SOS_TERRAIN_CELL_UNITS;

    int64_t lowest = INT64_MAX;
    int64_t highest = INT64_MIN;
    for (uint32_t row = 0u; row < SAMPLES_XY; ++row)
    {
        for (uint32_t columnIndex = 0u; columnIndex < SAMPLES_XY; ++columnIndex)
        {
            const int64_t height =
                SosTerrainSurfaceZ(terrain, (cellX + columnIndex * stride) * SOS_TERRAIN_CELL_UNITS,
                                   (cellY + row * stride) * SOS_TERRAIN_CELL_UNITS);
            lowest = height < lowest ? height : lowest;
            highest = height > highest ? height : highest;
        }
    }
    /* Two samples of margin below and above the surface; caves deeper than
     * that are out of sight from above. */
    int64_t firstZ = FloorDivide(lowest, step) - 2;
    int64_t countZ = FloorDivide(highest, step) + 3 - firstZ;
    if (countZ > (int64_t)MAX_SAMPLES_Z)
    {
        firstZ = FloorDivide(highest, step) + 3 - (int64_t)MAX_SAMPLES_Z;
        countZ = MAX_SAMPLES_Z;
    }
    const SosTerrainGrid grid = {
        .cellX = cellX,
        .cellY = cellY,
        .cellZ = firstZ * stride,
        .stride = (uint32_t)stride,
        .countX = SAMPLES_XY,
        .countY = SAMPLES_XY,
        .countZ = (uint32_t)countZ,
    };
    if (!SosTerrainSampleGrid(terrain, &grid, view->samples))
    {
        return false;
    }
    const size_t sampleCount = (size_t)SAMPLES_XY * SAMPLES_XY * (size_t)countZ;
    for (size_t index = 0u; index < sampleCount; ++index)
    {
        view->values[index] = (float)view->samples[index].density;
        view->materials[index] = view->samples[index].material;
    }

    LaiueSurfaceMeshRequestV1 request = {
        .structSize = sizeof(request),
        .countX = SAMPLES_XY,
        .countY = SAMPLES_XY,
        .countZ = (uint32_t)countZ,
        .values = view->values,
        .materials = view->materials,
        .emitMin = {1u, 1u, 0u},
        .emitMax = {COLUMN_CELLS + 1u, COLUMN_CELLS + 1u, (uint32_t)countZ},
        .cellScratch = view->cellScratch,
        .cellScratchCapacity = MAX_CELLS,
        .vertices = view->surfaceVertices,
        .vertexCapacity = MAX_SURFACE_VERTICES,
        .indices = view->surfaceIndices,
        .indexCapacity = MAX_SURFACE_INDICES,
    };
    if (view->buildSurfaceMesh(&request) != LAIUE_SURFACE_MESH_OK)
    {
        return false;
    }
    column->origin = (SosPosition){
        cellX * SOS_TERRAIN_CELL_UNITS,
        cellY * SOS_TERRAIN_CELL_UNITS,
        grid.cellZ * SOS_TERRAIN_CELL_UNITS,
    };
    if (request.indexCount == 0u)
    {
        column->empty = true;
        return true;
    }

    const float metresPerSample = (float)Metres(step);
    LaiueGraphicsVertexV2 *vertices = view->vertices;
    if (view->nativeIndices)
    {
        for (uint32_t index = 0u; index < request.vertexCount; ++index)
        {
            const LaiueSurfaceVertexV1 *source = &view->surfaceVertices[index];
            vertices[index] = (LaiueGraphicsVertexV2){
                .position = {source->position[0] * metresPerSample,
                             source->position[1] * metresPerSample,
                             source->position[2] * metresPerSample},
                .colorRGBA = ShadeVertex(source),
            };
        }
        column->indexed = true;
        column->elementCount = request.indexCount;
        return Upload(view, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices,
                      (uint64_t)request.vertexCount * sizeof(*vertices), &column->vertexBuffer) &&
               Upload(view, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, view->surfaceIndices,
                      (uint64_t)request.indexCount * sizeof(uint32_t), &column->indexBuffer);
    }
    /* Without native indices every triangle carries its own vertices. */
    for (uint32_t index = 0u; index < request.indexCount; ++index)
    {
        const LaiueSurfaceVertexV1 *source = &view->surfaceVertices[view->surfaceIndices[index]];
        vertices[index] = (LaiueGraphicsVertexV2){
            .position = {source->position[0] * metresPerSample,
                         source->position[1] * metresPerSample,
                         source->position[2] * metresPerSample},
            .colorRGBA = ShadeVertex(source),
        };
    }
    column->elementCount = request.indexCount;
    return Upload(view, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices,
                  (uint64_t)request.indexCount * sizeof(*vertices), &column->vertexBuffer);
}

/* A free cache slot, evicting the column unused for longest if needed. */
static Column *AcquireColumn(SosTerrainView *view)
{
    Column *oldest = NULL;
    for (uint32_t index = 0u; index < CACHE_CAPACITY; ++index)
    {
        Column *column = &view->columns[index];
        if (!column->used)
        {
            return column;
        }
        if (column->lastUsedFrame < view->frame &&
            (oldest == NULL || column->lastUsedFrame < oldest->lastUsedFrame))
        {
            oldest = column;
        }
    }
    if (oldest != NULL)
    {
        ReleaseColumn(view, oldest);
    }
    return oldest;
}

static void MarkForDrawing(SosTerrainView *view, Column *column)
{
    column->lastUsedFrame = view->frame;
    if (column->drawnFrame == view->frame)
    {
        return;
    }
    column->drawnFrame = view->frame;
    if (!column->empty && view->drawCount < MAX_VISIBLE)
    {
        view->drawList[view->drawCount++] = (uint32_t)(column - view->columns);
    }
}

static ColumnKey ParentKey(ColumnKey key)
{
    /* floor(x / 2) for negative indices too. */
    return (ColumnKey){
        .level = key.level + 1,
        .x = key.x >= 0 ? key.x / 2 : -((-key.x + 1) / 2),
        .y = key.y >= 0 ? key.y / 2 : -((-key.y + 1) / 2),
    };
}

static Column *CachedAncestor(SosTerrainView *view, ColumnKey key)
{
    for (ColumnKey parent = key; parent.level < MAX_LEVEL;)
    {
        parent = ParentKey(parent);
        Column *ancestor = FindColumn(view, parent);
        if (ancestor != NULL)
        {
            return ancestor;
        }
    }
    return NULL;
}

/* Builds the column into the cache; NULL when it is already there, the cache
 * is full of columns in use, or the build failed. */
static Column *Build(SosTerrainView *view, const SosTerrain *terrain, ColumnKey key)
{
    if (FindColumn(view, key) != NULL)
    {
        return NULL;
    }
    Column *column = AcquireColumn(view);
    if (column == NULL)
    {
        return NULL;
    }
    column->key = key;
    column->used = true;
    /* Needed this frame: later builds must not evict it. */
    column->lastUsedFrame = view->frame;
    if (!BuildColumn(view, terrain, column))
    {
        SosLog(SOS_LOG_WARNING, "terrain column level %d could not be built", (int)key.level);
        ReleaseColumn(view, column);
        column = NULL;
    }
    RebuildHash(view);
    return column;
}

void SosTerrainViewUpdate(SosTerrainView *view, const SosTerrain *terrain,
                          const SosCameraView *camera)
{
    if (view == NULL || terrain == NULL || camera == NULL)
    {
        return;
    }
    ++view->frame;
    view->drawCount = 0u;
    view->stats = (SosTerrainViewStats){0};
    RebuildHash(view);
    SelectLeaves(view, terrain, camera);

    /* Coverage first: a leaf with nothing to show gets a column three levels
     * coarser built, which covers 64 leaves at once. Detail follows, nearest
     * first, so moving fast never leaves holes for more than a frame or two. */
    uint32_t built = 0u;
    for (uint32_t index = 0u; index < view->leafCount && built < BUILDS_PER_FRAME; ++index)
    {
        const ColumnKey key = view->leaves[index].key;
        if (FindColumn(view, key) == NULL && CachedAncestor(view, key) == NULL)
        {
            ColumnKey cover = key;
            for (int32_t step = 0; step < COVER_LEVELS && cover.level < MAX_LEVEL; ++step)
            {
                cover = ParentKey(cover);
            }
            built += Build(view, terrain, cover) != NULL ? 1u : 0u;
        }
    }
    for (uint32_t index = 0u; index < view->leafCount; ++index)
    {
        const ColumnKey key = view->leaves[index].key;
        Column *column = FindColumn(view, key);
        if (column == NULL && built < BUILDS_PER_FRAME)
        {
            column = Build(view, terrain, key);
            ++built;
        }
        /* Not built yet: show the nearest coarser column that is. */
        column = column != NULL ? column : CachedAncestor(view, key);
        if (column != NULL)
        {
            MarkForDrawing(view, column);
        }
    }
    view->stats.visibleColumns = view->leafCount;
    view->stats.builtThisFrame = built;
    for (uint32_t index = 0u; index < CACHE_CAPACITY; ++index)
    {
        view->stats.cachedColumns += view->columns[index].used ? 1u : 0u;
    }
}

void SosTerrainViewDraw(SosTerrainView *view, const SosCameraView *camera)
{
    if (view == NULL || camera == NULL)
    {
        return;
    }
    for (uint32_t index = 0u; index < view->drawCount; ++index)
    {
        const Column *column = &view->columns[view->drawList[index]];
        const LaiueGraphicsDrawItemV2 draw = {
            .structSize = LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE,
            .vertexBuffer = column->vertexBuffer,
            .indexBuffer = column->indexed ? column->indexBuffer : 0u,
            .indexCount = column->elementCount,
            .originRelative = {(float)Metres(column->origin.x - camera->renderOrigin.x),
                               (float)Metres(column->origin.y - camera->renderOrigin.y),
                               (float)Metres(column->origin.z - camera->renderOrigin.z)},
            .scale = 1.0f,
        };
        view->stats.drawnColumns += view->device->submit(view->device, &draw, 1u) != 0u ? 1u : 0u;
    }
}

SosTerrainViewStats SosTerrainViewGetStats(const SosTerrainView *view)
{
    return view == NULL ? (SosTerrainViewStats){0} : view->stats;
}
