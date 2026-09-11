#include "app/application.h"

#include "game/falling_cubes.h"
#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/ground_provider.h"
#include "game/physics_defaults.h"
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
#include <string.h>

#define SIMULATION_VIEW_RADIUS_CHUNKS 2
#define SIMULATION_FIELD_OF_VIEW_DEGREES 90.0f
#define SIMULATION_CAMERA_SPEED 7.0f
#define SIMULATION_CAMERA_FAST_SPEED 22.0f
#define SIMULATION_MOUSE_SENSITIVITY 0.0022f
#define SIMULATION_MAX_CONSECUTIVE_RENDER_FAILURES 120U
// Высота появления куба над поверхностью пола. Достаточно, чтобы падение
// было видно, и достаточно, чтобы растущая куча до неё не дотянулась.
#define SIMULATION_CUBE_SPAWN_HEIGHT 18.0

// Схема проверки горячей смены бэкенда: сколько кадров показать до смены и
// после неё, и до какого кадра ждать перезаливки куба чанков, прежде чем
// считать прогон неудачным.
#define SIMULATION_BACKEND_SWITCH_WARMUP_FRAMES 3u
#define SIMULATION_BACKEND_SWITCH_SETTLE_FRAMES 3u
#define SIMULATION_BACKEND_SWITCH_MAX_FRAMES 300u

// Имена материалов принадлежат игре, а не движку: текстурпак — папка, и файл
// в ней зовётся так же, как здесь написано. Расширение подбирает движок,
// поэтому в паке может лежать и PNG, и готовый .lt.
static const wchar_t *const SIMULATION_MATERIAL_NAMES[] = {
    L"blocks/foundation",
    L"blocks/marker",
    L"blocks/accent",
};
static const uint32_t SIMULATION_MATERIAL_NAME_COUNT =
    (uint32_t)(sizeof(SIMULATION_MATERIAL_NAMES) / sizeof(SIMULATION_MATERIAL_NAMES[0]));

// Порядок полей подобран так, чтобы не было лишних выравнивающих дыр
// (clang-analyzer-optin.performance.Padding): сначала указатели и 8-байтные
// скаляры, затем структуры, затем 4-байтные счётчики и флаги в конце.
typedef struct SimulationApplication
{
    Window *window;
    Input *input;
    LaiueContentCatalog *content;
    Renderer *renderer;
    World *world;
    ChunkStreaming *streaming;
    LaiueTaskPool *physicsPool;
    RendererMesh *cubeMesh;
    // Кубов сколько угодно, поэтому буфер инстансов тоже растёт.
    RendererMeshInstance *cubeInstances;
    double previousTimeSeconds;
    FILE *profileFile;
    uint64_t profileFrameCount;
    uint64_t profileWindowFrames;
    uint64_t profilePhysicsTicks;
    double profileRunStart;
    double profileWindowStart;
    double profileFrameSum;
    double profileFrameMaximum;
    double profilePhysicsSum;
    double profilePhysicsMaximum;
    double profilePrepareSum;
    double profilePrepareMaximum;
    double profilePresentSum;
    double profilePresentMaximum;
    // Разложение кадра дальше движковых стадий: стриминг заказывает и
    // забирает меши, DrawCubes готовит инстансы, ChunkStreamingDraw их
    // рисует. Всё это части frame, но они не входят в physics/prepare/present
    // по отдельности, поэтому измеряются отдельно.
    double profileStreamingSum;
    double profileStreamingMaximum;
    double profileInstancesSum;
    double profileInstancesMaximum;
    double profileStreamDrawSum;
    double profileStreamDrawMaximum;
    double profileBetweenSum;
    double profileBetweenMaximum;
    // Замер запуска (SOS_STARTUP_PROFILE): время стадий и детерминированные
    // счётчики до первого кадра с полностью обработанным кубом чанков.
    FILE *startupFile;
    double startupOriginSeconds;
    double startupPreviousSeconds;
    double switchDestroySeconds;
    double switchCreateSeconds;
    // Базовый слой обязан пережить World: движок хранит указатель на него.
    SimulationGroundProvider ground;
    LaiueTaskExecutor physicsExecutor;
    Camera camera;
    SimulationCubeField cubes;
    uint32_t physicsThreadCount;
    uint32_t cubeInstanceCapacity;
    int32_t windowWidth;
    int32_t windowHeight;
    uint32_t maximumPresentedFrames;
    uint32_t presentedFrames;
    uint32_t consecutiveRenderFailures;
    uint32_t profileSecondsLimit;
    uint32_t startupFrames;
    // Режим запуска и признак того, что цикл кадров уже начался: до него
    // создание рендер-сессии пишет стадии запуска, после — только лог.
    SimulationRunMode mode;
    // Код ошибки последней неудачной RenderSessionCreate (5 или 7), чтобы
    // стартовый путь сохранил прежние коды возврата 2..7.
    int sessionErrorCode;
    // Схема проверки горячей смены бэкенда.
    uint32_t switchPhase;
    uint32_t switchFrame;
    uint32_t refusalFrame;
    int exitCode;
    PanoramaCache panorama;
    bool startupFilled;
    bool runLoopStarted;
    bool switchFillObserved;
} SimulationApplication;

static const char *RenderBackendName(RendererBackendKind backend)
{
    switch (backend)
    {
    case RENDERER_BACKEND_D3D12:
        return "d3d12";
    case RENDERER_BACKEND_VULKAN:
        return "vulkan";
    case RENDERER_BACKEND_AUTO:
        break;
    }
    return "auto";
}

// У графического клиента нет консоли, но сообщения всё равно пишем в stderr:
// при запуске из терминала и в тестах их видно, а в остальных случаях это
// безвредная запись в унаследованный дескриптор. На MSVC/clang-cl берём
// защищённый вариант, чтобы не спорить с clang-tidy.
#if defined(_MSC_VER)
#define SIMULATION_BACKEND_PRINT fprintf_s
#else
#define SIMULATION_BACKEND_PRINT fprintf
#endif

static void RenderBackendLog(const char *message)
{
    (void)SIMULATION_BACKEND_PRINT(stderr, "[render-backend] %s\n", message);
    (void)fflush(stderr);
}

static void RenderBackendLogActive(const char *prefix, RendererBackendKind backend)
{
    (void)SIMULATION_BACKEND_PRINT(stderr, "[render-backend] %s: %s\n", prefix,
                                   RenderBackendName(backend));
    (void)fflush(stderr);
}

// SOS_RENDER_BACKEND=auto|d3d12|vulkan. Неизвестное значение и отсутствие
// переменной — AUTO, как раньше. Разбор по образцу SOS_PHYSICS_THREADS.
static RendererBackendKind RenderBackendFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *owned = NULL;
    size_t ownedBytes = 0u;
    (void)_dupenv_s(&owned, &ownedBytes, "SOS_RENDER_BACKEND");
    const char *text = owned;
#else
    const char *text = getenv("SOS_RENDER_BACKEND");
#endif
    RendererBackendKind backend = RENDERER_BACKEND_AUTO;
    if (text != NULL)
    {
        if (strcmp(text, "d3d12") == 0)
        {
            backend = RENDERER_BACKEND_D3D12;
        }
        else if (strcmp(text, "vulkan") == 0)
        {
            backend = RENDERER_BACKEND_VULKAN;
        }
    }
#if defined(_MSC_VER)
    free(owned);
#endif
    return backend;
}

static uint32_t PhysicsThreadCountFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *owned = NULL;
    size_t ownedBytes = 0u;
    (void)_dupenv_s(&owned, &ownedBytes, "SOS_PHYSICS_THREADS");
    const char *text = owned;
#else
    const char *text = getenv("SOS_PHYSICS_THREADS");
#endif
    uint32_t value = SimulationPhysicsThreads(text, LaiueTaskLogicalProcessorCount());
#if defined(_MSC_VER)
    free(owned);
#endif
    return value;
}

static VoxelRigidSolverOrder SolverOrderFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *owned = NULL;
    size_t ownedBytes = 0u;
    (void)_dupenv_s(&owned, &ownedBytes, "SOS_PHYSICS_SOLVER");
    const char *text = owned;
#else
    const char *text = getenv("SOS_PHYSICS_SOLVER");
#endif
    VoxelRigidSolverOrder order = text != NULL && strcmp(text, "canonical") == 0
                                      ? VOXEL_RIGID_SOLVER_CANONICAL
                                      : VOXEL_RIGID_SOLVER_COLORED;
#if defined(_MSC_VER)
    free(owned);
#endif
    return order;
}

static void ProfileWriteWindow(SimulationApplication *application, double now, bool flushPartial)
{
    if (application == NULL || application->profileFile == NULL ||
        application->profileWindowFrames == 0u ||
        (!flushPartial && now - application->profileWindowStart < 1.0))
    {
        return;
    }
    // Счётчики читаются только здесь, а не на каждом кадре: обход тел ради
    // «бодрствующих» стоит O(n) и без нужды удорожал бы кадр.
    uint32_t bodyCount = SimulationCubeFieldCount(&application->cubes);
    uint32_t awakeCount = SimulationCubeFieldAwakeCount(&application->cubes);
    uint32_t candidatePairCount = SimulationCubeFieldLastCandidatePairCount(&application->cubes);
    uint32_t contactCount = SimulationCubeFieldLastContactCount(&application->cubes);
    double frames = (double)application->profileWindowFrames;
#if defined(_MSC_VER)
#define SIMULATION_PROFILE_PRINT fprintf_s
#else
#define SIMULATION_PROFILE_PRINT fprintf
#endif
    (void)SIMULATION_PROFILE_PRINT(
        application->profileFile,
        "%.6f,%llu,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%.6f,%u,%u,%"
        "u,%u,%u,%u,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s\n",
        now, (unsigned long long)application->profileFrameCount, bodyCount, awakeCount,
        candidatePairCount, contactCount, application->profileFrameSum * 1000.0 / frames,
        application->profileFrameMaximum * 1000.0, application->profilePhysicsSum * 1000.0 / frames,
        application->profilePhysicsMaximum * 1000.0,
        application->profilePrepareSum * 1000.0 / frames,
        application->profilePrepareMaximum * 1000.0,
        application->profilePresentSum * 1000.0 / frames,
        application->profilePresentMaximum * 1000.0,
        (double)application->profileWindowFrames / (now - application->profileWindowStart), frames,
        (unsigned long long)application->profilePhysicsTicks,
        application->profilePhysicsTicks != 0u
            ? application->profilePhysicsSum * 1000.0 / (double)application->profilePhysicsTicks
            : 0.0,
        application->cubes.contactCache.matchedContactCount,
        application->cubes.broadphase.proxyCount, application->cubes.broadphase.updatedProxyCount,
        application->cubes.broadphase.visitedNodeCount,
        application->cubes.useSpatialIndex ? 1u : 0u, application->physicsThreadCount,
        application->cubes.stepOptions.solverOrder == VOXEL_RIGID_SOLVER_CANONICAL ? "canonical"
                                                                                   : "colored",
        application->profileStreamingSum * 1000.0 / frames,
        application->profileStreamingMaximum * 1000.0,
        application->profileInstancesSum * 1000.0 / frames,
        application->profileInstancesMaximum * 1000.0,
        application->profileStreamDrawSum * 1000.0 / frames,
        application->profileStreamDrawMaximum * 1000.0,
        application->profileBetweenSum * 1000.0 / frames,
        application->profileBetweenMaximum * 1000.0,
        application->renderer != NULL ? RenderBackendName(RendererGetBackend(application->renderer))
                                      : "none");
#undef SIMULATION_PROFILE_PRINT
    fflush(application->profileFile);
    application->profileWindowStart = now;
    application->profileWindowFrames = 0u;
    application->profilePhysicsTicks = 0u;
    application->profileFrameSum = 0.0;
    application->profileFrameMaximum = 0.0;
    application->profilePhysicsSum = 0.0;
    application->profilePhysicsMaximum = 0.0;
    application->profilePrepareSum = 0.0;
    application->profilePrepareMaximum = 0.0;
    application->profilePresentSum = 0.0;
    application->profilePresentMaximum = 0.0;
    application->profileStreamingSum = 0.0;
    application->profileStreamingMaximum = 0.0;
    application->profileInstancesSum = 0.0;
    application->profileInstancesMaximum = 0.0;
    application->profileStreamDrawSum = 0.0;
    application->profileStreamDrawMaximum = 0.0;
    application->profileBetweenSum = 0.0;
    application->profileBetweenMaximum = 0.0;
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

// Запуск: SOS_STARTUP_PROFILE=<путь> включает запись времени стадий создания
// окна, контента, рендера, мира и стриминга, а также момента, когда куб
// чанков впервые полностью обработан. Файл — CSV:
// stage,elapsed,delta,queued,uploaded,builds,peak,build_avg_ms,frames,bodies.
static FILE *StartupOpenFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *startupPath = NULL;
    size_t startupPathBytes = 0u;
    FILE *startupFile = NULL;
    if (_dupenv_s(&startupPath, &startupPathBytes, "SOS_STARTUP_PROFILE") == 0 &&
        startupPath != NULL && startupPath[0] != '\0')
    {
        (void)fopen_s(&startupFile, startupPath, "wb");
    }
    free(startupPath);
    return startupFile;
#else
    const char *startupPath = getenv("SOS_STARTUP_PROFILE");
    if (startupPath == NULL || startupPath[0] == '\0')
    {
        return NULL;
    }
    return fopen(startupPath, "wb");
#endif
}

// SOS_NO_VSYNC=1 отключает вертикальную синхронизацию: present перестаёт
// включать ожидание кадрового импульса и в профиле видно настоящую работу.
static bool NoVerticalSyncFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *ownedText = NULL;
    size_t textBytes = 0u;
    (void)_dupenv_s(&ownedText, &textBytes, "SOS_NO_VSYNC");
    const char *text = ownedText;
#else
    const char *text = getenv("SOS_NO_VSYNC");
#endif
    bool disabled = text != NULL && text[0] != '\0' && text[0] != '0';
#if defined(_MSC_VER)
    free(ownedText);
#endif
    return disabled;
}

static void StartupWriteStage(SimulationApplication *application, const char *stage)
{
    if (application == NULL || application->startupFile == NULL)
    {
        return;
    }
    double now = PlatformTimeSeconds();
    double elapsed = now - application->startupOriginSeconds;
    double delta = now - application->startupPreviousSeconds;
    application->startupPreviousSeconds = now;
#if defined(_MSC_VER)
#define SIMULATION_STARTUP_PRINT fprintf_s
#else
#define SIMULATION_STARTUP_PRINT fprintf
#endif
    (void)SIMULATION_STARTUP_PRINT(application->startupFile,
                                   "%s,%.6f,%.6f,0,0,0,0,0.000000,0,0\n", stage, elapsed, delta);
#undef SIMULATION_STARTUP_PRINT
    fflush(application->startupFile);
}

static void StartupWriteFilled(SimulationApplication *application)
{
    if (application == NULL || application->startupFile == NULL || application->startupFilled)
    {
        return;
    }
    ChunkStreamingStats stats;
    ChunkStreamingGetStats(application->streaming, &stats);
    double now = PlatformTimeSeconds();
    double elapsed = now - application->startupOriginSeconds;
    double delta = now - application->startupPreviousSeconds;
    application->startupPreviousSeconds = now;
#if defined(_MSC_VER)
#define SIMULATION_STARTUP_PRINT fprintf_s
#else
#define SIMULATION_STARTUP_PRINT fprintf
#endif
    (void)SIMULATION_STARTUP_PRINT(
        application->startupFile, "first_filled,%.6f,%.6f,%llu,%llu,%llu,%u,%.6f,%u,%u\n",
        elapsed, delta, (unsigned long long)stats.queuedRequests,
        (unsigned long long)stats.uploadedMeshes, (unsigned long long)stats.completedBuilds,
        stats.peakUnfinishedWork, stats.averageBuildMilliseconds, application->startupFrames,
        SimulationCubeFieldCount(&application->cubes));
#undef SIMULATION_STARTUP_PRINT
    fflush(application->startupFile);
    application->startupFilled = true;
}

static uint32_t ProfileSecondsLimitFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *ownedText = NULL;
    size_t textBytes = 0u;
    (void)_dupenv_s(&ownedText, &textBytes, "SOS_PROFILE_SECONDS");
    const char *text = ownedText;
#else
    const char *text = getenv("SOS_PROFILE_SECONDS");
#endif
    uint32_t seconds = 0u;
    if (text != NULL)
    {
        for (const char *digit = text; *digit != '\0'; ++digit)
        {
            if (*digit < '0' || *digit > '9' || seconds > 8640u)
            {
                seconds = 0u;
                break;
            }
            seconds = seconds * 10u + (uint32_t)(*digit - '0');
        }
    }
#if defined(_MSC_VER)
    free(ownedText);
#endif
    return seconds <= 86400u ? seconds : 0u;
}

static bool UseSpatialIndexFromEnvironment(void)
{
#if defined(_MSC_VER)
    char *ownedText = NULL;
    size_t textBytes = 0u;
    (void)_dupenv_s(&ownedText, &textBytes, "SOS_PHYSICS_BROADPHASE");
    bool useIndex = SimulationUseSpatialIndex(ownedText);
    free(ownedText);
#else
    bool useIndex = SimulationUseSpatialIndex(getenv("SOS_PHYSICS_BROADPHASE"));
#endif
    return useIndex;
}

typedef struct ProfileFrameTiming
{
    double frameSeconds;
    double physicsSeconds;
    double streamingSeconds;
    double instancesSeconds;
    double streamDrawSeconds;
    double prepareSeconds;
    double presentSeconds;
    double now;
    uint64_t physicsTicks;
} ProfileFrameTiming;

static void ProfileRecordFrame(SimulationApplication *application, const ProfileFrameTiming *timing)
{
    if (application == NULL || application->profileFile == NULL)
    {
        return;
    }
    if (application->profileFrameCount == 0u)
    {
        application->profileRunStart = timing->now - timing->frameSeconds;
    }
    if (application->profileWindowFrames == 0u)
    {
        application->profileWindowStart = timing->now - timing->frameSeconds;
    }
    ++application->profileFrameCount;
    ++application->profileWindowFrames;
    application->profilePhysicsTicks += timing->physicsTicks;
    application->profileFrameSum += timing->frameSeconds;
    if (timing->frameSeconds > application->profileFrameMaximum)
    {
        application->profileFrameMaximum = timing->frameSeconds;
    }
    application->profilePhysicsSum += timing->physicsSeconds;
    if (timing->physicsSeconds > application->profilePhysicsMaximum)
    {
        application->profilePhysicsMaximum = timing->physicsSeconds;
    }
    application->profilePrepareSum += timing->prepareSeconds;
    if (timing->prepareSeconds > application->profilePrepareMaximum)
    {
        application->profilePrepareMaximum = timing->prepareSeconds;
    }
    application->profilePresentSum += timing->presentSeconds;
    if (timing->presentSeconds > application->profilePresentMaximum)
    {
        application->profilePresentMaximum = timing->presentSeconds;
    }
    application->profileStreamingSum += timing->streamingSeconds;
    if (timing->streamingSeconds > application->profileStreamingMaximum)
    {
        application->profileStreamingMaximum = timing->streamingSeconds;
    }
    application->profileInstancesSum += timing->instancesSeconds;
    if (timing->instancesSeconds > application->profileInstancesMaximum)
    {
        application->profileInstancesMaximum = timing->instancesSeconds;
    }
    application->profileStreamDrawSum += timing->streamDrawSeconds;
    if (timing->streamDrawSeconds > application->profileStreamDrawMaximum)
    {
        application->profileStreamDrawMaximum = timing->streamDrawSeconds;
    }
    // «Между» — всё, что не попало в физику, стриминг, prepare и present:
    // подготовка кадра, камера, смена origin и служебные вызовы кадра.
    // prepare уже включает инстансы и Draw стриминга, поэтому они вычтены
    // не повторно, а показаны отдельными колонками.
    double betweenSeconds = timing->frameSeconds - timing->physicsSeconds -
                            timing->streamingSeconds - timing->prepareSeconds -
                            timing->presentSeconds;
    if (betweenSeconds < 0.0)
    {
        betweenSeconds = 0.0;
    }
    application->profileBetweenSum += betweenSeconds;
    if (betweenSeconds > application->profileBetweenMaximum)
    {
        application->profileBetweenMaximum = betweenSeconds;
    }
    ProfileWriteWindow(application, timing->now, false);
    if (application->profileSecondsLimit != 0u &&
        timing->now - application->profileRunStart >= (double)application->profileSecondsLimit)
    {
        WindowRequestClose(application->window);
    }
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
        quads[face] =
            PackChunkQuad(0u, 0u, 0u, face, (uint32_t)SIMULATION_MATERIAL_ACCENT, 1u, 1u, 1u);
    }
    return RendererCreateMesh(renderer, quads, 6u);
}

// Уничтожение рендер-сессии в порядке, обратном созданию: стриминг держит
// RendererMesh* и обязан уйти первым, затем меш куба и сам рендерер. Мир,
// тела, камера и ввод сессии не принадлежат.
static void RenderSessionDestroy(SimulationApplication *application)
{
    if (application == NULL)
    {
        return;
    }
    if (application->streaming != NULL)
    {
        ChunkStreamingDestroy(application->streaming);
        application->streaming = NULL;
    }
    if (application->cubeMesh != NULL)
    {
        if (application->renderer != NULL)
        {
            RendererDestroyMesh(application->renderer, application->cubeMesh);
        }
        application->cubeMesh = NULL;
    }
    if (application->renderer != NULL)
    {
        RendererDestroy(application->renderer);
        application->renderer = NULL;
    }
}

// Создание всей рендер-сессии: рендерер выбранного бэкенда, имена
// материалов, подготовка мира, vsync, меш куба, стриминг чанков и его центр
// на текущей позиции камеры. При любой ошибке всё уже созданное снимается, а
// application->sessionErrorCode хранит прежний код возврата старта (5 —
// рендерер/материалы/мир, 7 — стриминг). Стадии запуска пишутся только до
// входа в цикл кадров, чтобы горячая смена не засоряла startup-профиль.
static bool RenderSessionCreate(SimulationApplication *application, RendererBackendKind requested)
{
    if (application == NULL || application->window == NULL)
    {
        return false;
    }
    application->sessionErrorCode = 5;
    application->renderer = RendererCreateWithBackend(WindowGetNativeHandle(application->window),
                                                      application->windowWidth,
                                                      application->windowHeight, requested);
    if (!application->runLoopStarted)
    {
        StartupWriteStage(application, "renderer_create");
    }
    if (application->renderer == NULL)
    {
        RenderSessionDestroy(application);
        return false;
    }
    if (!RendererSetMaterialNames(application->renderer, SIMULATION_MATERIAL_NAMES,
                                  SIMULATION_MATERIAL_NAME_COUNT))
    {
        RenderSessionDestroy(application);
        return false;
    }
    if (!application->runLoopStarted)
    {
        StartupWriteStage(application, "renderer_material_names");
    }
    if (!RendererPrepareWorldFrom(application->renderer, application->content))
    {
        RenderSessionDestroy(application);
        return false;
    }
    if (!application->runLoopStarted)
    {
        StartupWriteStage(application, "renderer_prepare_world");
    }
    // По умолчанию кадр ждёт кадровый импульс; SOS_NO_VSYNC=1 убирает это
    // ожидание, чтобы профиль показывал работу, а не паузу до вертикали.
    RendererSetVerticalSync(application->renderer, !NoVerticalSyncFromEnvironment());
    // Меш куба мог не создаться при нехватке памяти: это не отказ сессии —
    // OnFrame повторит попытку, как только место найдётся.
    application->cubeMesh = CreateCubeMesh(application->renderer);
    if (!application->runLoopStarted)
    {
        StartupWriteStage(application, "cube_mesh_create");
    }
    if (application->world == NULL)
    {
        application->sessionErrorCode = 7;
        RenderSessionDestroy(application);
        return false;
    }
    application->streaming = ChunkStreamingCreate(application->world, application->renderer,
                                                  SIMULATION_VIEW_RADIUS_CHUNKS);
    if (application->streaming == NULL)
    {
        application->sessionErrorCode = 7;
        RenderSessionDestroy(application);
        return false;
    }
    if (!application->runLoopStarted)
    {
        StartupWriteStage(application, "streaming_create");
    }
    // Иначе после смены куб чанков был бы пустым до первого движения камеры.
    int64_t center[3];
    StreamingCenter(&application->camera, center);
    ChunkStreamingSetCenter(application->streaming, center[0], center[1], center[2]);
    application->sessionErrorCode = 0;
    return true;
}

// Горячая смена бэкенда. Целевой бэкенд сначала проверяется на доступность
// (недоступный — только лог и отказ, сессия остаётся как была). При неудаче
// создания целевого восстанавливается прежний; если не поднимается и он,
// приложение закрывается с кодом ошибки, а не падает.
static bool RenderSessionSwitchBackend(SimulationApplication *application, RendererBackendKind target)
{
    if (application == NULL || application->renderer == NULL)
    {
        return false;
    }
    RendererBackendKind previous = RendererGetBackend(application->renderer);
    if (!RendererBackendIsAvailable(target))
    {
        RenderBackendLog("requested backend is not available in this build; keeping current");
        return false;
    }
    double destroyStart = PlatformTimeSeconds();
    RenderSessionDestroy(application);
    double createStart = PlatformTimeSeconds();
    bool created = RenderSessionCreate(application, target);
    double createdEnd = PlatformTimeSeconds();
    application->switchDestroySeconds = createStart - destroyStart;
    application->switchCreateSeconds = createdEnd - createStart;
    if (!created)
    {
        RenderBackendLog("failed to create the target backend; restoring the previous one");
        if (!RenderSessionCreate(application, previous))
        {
            RenderBackendLog("failed to restore the previous backend; closing");
            application->exitCode = 11;
            WindowRequestClose(application->window);
            return false;
        }
        return false;
    }
    (void)SIMULATION_BACKEND_PRINT(
        stderr, "[render-backend] switch timing: destroy=%.3f ms create=%.3f ms backend=%s\n",
        application->switchDestroySeconds * 1000.0, application->switchCreateSeconds * 1000.0,
        RenderBackendName(RendererGetBackend(application->renderer)));
    (void)fflush(stderr);
    application->consecutiveRenderFailures = 0u;
    return true;
}

static RendererBackendKind RenderBackendOther(RendererBackendKind current)
{
    return current == RENDERER_BACKEND_D3D12 ? RENDERER_BACKEND_VULKAN : RENDERER_BACKEND_D3D12;
}

// Первым делом предлагает второй бэкенд относительно текущего: именно его
// отсутствие и должен корректно пережить клиент.
static bool RenderBackendUnavailable(RendererBackendKind current, RendererBackendKind *outBackend)
{
    if (!RendererBackendIsAvailable(RenderBackendOther(current)))
    {
        *outBackend = RenderBackendOther(current);
        return true;
    }
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        *outBackend = RENDERER_BACKEND_D3D12;
        return true;
    }
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        *outBackend = RENDERER_BACKEND_VULKAN;
        return true;
    }
    return false;
}

// Схема проверки горячей смены: 3 кадра прогрева, смена на второй доступный
// бэкенд (или пересоздание того же, если второго нет), ожидание перезаливки
// куба чанков, затем попытка переключиться на недоступный бэкенд и ещё 3
// кадра, подтверждающие, что отказ ничего не сломал.
static void RenderBackendSwitchSmokeTick(SimulationApplication *application)
{
    if (application == NULL || application->renderer == NULL)
    {
        return;
    }
    if (application->presentedFrames > SIMULATION_BACKEND_SWITCH_MAX_FRAMES)
    {
        application->exitCode = 12;
        WindowRequestClose(application->window);
        return;
    }
    switch (application->switchPhase)
    {
    case 0u:
        if (application->presentedFrames >= SIMULATION_BACKEND_SWITCH_WARMUP_FRAMES)
        {
            RendererBackendKind current = RendererGetBackend(application->renderer);
            RendererBackendKind other = RenderBackendOther(current);
            // Если второго бэкенда в сборке нет, честно проверяем само
            // пересоздание на том же.
            RendererBackendKind target = RendererBackendIsAvailable(other) ? other : current;
            bool switched = RenderSessionSwitchBackend(application, target);
            RenderBackendLog(switched ? "backend switch performed"
                                      : "backend switch fell back to the current backend");
            application->switchFrame = application->presentedFrames;
            application->switchPhase = 1u;
        }
        break;
    case 1u:
    {
        if (application->streaming == NULL)
        {
            application->exitCode = 12;
            WindowRequestClose(application->window);
            return;
        }
        ChunkStreamingStats stats;
        ChunkStreamingGetStats(application->streaming, &stats);
        if (!application->switchFillObserved && stats.uploadedMeshes > 0u)
        {
            application->switchFillObserved = true;
            (void)SIMULATION_BACKEND_PRINT(
                stderr, "[render-backend] chunk cube refilled after %u frames (%llu meshes)\n",
                (unsigned)(application->presentedFrames - application->switchFrame),
                (unsigned long long)stats.uploadedMeshes);
            (void)fflush(stderr);
        }
        bool settled = application->presentedFrames >=
                       application->switchFrame + SIMULATION_BACKEND_SWITCH_SETTLE_FRAMES;
        if (application->switchFillObserved && settled)
        {
            RendererBackendKind before = RendererGetBackend(application->renderer);
            RendererBackendKind unavailable;
            if (RenderBackendUnavailable(before, &unavailable) &&
                RenderSessionSwitchBackend(application, unavailable))
            {
                // Недоступный бэкенд не мог смениться: это ошибка проверки.
                application->exitCode = 12;
                WindowRequestClose(application->window);
                return;
            }
            application->refusalFrame = application->presentedFrames;
            application->switchPhase = 2u;
        }
        else if (application->presentedFrames >
                 application->switchFrame + SIMULATION_BACKEND_SWITCH_MAX_FRAMES)
        {
            application->exitCode = 12;
            WindowRequestClose(application->window);
        }
        break;
    }
    case 2u:
        if (application->presentedFrames >=
            application->refusalFrame + SIMULATION_BACKEND_SWITCH_SETTLE_FRAMES)
        {
            application->switchPhase = 3u;
            WindowRequestClose(application->window);
        }
        break;
    default:
        break;
    }
}

// Центр мира в локальных координатах: абсолютный ноль минус то, насколько
// локальная сетка от него уехала.
static void CubeSpawnPosition(const SimulationApplication *application, double outPosition[3])
{
    outPosition[0] = 0.5 - (double)application->ground.originBlock[0];
    outPosition[1] = 0.5 - (double)application->ground.originBlock[1];
    // Слой пола занимает [level, level + 1], спавн отсчитывается от его верха.
    outPosition[2] = (double)SimulationGroundLocalLevel(&application->ground) + 1.0 +
                     SIMULATION_CUBE_SPAWN_HEIGHT;
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
        // Удвоение, а не рост ровно под текущее число кубов: иначе буфер
        // перевыделялся бы почти на каждом кадре, где появился новый куб.
        uint32_t capacity =
            SimulationGrownCapacity(application->cubeInstanceCapacity, count);
        RendererMeshInstance *grown =
            realloc(application->cubeInstances, (size_t)capacity * sizeof(*grown));
        if (grown == NULL)
        {
            // Памяти не хватило — рисуем столько, сколько уже помещается.
            count = application->cubeInstanceCapacity;
        }
        else
        {
            application->cubeInstances = grown;
            application->cubeInstanceCapacity = capacity;
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
    if (application->startupFile != NULL)
    {
        application->startupFrames++;
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
    // Горячая смена бэкенда — в начале кадра, до стриминга и BeginFrame:
    // мир, тела, камера и ввод при этом не трогаются.
    if (InputConsumeKeyPress(application->input, INPUT_KEY_F7))
    {
        RendererBackendKind current = RendererGetBackend(application->renderer);
        RendererBackendKind target = RenderBackendOther(current);
        if (!RendererBackendIsAvailable(target))
        {
            RenderBackendLog("F7 ignored: the other backend is not available");
        }
        else
        {
            (void)RenderSessionSwitchBackend(application, target);
        }
    }
    if (application->mode == SIMULATION_RUN_BACKEND_SWITCH_SMOKE)
    {
        RenderBackendSwitchSmokeTick(application);
    }
    if (application->exitCode != 0 || application->renderer == NULL)
    {
        // Сессию поднять не удалось (или проверка уже провалена): без
        // рендера кадр продолжать нечем.
        InputEndFrame(application->input);
        return;
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
    double streamingStart = profiling ? PlatformTimeSeconds() : 0.0;
    ChunkStreamingSetCenter(application->streaming,
                            SimulationBlockToChunkFloor(renderOriginBlock[0]),
                            SimulationBlockToChunkFloor(renderOriginBlock[1]),
                            SimulationBlockToChunkFloor(renderOriginBlock[2]));
    ChunkStreamingPump(application->streaming);
    double streamingEnd = profiling ? PlatformTimeSeconds() : 0.0;

    // Куб чанков считается заполненным, когда все заявленные меши построены
    // и забраны: ни заявок, ни результатов, ни работ в полёте.
    if (application->startupFile != NULL && !application->startupFilled)
    {
        ChunkStreamingStats fillStats;
        ChunkStreamingGetStats(application->streaming, &fillStats);
        if (fillStats.queuedRequests > 0u &&
            fillStats.queuedRequests == fillStats.completedBuilds &&
            fillStats.pendingRequests == 0u && fillStats.pendingResults == 0u)
        {
            StartupWriteFilled(application);
            WindowRequestClose(application->window);
        }
    }

    // Меш куба мог не создаться при нехватке памяти на старте: повтор
    // ничего не стоит, а кубы появятся, как только место найдётся.
    if (application->cubeMesh == NULL)
    {
        application->cubeMesh = CreateCubeMesh(application->renderer);
    }
    double spawnPosition[3];
    CubeSpawnPosition(application, spawnPosition);
    double physicsStart = profiling ? PlatformTimeSeconds() : 0.0;
    uint64_t ticksBeforeFrame = application->cubes.tickCount;
    SimulationCubeFieldUpdate(&application->cubes, application->world, spawnPosition,
                              (double)deltaSeconds);
    double physicsEnd = profiling ? PlatformTimeSeconds() : 0.0;
    if (application->cubes.failed)
    {
        application->exitCode = 10;
        WindowRequestClose(application->window);
        InputEndFrame(application->input);
        return;
    }

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

    double instancesSum = 0.0;
    double streamDrawSum = 0.0;
    for (uint32_t pass = 0; pass < frame.passCount; ++pass)
    {
        RendererBeginScenePass(application->renderer, pass);
        double drawStart = profiling ? PlatformTimeSeconds() : 0.0;
        ChunkStreamingDraw(application->streaming, frame.passes[pass].viewProjection,
                           renderOriginBlock);
        double drawMiddle = profiling ? PlatformTimeSeconds() : 0.0;
        DrawCubes(application, renderOriginBlock);
        double drawEnd = profiling ? PlatformTimeSeconds() : 0.0;
        streamDrawSum += drawMiddle - drawStart;
        instancesSum += drawEnd - drawMiddle;
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
        const ProfileFrameTiming timing = {
            .frameSeconds = frameEnd - frameStart,
            .physicsSeconds = physicsEnd - physicsStart,
            .streamingSeconds = streamingEnd - streamingStart,
            .instancesSeconds = instancesSum,
            .streamDrawSeconds = streamDrawSum,
            .prepareSeconds = prepareEnd - prepareStart,
            .presentSeconds = frameEnd - prepareEnd,
            .now = frameEnd,
            .physicsTicks = application->cubes.tickCount - ticksBeforeFrame,
        };
        ProfileRecordFrame(application, &timing);
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
        ProfileWriteWindow(application, PlatformTimeSeconds(), true);
        fclose(application->profileFile);
        application->profileFile = NULL;
    }
    if (application->startupFile != NULL)
    {
        fclose(application->startupFile);
        application->startupFile = NULL;
    }
    // Стриминг обязан уйти раньше мира: рабочие потоки держат World*.
    RenderSessionDestroy(application);
    SimulationCubeFieldRelease(&application->cubes);
    LaiueTaskPoolDestroy(application->physicsPool);
    application->physicsPool = NULL;
    free(application->cubeInstances);
    application->cubeInstances = NULL;
    application->cubeInstanceCapacity = 0u;
    if (application->world != NULL)
    {
        WorldDestroy(application->world);
        application->world = NULL;
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
    if (mode < SIMULATION_RUN_INTERACTIVE || mode > SIMULATION_RUN_BACKEND_SWITCH_SMOKE)
    {
        return 1;
    }
    SimulationApplication *application = calloc(1, sizeof(*application));
    if (application == NULL)
    {
        return 1;
    }
    application->mode = mode;
    application->maximumPresentedFrames =
        mode == SIMULATION_RUN_INTERACTIVE
            ? 0U
            : (mode == SIMULATION_RUN_BACKEND_SWITCH_SMOKE
                   ? SIMULATION_BACKEND_SWITCH_MAX_FRAMES
                   : 3U);
    application->exitCode = 0;
    application->startupOriginSeconds = PlatformTimeSeconds();
    application->startupPreviousSeconds = application->startupOriginSeconds;
    application->profileFile = ProfileOpenFromEnvironment();
    application->profileSecondsLimit = ProfileSecondsLimitFromEnvironment();
    if (application->profileFile != NULL)
    {
        fputs("time_seconds,frame_count,bodies,awake,candidate_pairs,contacts,"
              "frame_avg_ms,frame_max_ms,"
              "physics_avg_ms,physics_max_ms,prepare_avg_ms,prepare_max_ms,"
              "present_avg_ms,present_max_ms,fps,samples,physics_ticks,physics_tick_avg_ms,"
              "warm_contacts,index_proxies,index_updates,index_visits,indexed,physics_threads,"
              "physics_solver,"
              "streaming_avg_ms,streaming_max_ms,instances_avg_ms,instances_max_ms,"
              "streamdraw_avg_ms,streamdraw_max_ms,between_avg_ms,between_max_ms,"
              "render_backend\n",
              application->profileFile);
        fflush(application->profileFile);
    }
    application->startupFile = StartupOpenFromEnvironment();
    if (application->startupFile != NULL)
    {
        fputs("stage,elapsed_seconds,delta_seconds,queued_chunks,uploaded_meshes,"
              "completed_builds,peak_unfinished,build_avg_ms,frames,bodies\n",
              application->startupFile);
        fflush(application->startupFile);
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
    StartupWriteStage(application, "window_create");
    application->input = InputCreate(WindowGetNativeHandle(application->window));
    if (application->input == NULL)
    {
        DestroyApplication(application);
        return 3;
    }
    StartupWriteStage(application, "input_create");
    WindowGetClientSize(application->window, &application->windowWidth, &application->windowHeight);
    application->content = LaiueContentCatalogCreate(NULL);
    if (application->content == NULL)
    {
        DestroyApplication(application);
        return 4;
    }
    StartupWriteStage(application, "content_catalog");
    // Мир, тела и камера не зависят от рендера, но RenderSessionCreate
    // поднимает стриминг чанков и задаёт ему центр по камере — поэтому они
    // создаются до рендер-сессии. Имена стадий запуска сохранены.
    SimulationGroundProviderInit(&application->ground);
    WorldBaseProvider groundProvider;
    SimulationGroundProviderBind(&application->ground, &groundProvider);
    if (!SimulationCubeFieldInit(&application->cubes))
    {
        DestroyApplication(application);
        return 6;
    }
    StartupWriteStage(application, "cube_field_init");
    uint32_t physicsThreads = PhysicsThreadCountFromEnvironment();
    application->physicsThreadCount = 1u;
    application->physicsExecutor.structSize = sizeof(application->physicsExecutor);
    if (physicsThreads > 1u)
    {
        application->physicsPool = LaiueTaskPoolCreate(physicsThreads);
        if (application->physicsPool != NULL &&
            LaiueTaskPoolGetExecutor(application->physicsPool, &application->physicsExecutor))
        {
            application->cubes.stepOptions.executor = &application->physicsExecutor;
            application->physicsThreadCount = physicsThreads;
        }
        else
        {
            LaiueTaskPoolDestroy(application->physicsPool);
            application->physicsPool = NULL;
        }
    }
    application->cubes.stepOptions.solverOrder = SolverOrderFromEnvironment();
    StartupWriteStage(application, "physics_pool");
    application->world = WorldCreate(&groundProvider);
    if (application->world == NULL)
    {
        DestroyApplication(application);
        return 6;
    }
    StartupWriteStage(application, "world_create");
    if (!SimulationFoundationWorldPopulate(application->world))
    {
        DestroyApplication(application);
        return 6;
    }
    StartupWriteStage(application, "world_populate");
    if (mode == SIMULATION_RUN_REBASE_RENDER_SMOKE &&
        !WorldRebase(application->world,
                     -(int64_t)(SIMULATION_REBASE_THRESHOLD_CHUNKS * CHUNK_SIZE), 0, 0))
    {
        DestroyApplication(application);
        return 6;
    }
    double initialCameraX = mode == SIMULATION_RUN_REBASE_RENDER_SMOKE
                                ? (double)(SIMULATION_REBASE_THRESHOLD_CHUNKS * CHUNK_SIZE) + 1.25
                                : 0.0;
    CameraInit(&application->camera, initialCameraX, -10.0, 4.0, 0.0f, -0.18f);
    StartupWriteStage(application, "camera_init");

    // Выбор бэкенда при запуске: SOS_RENDER_BACKEND. Запрошенный, но
    // недоступный в этой сборке (или не поднявшийся) бэкенд один раз
    // откатывается на AUTO.
    RendererBackendKind requested = RenderBackendFromEnvironment();
    if (requested != RENDERER_BACKEND_AUTO && !RendererBackendIsAvailable(requested))
    {
        RenderBackendLog("requested backend is not available in this build; using auto");
        requested = RENDERER_BACKEND_AUTO;
    }
    if (!RenderSessionCreate(application, requested))
    {
        if (requested == RENDERER_BACKEND_AUTO)
        {
            int failure = application->sessionErrorCode != 0 ? application->sessionErrorCode : 5;
            DestroyApplication(application);
            return failure;
        }
        RenderBackendLog("renderer creation failed; retrying with auto");
        if (!RenderSessionCreate(application, RENDERER_BACKEND_AUTO))
        {
            int failure = application->sessionErrorCode != 0 ? application->sessionErrorCode : 5;
            DestroyApplication(application);
            return failure;
        }
    }
    RenderBackendLogActive("active backend", RendererGetBackend(application->renderer));
    application->cubes.useSpatialIndex = UseSpatialIndexFromEnvironment();

    application->previousTimeSeconds = PlatformTimeSeconds();
    WindowSetMouseLook(application->window, true);
    WindowSetRawInputCallback(application->window, HandleRawInput, application);
    application->runLoopStarted = true;
    StartupWriteStage(application, "run_loop_begin");
    WindowRunLoop(application->window, OnFrame, application);

    int exitCode = application->exitCode;
    if (mode == SIMULATION_RUN_BACKEND_SWITCH_SMOKE && exitCode == 0 &&
        (!application->switchFillObserved || application->switchPhase != 3u))
    {
        exitCode = 12;
    }
    DestroyApplication(application);
    return exitCode;
}
