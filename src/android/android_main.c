/* The Android shell: a NativeActivity that shows the world on the activity's
 * surface and steers it with touch gestures. The engine is linked statically;
 * its graphics, camera math and meshing start as optional modules, as on the
 * desktop. The world outlives the surface: in the background the device goes
 * away and comes back later while the world waits. */

#include "app/log.h"
#include "app/modules.h"
#include "client/scene.h"
#include "client/touch.h"
#include "graphics/graphics_device_service.h"
#include "mesh/mesher_service.h"
#include "render/graphics_service.h"
#include "scene/math_service.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SEED UINT64_C(1)
#define SPEED_PRESETS 6
/* A finger may wander this far, in millimetres, and still tap. */
#define TOUCH_SLOP_MILLIMETRES 1.5f

typedef struct AndroidGame
{
    struct android_app *app;
    SosModules modules;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    uint32_t graphicsSize;
    LaiueGraphicsDeviceV2 *device;
    SosScene *scene;
    SosTouch touch;
    int32_t width;
    int32_t height;
    int32_t speed;
    bool speedChanged;
    bool focused;
    double previousSeconds;
} AndroidGame;

#define DEVICE_HAS(device, field)                                                                  \
    ((device)->structSize >= offsetof(LaiueGraphicsDeviceV2, field) + sizeof((device)->field) &&   \
     (device)->field != NULL)

/* Touch events carry times of this clock. */
static double MonotonicSeconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static bool ServiceHas(const AndroidGame *game, size_t offset, size_t size)
{
    return offset + size <= (size_t)game->graphicsSize &&
           offset + size <= (size_t)game->graphics->structSize;
}

static void StartEngine(AndroidGame *game)
{
    if (!SosModulesOpen(&game->modules))
    {
        return;
    }
    const LaiueModuleApiV1 *const apis[] = {
        LaiueGraphicsGetStaticModuleApiV1(),
        LaiueSceneMathGetStaticModuleApiV1(),
        LaiueMesherGetStaticModuleApiV1(),
    };
    const char *const names[] = {"render", "scene_math", "mesher"};
    if (!SosModulesLoadStatic(&game->modules, apis, names, 3u))
    {
        return;
    }
    const LaiueGraphicsDeviceServiceV2 *graphics =
        (const LaiueGraphicsDeviceServiceV2 *)SosModulesQuery(
            &game->modules, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, &game->graphicsSize);
    if (graphics != NULL && graphics->abiVersion == LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2 &&
        graphics->createDevice != NULL && graphics->destroyDevice != NULL)
    {
        game->graphics = graphics;
    }
    else
    {
        SosLog(SOS_LOG_ERROR, "graphics are unavailable; nothing can be shown");
    }

    SosSceneDesc desc = {.seed = SEED, .timeScale = 1.0};
    desc.math = (const LaiueSceneMathServiceV1 *)SosModulesQuery(
        &game->modules, LAIUE_SCENE_MATH_SERVICE_NAME, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
        LAIUE_SCENE_MATH_SERVICE_V1_LEGACY_SIZE, &desc.mathServiceSize);
    desc.mesher = (const LaiueMesherServiceV1 *)SosModulesQuery(
        &game->modules, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        LAIUE_MESHER_SERVICE_V1_LEGACY_SIZE, &desc.mesherServiceSize);
    game->scene = SosSceneCreate(&desc);
    if (game->scene == NULL)
    {
        SosLog(SOS_LOG_ERROR, "the world could not be created");
    }
}

static void DestroyDevice(AndroidGame *game)
{
    if (game->device == NULL)
    {
        return;
    }
    SosSceneDetachDevice(game->scene);
    game->graphics->destroyDevice(game->device);
    game->device = NULL;
}

static void CreateDevice(AndroidGame *game)
{
    ANativeWindow *window = game->app->window;
    if (game->graphics == NULL || game->scene == NULL || window == NULL)
    {
        return;
    }
    DestroyDevice(game);
    const int32_t width = ANativeWindow_getWidth(window);
    const int32_t height = ANativeWindow_getHeight(window);
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const LaiueGraphicsDeviceServiceV2 *graphics = game->graphics;
    LaiueGraphicsDeviceV2 *device = NULL;
    const bool withContext = ServiceHas(game, offsetof(LaiueGraphicsDeviceServiceV2, context),
                                        sizeof(graphics->context)) &&
                             graphics->createDeviceWithContext != NULL && graphics->context != NULL;
    const uint32_t created =
        withContext
            ? graphics->createDeviceWithContext(graphics->context, window, width, height,
                                                LAIUE_GRAPHICS_BACKEND_VULKAN, &device)
            : graphics->createDevice(window, width, height, LAIUE_GRAPHICS_BACKEND_VULKAN, &device);
    if (created == 0u || device == NULL ||
        device->structSize <
            offsetof(LaiueGraphicsDeviceV2, abiVersion) + sizeof(device->abiVersion) ||
        device->abiVersion != LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION ||
        !DEVICE_HAS(device, beginFrame) || !DEVICE_HAS(device, endFrame))
    {
        /* A partial result still belongs to the provider's destroy callback. */
        if (device != NULL)
        {
            graphics->destroyDevice(device);
        }
        SosLog(SOS_LOG_ERROR, "the Vulkan device could not be created");
        return;
    }
    game->device = device;
    game->width = width;
    game->height = height;
    (void)SosSceneAttachDevice(game->scene, device);
    game->previousSeconds = MonotonicSeconds();
}

static void FollowSize(AndroidGame *game)
{
    if (game->device == NULL || game->app->window == NULL)
    {
        return;
    }
    const int32_t width = ANativeWindow_getWidth(game->app->window);
    const int32_t height = ANativeWindow_getHeight(game->app->window);
    if ((width != game->width || height != game->height) && width > 0 && height > 0 &&
        ServiceHas(game, offsetof(LaiueGraphicsDeviceServiceV2, resize),
                   sizeof(game->graphics->resize)) &&
        game->graphics->resize != NULL)
    {
        game->graphics->resize(game->device, width, height);
        game->width = width;
        game->height = height;
    }
}

static void HandleCommand(struct android_app *app, int32_t command)
{
    AndroidGame *game = (AndroidGame *)app->userData;
    switch (command)
    {
    case APP_CMD_INIT_WINDOW:
        CreateDevice(game);
        break;
    case APP_CMD_TERM_WINDOW:
        DestroyDevice(game);
        break;
    case APP_CMD_GAINED_FOCUS:
        game->focused = true;
        game->previousSeconds = MonotonicSeconds();
        break;
    case APP_CMD_LOST_FOCUS:
        game->focused = false;
        SosTouchEvent(&game->touch, SOS_TOUCH_CANCEL, 0, 0.0f, 0.0f, MonotonicSeconds());
        break;
    case APP_CMD_CONFIG_CHANGED:
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        FollowSize(game);
        break;
    default:
        break;
    }
}

/* Volume keys change the speed of time, from real time to a year per
 * second. */
static int32_t HandleKey(AndroidGame *game, const AInputEvent *event)
{
    const int32_t key = AKeyEvent_getKeyCode(event);
    if (key != AKEYCODE_VOLUME_UP && key != AKEYCODE_VOLUME_DOWN)
    {
        return 0;
    }
    if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN)
    {
        const int32_t speed = game->speed + (key == AKEYCODE_VOLUME_UP ? 1 : -1);
        if (speed >= 0 && speed < SPEED_PRESETS)
        {
            game->speed = speed;
            game->speedChanged = true;
        }
    }
    return 1;
}

static int32_t HandleMotion(AndroidGame *game, const AInputEvent *event)
{
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                  AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const double seconds = (double)AMotionEvent_getEventTime(event) * 1e-9;
    SosTouch *touch = &game->touch;
    switch (masked)
    {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        SosTouchEvent(touch, SOS_TOUCH_DOWN, AMotionEvent_getPointerId(event, index),
                      AMotionEvent_getX(event, index), AMotionEvent_getY(event, index), seconds);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t pointer = 0u; pointer < AMotionEvent_getPointerCount(event); ++pointer)
        {
            SosTouchEvent(touch, SOS_TOUCH_MOVE, AMotionEvent_getPointerId(event, pointer),
                          AMotionEvent_getX(event, pointer), AMotionEvent_getY(event, pointer),
                          seconds);
        }
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        SosTouchEvent(touch, SOS_TOUCH_UP, AMotionEvent_getPointerId(event, index),
                      AMotionEvent_getX(event, index), AMotionEvent_getY(event, index), seconds);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        SosTouchEvent(touch, SOS_TOUCH_CANCEL, 0, 0.0f, 0.0f, seconds);
        break;
    default:
        return 0;
    }
    return 1;
}

static int32_t HandleInput(struct android_app *app, AInputEvent *event)
{
    AndroidGame *game = (AndroidGame *)app->userData;
    switch (AInputEvent_getType(event))
    {
    case AINPUT_EVENT_TYPE_KEY:
        return HandleKey(game, event);
    case AINPUT_EVENT_TYPE_MOTION:
        return HandleMotion(game, event);
    default:
        return 0;
    }
}

static void Frame(AndroidGame *game)
{
    FollowSize(game);
    const double now = MonotonicSeconds();
    const double elapsed = now - game->previousSeconds;
    game->previousSeconds = now;
    SosControls controls =
        SosTouchTakeControls(&game->touch, now, (float)game->width, (float)game->height);
    if (game->speedChanged)
    {
        controls.speedPreset = game->speed;
        game->speedChanged = false;
    }
    LaiueGraphicsDeviceV2 *device = game->device;
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    const float aspect = (float)game->width / (float)game->height;
    if (SosSceneUpdate(game->scene, &controls, elapsed, aspect, camera.viewProjection) &&
        DEVICE_HAS(device, setCamera))
    {
        (void)device->setCamera(device, &camera);
    }
    if (device->beginFrame(device, (uint32_t)game->width, (uint32_t)game->height) != 0u)
    {
        SosSceneDraw(game->scene);
        (void)device->endFrame(device);
    }
}

static bool Active(const AndroidGame *game)
{
    return game->device != NULL && game->focused;
}

void android_main(struct android_app *app)
{
    static AndroidGame game;
    game = (AndroidGame){.app = app};
    const float density = (float)AConfiguration_getDensity(app->config);
    SosTouchInit(&game.touch, (density > 0.0f ? density : 160.0f) / 25.4f * TOUCH_SLOP_MILLIMETRES);
    app->userData = &game;
    app->onAppCmd = HandleCommand;
    app->onInputEvent = HandleInput;
    StartEngine(&game);

    while (app->destroyRequested == 0)
    {
        /* Drain every event; then draw a frame while visible, or sleep until
         * the next event while hidden. */
        int timeout = Active(&game) ? 0 : -1;
        int events = 0;
        struct android_poll_source *source = NULL;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0)
        {
            if (source != NULL)
            {
                source->process(app, source);
            }
            if (app->destroyRequested != 0)
            {
                break;
            }
            timeout = Active(&game) ? 0 : -1;
        }
        if (app->destroyRequested == 0 && Active(&game))
        {
            Frame(&game);
        }
    }

    DestroyDevice(&game);
    SosSceneDestroy(game.scene);
    SosModulesClose(&game.modules);
}
