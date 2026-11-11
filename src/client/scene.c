#include "client/scene.h"

#include "app/log.h"
#include "app/report.h"
#include "client/camera.h"
#include "client/people_view.h"
#include "client/plants_view.h"
#include "client/terrain_view.h"
#include "sim/clock.h"
#include "sim/terrain.h"
#include "sim/world.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const double speedPresets[] = {1.0,     60.0,      3600.0,
                                      86400.0, 2592000.0, SOS_CLOCK_MAX_TIME_SCALE};
/* Holding the dig button digs this often. */
#define DIG_INTERVAL_SECONDS 0.15
/* The dug sphere grows with the view, so the god's hand shows at any zoom. */
#define DIG_RADIUS_PER_DISTANCE (1.0 / 40.0)
#define DIG_MIN_RADIUS_METRES 1.0
#define DIG_MAX_RADIUS_METRES 8.0
/* Ray directions leave the floats with this many world units per metre of
 * direction: plenty of precision, far from overflow. */
#define DIG_RAY_SCALE 65536.0f

static const char *const speedNames[] = {"real time",          "a minute per second",
                                         "an hour per second", "a day per second",
                                         "a month per second", "a year per second"};

struct SosScene
{
    SosWorld *world;
    SosClock clock;
    double resumeScale;
    SosCamera camera;
    SosCameraView view;
    bool viewReady;
    /* Serial of the person the camera follows; 0 follows nobody. */
    uint64_t followed;
    const LaiueSceneMathServiceV1 *math;
    /* Without the camera-ray tail of camera math nothing can be dug. */
    bool picking;
    /* Seconds until the held dig button digs again. */
    double digCooldown;
    const LaiueMesherServiceV1 *mesher;
    uint32_t mesherServiceSize;
    SosTerrainView *terrain;
    SosPlantsView *plants;
    SosPeopleView *people;
    SosBeingsStats reported;
};

static bool FindBeing(const SosBeings *beings, uint64_t serial, SosBeingView *outView)
{
    for (uint32_t slot = 0u; slot < SosBeingsSlotCount(beings); ++slot)
    {
        if (SosBeingsView(beings, slot, outView) && outView->serial == serial)
        {
            return true;
        }
    }
    return false;
}

/* The living being after the followed one in serial order, wrapping around. */
static uint64_t NextBeing(const SosBeings *beings, uint64_t after)
{
    uint64_t next = 0u;
    uint64_t first = 0u;
    for (uint32_t slot = 0u; slot < SosBeingsSlotCount(beings); ++slot)
    {
        SosBeingView view;
        if (!SosBeingsView(beings, slot, &view))
        {
            continue;
        }
        if (first == 0u || view.serial < first)
        {
            first = view.serial;
        }
        if (view.serial > after && (next == 0u || view.serial < next))
        {
            next = view.serial;
        }
    }
    return next != 0u ? next : first;
}

SosScene *SosSceneCreate(const SosSceneDesc *desc)
{
    if (desc == NULL)
    {
        return NULL;
    }
    SosScene *scene = calloc(1u, sizeof(*scene));
    if (scene == NULL)
    {
        return NULL;
    }
    const SosWorldDesc worldDesc = {.seed = desc->seed};
    scene->world = SosWorldCreate(&worldDesc);
    if (scene->world == NULL ||
        !SosClockInit(&scene->clock, SOS_CLOCK_TICKS_PER_SECOND,
                      SOS_CLOCK_DEFAULT_MAX_ADVANCE_SECONDS) ||
        !SosClockSetTimeScale(&scene->clock, desc->timeScale))
    {
        SosSceneDestroy(scene);
        return NULL;
    }
    scene->resumeScale = desc->timeScale > 0.0 ? desc->timeScale : 1.0;

    if (desc->math != NULL && desc->mathServiceSize >= LAIUE_SCENE_MATH_SERVICE_V1_CAMERA_SIZE &&
        desc->math->structSize >= LAIUE_SCENE_MATH_SERVICE_V1_CAMERA_SIZE &&
        desc->math->matrix4LookAt != NULL && desc->math->matrix4Perspective != NULL &&
        desc->math->matrix4Multiply != NULL)
    {
        scene->math = desc->math;
        scene->picking = desc->mathServiceSize >= LAIUE_SCENE_MATH_SERVICE_V1_PICKING_SIZE &&
                         desc->math->structSize >= LAIUE_SCENE_MATH_SERVICE_V1_PICKING_SIZE &&
                         desc->math->viewRayThroughScreenPoint != NULL;
        if (!scene->picking)
        {
            SosLog(SOS_LOG_WARNING, "camera math cannot cast rays; digging is unavailable");
        }
    }
    else
    {
        SosLog(SOS_LOG_WARNING, "camera math is unavailable; the world is not drawn");
    }
    scene->mesher = desc->mesher;
    scene->mesherServiceSize = desc->mesherServiceSize;
    if (desc->device != NULL)
    {
        (void)SosSceneAttachDevice(scene, desc->device);
    }

    const SosBeings *beings = SosWorldBeings(scene->world);
    scene->followed = NextBeing(beings, 0u);
    SosBeingView founder;
    const SosPosition origin = {0, 0, 0};
    SosCameraInit(&scene->camera,
                  FindBeing(beings, scene->followed, &founder) ? founder.position : origin);
    if (desc->cameraDistance > 0.0)
    {
        scene->camera.distance = desc->cameraDistance;
        SosCameraZoom(&scene->camera, 0.0f);
    }
    SosReportWorld(scene->world, "world created");
    return scene;
}

void SosSceneDestroy(SosScene *scene)
{
    if (scene == NULL)
    {
        return;
    }
    SosSceneDetachDevice(scene);
    SosWorldDestroy(scene->world);
    free(scene);
}

bool SosSceneAttachDevice(SosScene *scene, LaiueGraphicsDeviceV2 *device)
{
    if (scene == NULL || device == NULL)
    {
        return false;
    }
    SosSceneDetachDevice(scene);
    scene->terrain = SosTerrainViewCreate(device, scene->mesher, scene->mesherServiceSize);
    if (scene->terrain == NULL)
    {
        SosLog(SOS_LOG_WARNING, "surface meshing is unavailable; the terrain is not drawn");
    }
    scene->plants = SosPlantsViewCreate(device);
    if (scene->plants == NULL)
    {
        SosLog(SOS_LOG_WARNING, "plants cannot be drawn");
    }
    scene->people = SosPeopleViewCreate(device);
    if (scene->people == NULL)
    {
        SosLog(SOS_LOG_WARNING, "people cannot be drawn");
    }
    return true;
}

void SosSceneDetachDevice(SosScene *scene)
{
    if (scene == NULL)
    {
        return;
    }
    SosPeopleViewDestroy(scene->people);
    SosPlantsViewDestroy(scene->plants);
    SosTerrainViewDestroy(scene->terrain);
    scene->people = NULL;
    scene->plants = NULL;
    scene->terrain = NULL;
}

static void ApplyTimeControls(SosScene *scene, const SosControls *controls)
{
    if (controls->speedPreset >= 0 &&
        (size_t)controls->speedPreset < sizeof(speedPresets) / sizeof(speedPresets[0]))
    {
        scene->resumeScale = speedPresets[controls->speedPreset];
        (void)SosClockSetTimeScale(&scene->clock, scene->resumeScale);
        SosLog(SOS_LOG_INFO, "time runs at %s", speedNames[controls->speedPreset]);
    }
    if (controls->togglePause)
    {
        const bool paused = scene->clock.timeScale == 0.0;
        (void)SosClockSetTimeScale(&scene->clock, paused ? scene->resumeScale : 0.0);
        SosLog(SOS_LOG_INFO, "%s", paused ? "time resumes" : "time is paused");
    }
}

static void ApplyCameraControls(SosScene *scene, const SosControls *controls, double realSeconds)
{
    const SosBeings *beings = SosWorldBeings(scene->world);
    if (controls->followNext)
    {
        scene->followed = NextBeing(beings, scene->followed);
    }
    if (controls->zoomSteps != 0.0f)
    {
        SosCameraZoom(&scene->camera, controls->zoomSteps);
    }
    if (controls->rotating)
    {
        SosCameraRotate(&scene->camera, controls->rotateX, controls->rotateY);
    }
    /* Moving the camera by hand lets the followed person go. */
    if (controls->moveForward != 0.0f || controls->moveRight != 0.0f)
    {
        scene->followed = 0u;
        SosCameraPan(&scene->camera, controls->moveForward, controls->moveRight,
                     realSeconds * (controls->moveFast ? 3.0 : 1.0));
    }
    if (controls->dragForward != 0.0f || controls->dragRight != 0.0f)
    {
        scene->followed = 0u;
        SosCameraPan(&scene->camera, controls->dragForward, controls->dragRight, 1.0);
    }
    SosBeingView followed;
    if (scene->followed != 0u && FindBeing(beings, scene->followed, &followed))
    {
        scene->camera.focus = followed.position;
    }
    else
    {
        scene->followed = 0u;
        scene->camera.focus.z = SosTerrainSurfaceZ(SosWorldTerrain(scene->world),
                                                   scene->camera.focus.x, scene->camera.focus.y);
    }
}

/* Digs where the controls point while the dig button is held: a ray from
 * the camera through the pointer finds the ground, a sphere is dug there and
 * the terrain around it is rebuilt. */
static void Dig(SosScene *scene, const SosControls *controls, double realSeconds)
{
    if (!controls->digging)
    {
        scene->digCooldown = 0.0;
        return;
    }
    scene->digCooldown -= realSeconds;
    if (scene->digCooldown > 0.0 || !scene->picking || !scene->viewReady)
    {
        return;
    }
    scene->digCooldown = DIG_INTERVAL_SECONDS;
    float origin[3];
    float direction[3];
    if (!scene->math->viewRayThroughScreenPoint(scene->view.viewProjection, controls->digX,
                                                controls->digY, origin, direction))
    {
        return;
    }
    const SosPosition renderOrigin = scene->view.renderOrigin;
    const SosPosition start = {
        renderOrigin.x + (int64_t)(origin[0] * (float)SOS_UNITS_PER_METRE),
        renderOrigin.y + (int64_t)(origin[1] * (float)SOS_UNITS_PER_METRE),
        renderOrigin.z + (int64_t)(origin[2] * (float)SOS_UNITS_PER_METRE),
    };
    const SosPosition heading = {(int64_t)(direction[0] * DIG_RAY_SCALE),
                                 (int64_t)(direction[1] * DIG_RAY_SCALE),
                                 (int64_t)(direction[2] * DIG_RAY_SCALE)};
    SosPosition hit;
    if (!SosTerrainRaycast(SosWorldTerrain(scene->world), start, heading,
                           SOS_TERRAIN_RAY_MAX_DISTANCE, &hit))
    {
        return;
    }
    double radius = scene->camera.distance * DIG_RADIUS_PER_DISTANCE;
    radius = radius < DIG_MIN_RADIUS_METRES   ? DIG_MIN_RADIUS_METRES
             : radius > DIG_MAX_RADIUS_METRES ? DIG_MAX_RADIUS_METRES
                                              : radius;
    SosTerrainBox changed;
    if (SosWorldDig(scene->world, hit, (int64_t)(radius * SOS_UNITS_PER_METRE), &changed) > 0u)
    {
        SosTerrainViewInvalidate(scene->terrain, &changed);
    }
}

bool SosSceneUpdate(SosScene *scene, const SosControls *controls, double realSeconds,
                    float aspectRatio, float outViewProjection[16])
{
    if (scene == NULL || controls == NULL)
    {
        return false;
    }
    ApplyTimeControls(scene, controls);
    SosWorldAdvance(scene->world, SosClockAdvance(&scene->clock, realSeconds));
    SosReportPopulation(scene->world, &scene->reported);
    ApplyCameraControls(scene, controls, realSeconds);

    scene->viewReady = scene->math != NULL &&
                       SosCameraBuildView(&scene->camera, scene->math, aspectRatio, &scene->view);
    if (!scene->viewReady)
    {
        return false;
    }
    Dig(scene, controls, realSeconds);
    SosTerrainViewUpdate(scene->terrain, SosWorldTerrain(scene->world), &scene->view);
    if (outViewProjection != NULL)
    {
        memcpy(outViewProjection, scene->view.viewProjection, sizeof(scene->view.viewProjection));
    }
    return true;
}

void SosSceneDraw(SosScene *scene)
{
    if (scene == NULL || !scene->viewReady)
    {
        return;
    }
    SosTerrainViewDraw(scene->terrain, &scene->view);
    (void)SosPlantsViewDraw(scene->plants, SosWorldPlants(scene->world),
                            SosWorldTerrain(scene->world), SosWorldTick(scene->world),
                            &scene->view);
    (void)SosPeopleViewDraw(scene->people, SosWorldBeings(scene->world), SosWorldTick(scene->world),
                            &scene->view);
}
