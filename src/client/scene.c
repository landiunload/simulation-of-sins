#include "client/scene.h"

#include "app/log.h"
#include "app/report.h"
#include "client/camera.h"
#include "client/people_view.h"
#include "client/terrain_view.h"
#include "sim/clock.h"
#include "sim/world.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const double speedPresets[] = {1.0,     60.0,      3600.0,
                                      86400.0, 2592000.0, SOS_CLOCK_MAX_TIME_SCALE};
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
    SosTerrainView *terrain;
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
    if (desc == NULL || desc->device == NULL)
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
    }
    else
    {
        SosLog(SOS_LOG_WARNING, "camera math is unavailable; the world is not drawn");
    }
    scene->terrain = SosTerrainViewCreate(desc->device, desc->mesher, desc->mesherServiceSize);
    if (scene->terrain == NULL)
    {
        SosLog(SOS_LOG_WARNING, "surface meshing is unavailable; the terrain is not drawn");
    }
    scene->people = SosPeopleViewCreate(desc->device);
    if (scene->people == NULL)
    {
        SosLog(SOS_LOG_WARNING, "people cannot be drawn");
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
    SosPeopleViewDestroy(scene->people);
    SosTerrainViewDestroy(scene->terrain);
    SosWorldDestroy(scene->world);
    free(scene);
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
    if (controls->moveForward != 0.0f || controls->moveRight != 0.0f)
    {
        /* Moving the camera by hand lets the followed person go. */
        scene->followed = 0u;
        SosCameraPan(&scene->camera, controls->moveForward, controls->moveRight,
                     realSeconds * (controls->moveFast ? 3.0 : 1.0));
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
    (void)SosPeopleViewDraw(scene->people, SosWorldBeings(scene->world), SosWorldTick(scene->world),
                            &scene->view);
}
