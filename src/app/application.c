#include "app/application.h"

#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/rebase_policy.h"

#include "content/content_catalog.h"
#include "input/input.h"
#include "platform/time.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "scene/camera.h"
#include "scene/chunk_streaming.h"
#include "scene/panorama.h"
#include "world/world.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define SIMULATION_VIEW_RADIUS_CHUNKS 2
#define SIMULATION_FIELD_OF_VIEW_DEGREES 90.0f
#define SIMULATION_CAMERA_SPEED 7.0f
#define SIMULATION_CAMERA_FAST_SPEED 22.0f
#define SIMULATION_MOUSE_SENSITIVITY 0.0022f
#define SIMULATION_MAX_CONSECUTIVE_RENDER_FAILURES 120U

typedef struct SimulationApplication
{
    Window *window;
    Input *input;
    LaiueContentCatalog *content;
    Renderer *renderer;
    World *world;
    ChunkStreaming *streaming;
    Camera camera;
    PanoramaCache panorama;
    int32_t windowWidth;
    int32_t windowHeight;
    double previousTimeSeconds;
    uint32_t maximumPresentedFrames;
    uint32_t presentedFrames;
    uint32_t consecutiveRenderFailures;
    int exitCode;
} SimulationApplication;

static int64_t FloorToInt64(double value)
{
    return (int64_t)floor(value);
}

static void CameraBlockPosition(const Camera *camera, int64_t outBlock[3], float outRelativeEye[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outBlock[axis] = FloorToInt64(camera->position[axis]);
        outRelativeEye[axis] = (float)(camera->position[axis] - (double)outBlock[axis]);
    }
}

// The parameter order is fixed by the engine RawInputCallback ABI.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void HandleRawInput(void *userData, void *rawInputHandle)
{
    SimulationApplication *application = userData;
    if (application != NULL && application->input != NULL)
    {
        InputHandleRawInput(application->input, rawInputHandle);
    }
}

static void StreamingCenter(const Camera *camera, int64_t outCenter[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outCenter[axis] = SimulationBlockToChunkFloor(FloorToInt64(camera->position[axis]));
    }
}

static bool ApplyOriginShift(SimulationApplication *application)
{
    SimulationOriginShift shift;
    if (!SimulationOriginShiftPlan(application->camera.position, &shift))
    {
        return false;
    }
    if (!shift.required)
    {
        return true;
    }
    if (!ChunkStreamingPause(application->streaming))
    {
        return false;
    }

    bool worldShifted =
        WorldRebase(application->world, shift.block[0], shift.block[1], shift.block[2]);
    if (worldShifted)
    {
        SimulationOriginShiftApply(application->camera.position, &shift);
    }

    int64_t center[3];
    StreamingCenter(&application->camera, center);
    int64_t chunkShift[3] = {0, 0, 0};
    if (worldShifted)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            chunkShift[axis] = shift.block[axis] / CHUNK_SIZE;
        }
    }
    bool streamingResumed = ChunkStreamingResumeAfterOriginChange(
        application->streaming, true, chunkShift[0], chunkShift[1], chunkShift[2], center[0],
        center[1], center[2]);
    return worldShifted && streamingResumed;
}

static void ConfigureLighting(RendererFrameSetup *frame)
{
    frame->sunDirection[0] = 0.0f;
    frame->sunDirection[1] = -0.9701425f;
    frame->sunDirection[2] = -0.2425356f;
    frame->sunColor[0] = 0.95f;
    frame->sunColor[1] = 0.76f;
    frame->sunColor[2] = 0.62f;
    frame->ambientColor[0] = 0.16f;
    frame->ambientColor[1] = 0.18f;
    frame->ambientColor[2] = 0.26f;
    frame->skyColor[0] = 0.025f;
    frame->skyColor[1] = 0.018f;
    frame->skyColor[2] = 0.04f;
    frame->gamma = 2.2f;
}

static void OnFrame(void *userData)
{
    SimulationApplication *application = userData;
    if (application == NULL)
    {
        return;
    }

    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }
    if (WindowConsumeResize(application->window))
    {
        WindowGetClientSize(application->window, &application->windowWidth,
                            &application->windowHeight);
        if (application->windowWidth > 0 && application->windowHeight > 0)
        {
            RendererResize(application->renderer, application->windowWidth,
                           application->windowHeight);
        }
    }
    if (InputConsumeKeyPress(application->input, INPUT_KEY_ESCAPE))
    {
        WindowRequestClose(application->window);
        InputEndFrame(application->input);
        return;
    }

    double currentTime = PlatformTimeSeconds();
    float deltaSeconds = SimulationFrameDeltaSeconds(application->previousTimeSeconds, currentTime);
    application->previousTimeSeconds = currentTime;

    int32_t mouseDeltaX = 0;
    int32_t mouseDeltaY = 0;
    InputGetMouseDelta(application->input, &mouseDeltaX, &mouseDeltaY);
    float speed = InputIsKeyDown(application->input, INPUT_KEY_SHIFT) ? SIMULATION_CAMERA_FAST_SPEED
                                                                      : SIMULATION_CAMERA_SPEED;
    CameraUpdate(&application->camera, deltaSeconds,
                 InputIsKeyDown(application->input, INPUT_KEY_W),
                 InputIsKeyDown(application->input, INPUT_KEY_A),
                 InputIsKeyDown(application->input, INPUT_KEY_S),
                 InputIsKeyDown(application->input, INPUT_KEY_D),
                 InputIsKeyDown(application->input, INPUT_KEY_SPACE), mouseDeltaX, mouseDeltaY,
                 speed, SIMULATION_MOUSE_SENSITIVITY);
    if (InputIsKeyDown(application->input, INPUT_KEY_CONTROL))
    {
        application->camera.position[2] -= (double)(speed * deltaSeconds);
    }

    if (!ApplyOriginShift(application))
    {
        application->exitCode = 8;
        WindowRequestClose(application->window);
        InputEndFrame(application->input);
        return;
    }

    int64_t renderOriginBlock[3];
    float relativeEye[3];
    CameraBlockPosition(&application->camera, renderOriginBlock, relativeEye);
    ChunkStreamingSetCenter(application->streaming,
                            SimulationBlockToChunkFloor(renderOriginBlock[0]),
                            SimulationBlockToChunkFloor(renderOriginBlock[1]),
                            SimulationBlockToChunkFloor(renderOriginBlock[2]));
    ChunkStreamingPump(application->streaming);

    if (application->windowWidth <= 0 || application->windowHeight <= 0)
    {
        InputEndFrame(application->input);
        return;
    }

    float view[16];
    CameraGetViewMatrix(&application->camera, relativeEye, view);
    RendererFrameSetup frame;
    PanoramaBuildFrameSetup(&application->panorama, RENDER_PROJECTION_PERSPECTIVE,
                            SIMULATION_FIELD_OF_VIEW_DEGREES, application->windowWidth,
                            application->windowHeight, 0.05f, 2048.0f, view, &frame);
    ConfigureLighting(&frame);
    // Часы анимации текстур принадлежат приложению: расписание кадров
    // лежит в текстурпаке, а идти времени или стоять — решает игра.
    frame.animationSeconds = currentTime;

    if (!RendererBeginFrame(application->renderer, &frame))
    {
        application->consecutiveRenderFailures++;
        if (application->consecutiveRenderFailures >= SIMULATION_MAX_CONSECUTIVE_RENDER_FAILURES)
        {
            application->exitCode = 9;
            WindowRequestClose(application->window);
        }
        InputEndFrame(application->input);
        return;
    }
    application->consecutiveRenderFailures = 0;

    for (uint32_t pass = 0; pass < frame.passCount; ++pass)
    {
        RendererBeginScenePass(application->renderer, pass);
        ChunkStreamingDraw(application->streaming, frame.passes[pass].viewProjection,
                           renderOriginBlock);
    }
    if (!RendererEndFrame(application->renderer))
    {
        application->exitCode = 9;
        WindowRequestClose(application->window);
    }
    else
    {
        application->presentedFrames++;
        if (application->maximumPresentedFrames > 0 &&
            application->presentedFrames >= application->maximumPresentedFrames)
        {
            WindowRequestClose(application->window);
        }
    }

    InputEndFrame(application->input);
}

static void DestroyApplication(SimulationApplication *application)
{
    if (application == NULL)
    {
        return;
    }
    if (application->window != NULL)
    {
        WindowSetRawInputCallback(application->window, NULL, NULL);
        WindowSetMouseLook(application->window, false);
    }
    if (application->streaming != NULL)
    {
        ChunkStreamingDestroy(application->streaming);
        application->streaming = NULL;
    }
    if (application->world != NULL)
    {
        WorldDestroy(application->world);
        application->world = NULL;
    }
    if (application->renderer != NULL)
    {
        RendererDestroy(application->renderer);
        application->renderer = NULL;
    }
    if (application->content != NULL)
    {
        LaiueContentCatalogDestroy(application->content);
        application->content = NULL;
    }
    if (application->input != NULL)
    {
        InputDestroy(application->input);
        application->input = NULL;
    }
    if (application->window != NULL)
    {
        WindowDestroy(application->window);
        application->window = NULL;
    }
    free(application);
}

int SimulationApplicationRun(SimulationRunMode mode)
{
    if (mode < SIMULATION_RUN_INTERACTIVE || mode > SIMULATION_RUN_REBASE_RENDER_SMOKE)
    {
        return 1;
    }
    SimulationApplication *application = calloc(1, sizeof(*application));
    if (application == NULL)
    {
        return 1;
    }
    application->maximumPresentedFrames = mode == SIMULATION_RUN_INTERACTIVE ? 0U : 3U;
    application->exitCode = 0;

    WindowConfiguration windowConfiguration = {
        .title = L"Simulation of sins",
        .width = 1280,
        .height = 720,
    };
    application->window = WindowCreate(&windowConfiguration);
    if (application->window == NULL)
    {
        DestroyApplication(application);
        return 2;
    }
    application->input = InputCreate(WindowGetNativeHandle(application->window));
    if (application->input == NULL)
    {
        DestroyApplication(application);
        return 3;
    }
    WindowGetClientSize(application->window, &application->windowWidth, &application->windowHeight);
    application->content = LaiueContentCatalogCreate(NULL);
    if (application->content == NULL)
    {
        DestroyApplication(application);
        return 4;
    }
    application->renderer = RendererCreate(WindowGetNativeHandle(application->window),
                                           application->windowWidth, application->windowHeight);
    // Имена материалов принадлежат игре, а не движку: текстурпак — папка,
    // и файл в ней зовётся так же, как здесь написано. Расширение
    // подбирает движок, поэтому в паке может лежать и PNG, и готовый .lt.
    static const wchar_t *const materialNames[] = {
        L"blocks/foundation",
        L"blocks/marker",
        L"blocks/accent",
    };
    if (application->renderer == NULL ||
        !RendererSetMaterialNames(application->renderer, materialNames,
                                  (uint32_t)(sizeof(materialNames) / sizeof(materialNames[0]))) ||
        !RendererPrepareWorldFrom(application->renderer, application->content))
    {
        DestroyApplication(application);
        return 5;
    }
    application->world = WorldCreate(NULL);
    if (application->world == NULL || !SimulationFoundationWorldPopulate(application->world))
    {
        DestroyApplication(application);
        return 6;
    }
    if (mode == SIMULATION_RUN_REBASE_RENDER_SMOKE &&
        !WorldRebase(application->world,
                     -(int64_t)(SIMULATION_REBASE_THRESHOLD_CHUNKS * CHUNK_SIZE), 0, 0))
    {
        DestroyApplication(application);
        return 6;
    }
    application->streaming = ChunkStreamingCreate(application->world, application->renderer,
                                                  SIMULATION_VIEW_RADIUS_CHUNKS);
    if (application->streaming == NULL)
    {
        DestroyApplication(application);
        return 7;
    }

    double initialCameraX = mode == SIMULATION_RUN_REBASE_RENDER_SMOKE
                                ? (double)(SIMULATION_REBASE_THRESHOLD_CHUNKS * CHUNK_SIZE) + 1.25
                                : 0.0;
    CameraInit(&application->camera, initialCameraX, -10.0, 4.0, 0.0f, -0.18f);
    application->previousTimeSeconds = PlatformTimeSeconds();
    WindowSetMouseLook(application->window, true);
    WindowSetRawInputCallback(application->window, HandleRawInput, application);
    WindowRunLoop(application->window, OnFrame, application);

    int exitCode = application->exitCode;
    DestroyApplication(application);
    return exitCode;
}
