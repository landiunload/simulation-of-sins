#include "app/application.h"

#include "game/falling_cubes.h"
#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/ground_provider.h"
#include "game/rebase_policy.h"

#include "content/content_catalog.h"
#include "input/input.h"
#include "platform/time.h"
#include "platform/window.h"
#include "render/chunk_geometry.h"
#include "render/renderer.h"
#include "scene/camera.h"
#include "scene/chunk_streaming.h"
#include "scene/panorama.h"
#include "world/world.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SIMULATION_VIEW_RADIUS_CHUNKS 2
#define SIMULATION_FIELD_OF_VIEW_DEGREES 90.0f
#define SIMULATION_CAMERA_SPEED 7.0f
#define SIMULATION_CAMERA_FAST_SPEED 22.0f
#define SIMULATION_MOUSE_SENSITIVITY 0.0022f
#define SIMULATION_MAX_CONSECUTIVE_RENDER_FAILURES 120U
// Высота появления куба над поверхностью пола. Достаточно, чтобы падение
// было видно, и достаточно, чтобы растущая куча до неё не дотянулась.
#define SIMULATION_CUBE_SPAWN_HEIGHT 18.0


typedef struct SimulationApplication
{
    Window *window;
    Input *input;
    LaiueContentCatalog *content;
    Renderer *renderer;
    World *world;
    ChunkStreaming *streaming;
    // Базовый слой обязан пережить World: движок хранит указатель на него.
    SimulationGroundProvider ground;
    SimulationCubeField cubes;
    RendererMesh *cubeMesh;
    // Кубов сколько угодно, поэтому буфер инстансов тоже растёт.
    RendererMeshInstance *cubeInstances;
    uint32_t cubeInstanceCapacity;
    Camera camera;
    PanoramaCache panorama;
    int32_t windowWidth;
    int32_t windowHeight;
    double previousTimeSeconds;
    uint32_t maximumPresentedFrames;
    uint32_t presentedFrames;
    uint32_t consecutiveRenderFailures;
    FILE *profileFile;
    uint64_t profileFrameCount;
    uint64_t profileWindowFrames;
    double profileWindowStart;
    double profileFrameSum;
    double profileFrameMaximum;
    double profilePhysicsSum;
    double profilePhysicsMaximum;
    double profilePrepareSum;
    double profilePrepareMaximum;
    double profilePresentSum;
    double profilePresentMaximum;
    int exitCode;
} SimulationApplication;

static void ProfileWriteWindow(SimulationApplication *application, uint32_t bodyCount,
                               uint32_t awakeCount, uint32_t candidatePairCount,
                               uint32_t contactCount, double now)
{
    if (application == NULL || application->profileFile == NULL ||
        application->profileWindowFrames == 0u || now - application->profileWindowStart < 1.0)
    {
        return;
    }
    double frames = (double)application->profileWindowFrames;
#if defined(_MSC_VER)
#define SIMULATION_PROFILE_PRINT fprintf_s
#else
#define SIMULATION_PROFILE_PRINT fprintf
#endif
    (void)SIMULATION_PROFILE_PRINT(
        application->profileFile,
        "%.6f,%llu,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        now, (unsigned long long)application->profileFrameCount, bodyCount, awakeCount,
        candidatePairCount, contactCount,
        application->profileFrameSum * 1000.0 / frames,
        application->profileFrameMaximum * 1000.0,
        application->profilePhysicsSum * 1000.0 / frames,
        application->profilePhysicsMaximum * 1000.0,
        application->profilePrepareSum * 1000.0 / frames,
        application->profilePrepareMaximum * 1000.0,
        application->profilePresentSum * 1000.0 / frames,
        application->profilePresentMaximum * 1000.0,
        (double)application->profileWindowFrames / (now - application->profileWindowStart),
        frames);
#undef SIMULATION_PROFILE_PRINT
    fflush(application->profileFile);
    application->profileWindowStart = now;
    application->profileWindowFrames = 0u;
    application->profileFrameSum = 0.0;
    application->profileFrameMaximum = 0.0;
    application->profilePhysicsSum = 0.0;
    application->profilePhysicsMaximum = 0.0;
    application->profilePrepareSum = 0.0;
    application->profilePrepareMaximum = 0.0;
    application->profilePresentSum = 0.0;
    application->profilePresentMaximum = 0.0;
}

static FILE *ProfileOpenFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *profilePath = NULL;
    size_t profilePathBytes = 0u;
    FILE *profileFile = NULL;
    if (_dupenv_s(&profilePath, &profilePathBytes, "SOS_FRAME_PROFILE") == 0 &&
        profilePath != NULL && profilePath[0] != '\0')
    {
        (void)fopen_s(&profileFile, profilePath, "wb");
    }
    free(profilePath);
    return profileFile;
#else
    const char *profilePath = getenv("SOS_FRAME_PROFILE");
    if (profilePath == NULL || profilePath[0] == '\0')
    {
        return NULL;
    }
    return fopen(profilePath, "wb");
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ProfileRecordFrame(SimulationApplication *application, double frameSeconds,
                               double physicsSeconds, double prepareSeconds,
                               double presentSeconds, uint32_t bodyCount, uint32_t awakeCount,
                               uint32_t candidatePairCount, uint32_t contactCount, double now)
{
    if (application == NULL || application->profileFile == NULL)
    {
        return;
    }
    if (application->profileWindowFrames == 0u)
    {
        application->profileWindowStart = now;
    }
    ++application->profileFrameCount;
    ++application->profileWindowFrames;
    application->profileFrameSum += frameSeconds;
    if (frameSeconds > application->profileFrameMaximum)
    {
        application->profileFrameMaximum = frameSeconds;
    }
    application->profilePhysicsSum += physicsSeconds;
    if (physicsSeconds > application->profilePhysicsMaximum)
    {
        application->profilePhysicsMaximum = physicsSeconds;
    }
    application->profilePrepareSum += prepareSeconds;
    if (prepareSeconds > application->profilePrepareMaximum)
    {
        application->profilePrepareMaximum = prepareSeconds;
    }
    application->profilePresentSum += presentSeconds;
    if (presentSeconds > application->profilePresentMaximum)
    {
        application->profilePresentMaximum = presentSeconds;
    }
    ProfileWriteWindow(application, bodyCount, awakeCount, candidatePairCount, contactCount, now);
}

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
        // Кубы живут в локальных координатах, как и камера: смена начала
        // координат обязана сдвинуть и их, иначе куча уедет из-под ног.
        SimulationCubeFieldRebase(&application->cubes, shift.block);
    }
    bool streamingResumed = ChunkStreamingResumeAfterOriginChange(
        application->streaming, true, chunkShift[0], chunkShift[1], chunkShift[2], center[0],
        center[1], center[2]);
    return worldShifted && streamingResumed;
}

// Меш куба — тот же формат, что и у чанков: шесть граней единичной ячейки.
// Он создаётся один раз, а все кубы рисуются его инстансами.
static RendererMesh *CreateCubeMesh(Renderer *renderer)
{
    ChunkQuad quads[6];
    for (uint32_t face = 0; face < 6u; ++face)
    {
        quads[face] = PackChunkQuad(0u, 0u, 0u, face, (uint32_t)SIMULATION_MATERIAL_ACCENT, 1u, 1u,
                                    1u);
    }
    return RendererCreateMesh(renderer, quads, 6u);
}

// Центр мира в локальных координатах: абсолютный ноль минус то, насколько
// локальная сетка от него уехала.
static void CubeSpawnPosition(const SimulationApplication *application, double outPosition[3])
{
    outPosition[0] = 0.5 - (double)application->ground.originBlock[0];
    outPosition[1] = 0.5 - (double)application->ground.originBlock[1];
    // Слой пола занимает [level, level + 1], спавн отсчитывается от его верха.
    outPosition[2] =
        (double)SimulationGroundLocalLevel(&application->ground) + 1.0 + SIMULATION_CUBE_SPAWN_HEIGHT;
}

static void DrawCubes(SimulationApplication *application, const int64_t renderOriginBlock[3])
{
    if (application->cubeMesh == NULL)
    {
        return;
    }
    uint32_t count = SimulationCubeFieldCount(&application->cubes);
    if (count > application->cubeInstanceCapacity)
    {
        RendererMeshInstance *grown = realloc(application->cubeInstances,
                                              (size_t)count * sizeof(*grown));
        if (grown == NULL)
        {
            // Памяти не хватило — рисуем столько, сколько уже помещается.
            count = application->cubeInstanceCapacity;
        }
        else
        {
            application->cubeInstances = grown;
            application->cubeInstanceCapacity = count;
        }
    }
    uint32_t written = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        double origin[3];
        float rotation[4];
        if (!SimulationCubeFieldPlacement(&application->cubes, index, origin, rotation))
        {
            // Куб улетел за пределы локальных координат: рисовать нечем.
            continue;
        }
        RendererMeshInstance *instance = &application->cubeInstances[written++];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            instance->originRelative[axis] =
                (float)(origin[axis] - (double)renderOriginBlock[axis]);
        }
        instance->scale = (float)SIMULATION_CUBE_EXTENT;
        for (int32_t component = 0; component < 4; ++component)
        {
            instance->rotation[component] = rotation[component];
        }
    }
    if (written != 0)
    {
        RendererDrawMeshInstances(application->renderer, application->cubeMesh,
                                  application->cubeInstances, written);
    }
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
    bool profiling = application->profileFile != NULL;
    double frameStart = profiling ? PlatformTimeSeconds() : 0.0;

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

    // Меш куба мог не создаться при нехватке памяти на старте: повтор
    // ничего не стоит, а кубы появятся, как только место найдётся.
    if (application->cubeMesh == NULL)
    {
        application->cubeMesh = CreateCubeMesh(application->renderer);
    }
    double spawnPosition[3];
    CubeSpawnPosition(application, spawnPosition);
    double physicsStart = profiling ? PlatformTimeSeconds() : 0.0;
    SimulationCubeFieldUpdate(&application->cubes, application->world, spawnPosition,
                              (double)deltaSeconds);
    double physicsEnd = profiling ? PlatformTimeSeconds() : 0.0;

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

    double prepareStart = profiling ? PlatformTimeSeconds() : 0.0;
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
        DrawCubes(application, renderOriginBlock);
    }
    double prepareEnd = profiling ? PlatformTimeSeconds() : 0.0;
    bool presented = RendererEndFrame(application->renderer);
    double frameEnd = profiling ? PlatformTimeSeconds() : 0.0;
    if (!presented)
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

    if (profiling && presented)
    {
        ProfileRecordFrame(application, frameEnd - frameStart, physicsEnd - physicsStart,
                           prepareEnd - prepareStart, frameEnd - prepareEnd,
                           SimulationCubeFieldCount(&application->cubes),
                           SimulationCubeFieldAwakeCount(&application->cubes),
                           SimulationCubeFieldLastCandidatePairCount(&application->cubes),
                           SimulationCubeFieldLastContactCount(&application->cubes), frameEnd);
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
    if (application->profileFile != NULL)
    {
        ProfileWriteWindow(application, SimulationCubeFieldCount(&application->cubes),
                           SimulationCubeFieldAwakeCount(&application->cubes),
                           SimulationCubeFieldLastCandidatePairCount(&application->cubes),
                           SimulationCubeFieldLastContactCount(&application->cubes),
                           PlatformTimeSeconds() + 1.0);
        fclose(application->profileFile);
        application->profileFile = NULL;
    }
    if (application->streaming != NULL)
    {
        ChunkStreamingDestroy(application->streaming);
        application->streaming = NULL;
    }
    if (application->cubeMesh != NULL)
    {
        RendererDestroyMesh(application->renderer, application->cubeMesh);
        application->cubeMesh = NULL;
    }
    SimulationCubeFieldRelease(&application->cubes);
    free(application->cubeInstances);
    application->cubeInstances = NULL;
    application->cubeInstanceCapacity = 0u;
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
    application->profileFile = ProfileOpenFromEnvironment();
    if (application->profileFile != NULL)
    {
        fputs("time_seconds,frame_count,bodies,awake,candidate_pairs,contacts,"
              "frame_avg_ms,frame_max_ms,"
              "physics_avg_ms,physics_max_ms,prepare_avg_ms,prepare_max_ms,"
              "present_avg_ms,present_max_ms,fps,samples\n",
              application->profileFile);
        fflush(application->profileFile);
    }

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
    // Пол бесконечен, поэтому он не выкладывается блоками, а вычисляется
    // базовым слоем. Контекст лежит в application и переживает World.
    SimulationGroundProviderInit(&application->ground);
    WorldBaseProvider groundProvider;
    SimulationGroundProviderBind(&application->ground, &groundProvider);
    if (!SimulationCubeFieldInit(&application->cubes))
    {
        DestroyApplication(application);
        return 6;
    }
    application->cubeMesh = CreateCubeMesh(application->renderer);

    application->world = WorldCreate(&groundProvider);
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
