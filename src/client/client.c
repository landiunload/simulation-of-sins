#include "client/client.h"

#include "app/log.h"
#include "app/modules.h"
#include "graphics/graphics_device_service.h"
#include "input/input_service.h"
#include "mod/application.h"
#include "platform/window_service.h"
#include "sim/clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define SOS_CLIENT_WIDTH 1280
#define SOS_CLIENT_HEIGHT 720
/* A frame gap longer than this is a stall (a dragged window, a debugger) and
 * is not simulated as elapsed time. */
#define SOS_CLIENT_MAX_FRAME_SECONDS 0.25

typedef struct GraphicsProvider
{
    SosGraphicsChoice choice;
    const char *module;
    const char *id;
} GraphicsProvider;

/* Preferred first: the platform's native API, then the portable one. */
static const GraphicsProvider graphicsProviders[] = {
#if defined(_WIN32)
    {SOS_GRAPHICS_D3D12, "graphics_d3d12", "laiue.graphics.d3d12"},
#endif
    {SOS_GRAPHICS_VULKAN, "graphics_vulkan", "laiue.graphics.vulkan"},
};

typedef struct ClientSession
{
    const LaiueWindowServiceV1 *windowService;
    Window *window;
    const LaiueInputServiceV1 *inputService;
    Input *input;
    const LaiueGraphicsDeviceServiceV2 *graphicsService;
    uint32_t graphicsServiceSize;
    LaiueGraphicsDeviceV2 *device;
    SosClock clock;
    double previousSeconds;
    uint32_t smokeFrames;
    uint32_t presentedFrames;
    bool frameFailed;
} ClientSession;

static bool FieldPresent(uint32_t publishedSize, uint32_t structSize, size_t offset, size_t size)
{
    return offset + size <= (size_t)publishedSize && offset + size <= (size_t)structSize;
}

#define DEVICE_HAS(device, field)                                                                  \
    ((device)->structSize >= offsetof(LaiueGraphicsDeviceV2, field) + sizeof((device)->field) &&   \
     (device)->field != NULL)

static const LaiueWindowServiceV1 *QueryWindow(const SosModules *modules)
{
    uint32_t size = 0u;
    const LaiueWindowServiceV1 *service = (const LaiueWindowServiceV1 *)SosModulesQuery(
        modules, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
        (uint32_t)sizeof(*service), &size);
    if (service == NULL || service->structSize < sizeof(*service) ||
        service->abiVersion != LAIUE_WINDOW_SERVICE_ABI_VERSION_1 || service->create == NULL ||
        service->destroy == NULL || service->getNativeHandle == NULL ||
        service->getClientSize == NULL || service->runLoop == NULL ||
        service->requestClose == NULL || service->setRawInputCallback == NULL)
    {
        return NULL;
    }
    return service;
}

static const LaiueInputServiceV1 *QueryInput(const SosModules *modules)
{
    uint32_t size = 0u;
    const LaiueInputServiceV1 *service = (const LaiueInputServiceV1 *)SosModulesQuery(
        modules, LAIUE_INPUT_SERVICE_NAME, LAIUE_INPUT_SERVICE_ABI_VERSION_1,
        (uint32_t)sizeof(*service), &size);
    if (service == NULL || service->structSize < sizeof(*service) ||
        service->abiVersion != LAIUE_INPUT_SERVICE_ABI_VERSION_1 || service->create == NULL ||
        service->destroy == NULL || service->handleRawInput == NULL || service->endFrame == NULL ||
        service->resetState == NULL || service->consumeKeyPress == NULL)
    {
        return NULL;
    }
    return service;
}

static const LaiueGraphicsDeviceServiceV2 *QueryGraphics(const SosModules *modules,
                                                         uint32_t *outSize)
{
    const LaiueGraphicsDeviceServiceV2 *service =
        (const LaiueGraphicsDeviceServiceV2 *)SosModulesQuery(
            modules, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, outSize);
    if (service == NULL || service->structSize < LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE ||
        service->abiVersion != LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2 ||
        service->createDevice == NULL || service->destroyDevice == NULL)
    {
        return NULL;
    }
    return service;
}

static LaiueGraphicsDeviceV2 *CreateDevice(const LaiueGraphicsDeviceServiceV2 *service,
                                           uint32_t serviceSize, void *nativeWindow)
{
    LaiueGraphicsDeviceV2 *device = NULL;
    const bool withContext =
        FieldPresent(serviceSize, service->structSize,
                     offsetof(LaiueGraphicsDeviceServiceV2, context), sizeof(service->context)) &&
        service->createDeviceWithContext != NULL && service->context != NULL;
    const uint32_t created =
        withContext ? service->createDeviceWithContext(service->context, nativeWindow,
                                                       SOS_CLIENT_WIDTH, SOS_CLIENT_HEIGHT,
                                                       LAIUE_GRAPHICS_BACKEND_AUTO, &device)
                    : service->createDevice(nativeWindow, SOS_CLIENT_WIDTH, SOS_CLIENT_HEIGHT,
                                            LAIUE_GRAPHICS_BACKEND_AUTO, &device);
    if (created != 0u && device != NULL &&
        device->structSize >=
            offsetof(LaiueGraphicsDeviceV2, abiVersion) + sizeof(device->abiVersion) &&
        device->abiVersion == LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION &&
        DEVICE_HAS(device, beginFrame) && DEVICE_HAS(device, endFrame))
    {
        return device;
    }
    /* A partial result still belongs to the provider's destroy callback. */
    if (device != NULL)
    {
        service->destroyDevice(device);
    }
    return NULL;
}

static void OnRawInput(void *opaque, void *rawInput)
{
    ClientSession *session = (ClientSession *)opaque;
    if (session->input != NULL)
    {
        session->inputService->handleRawInput(session->input, rawInput);
    }
}

static void OnFrame(void *opaque)
{
    ClientSession *session = (ClientSession *)opaque;
    const LaiueWindowServiceV1 *window = session->windowService;
    if (window->consumeFocusLoss != NULL && window->consumeFocusLoss(session->window) &&
        session->input != NULL)
    {
        /* Releases that happened outside the window are never delivered. */
        session->inputService->resetState(session->input);
    }
    if (window->consumeResize != NULL && window->consumeResize(session->window) &&
        FieldPresent(session->graphicsServiceSize, session->graphicsService->structSize,
                     offsetof(LaiueGraphicsDeviceServiceV2, resize),
                     sizeof(session->graphicsService->resize)) &&
        session->graphicsService->resize != NULL)
    {
        int32_t width = 0;
        int32_t height = 0;
        window->getClientSize(session->window, &width, &height);
        if (width > 0 && height > 0)
        {
            session->graphicsService->resize(session->device, width, height);
        }
    }
    if (session->input != NULL &&
        session->inputService->consumeKeyPress(session->input, INPUT_KEY_ESCAPE))
    {
        window->requestClose(session->window);
    }

    const double now = LaiueApplicationMonotonicSeconds();
    double elapsed = now - session->previousSeconds;
    session->previousSeconds = now;
    if (elapsed > SOS_CLIENT_MAX_FRAME_SECONDS)
    {
        elapsed = 0.0;
    }
    (void)SosClockAdvance(&session->clock, elapsed);

    int32_t width = 0;
    int32_t height = 0;
    window->getClientSize(session->window, &width, &height);
    if (width > 0 && height > 0)
    {
        LaiueGraphicsDeviceV2 *device = session->device;
        if (device->beginFrame(device, (uint32_t)width, (uint32_t)height) != 0u &&
            device->endFrame(device) != 0u)
        {
            ++session->presentedFrames;
        }
        else if (session->smokeFrames != 0u)
        {
            session->frameFailed = true;
            window->requestClose(session->window);
        }
    }
    if (session->input != NULL)
    {
        session->inputService->endFrame(session->input);
    }
    if (session->smokeFrames != 0u && session->presentedFrames >= session->smokeFrames)
    {
        window->requestClose(session->window);
    }
}

/* Opens a window and a device on the loaded modules and runs until the window
 * closes. False means this provider cannot present here, and nothing is left
 * open. */
static bool RunPresentation(const SosModules *modules, const SosClientOptions *options,
                            int *outExitCode)
{
    static ClientSession session;
    memset(&session, 0, sizeof(session));
    session.windowService = QueryWindow(modules);
    session.graphicsService = QueryGraphics(modules, &session.graphicsServiceSize);
    if (session.windowService == NULL || session.graphicsService == NULL ||
        !SosClockInit(&session.clock, SOS_CLOCK_DEFAULT_TICKS_PER_SECOND,
                      SOS_CLOCK_DEFAULT_MAX_TICKS_PER_ADVANCE) ||
        !SosClockSetTimeScale(&session.clock, options->timeScale))
    {
        return false;
    }

    const WindowConfiguration configuration = {
        .title = L"Simulation of sins",
        .width = SOS_CLIENT_WIDTH,
        .height = SOS_CLIENT_HEIGHT,
    };
    session.window = session.windowService->create(&configuration);
    if (session.window == NULL)
    {
        SosLog(SOS_LOG_WARNING, "window could not be created");
        return false;
    }
    void *nativeWindow = session.windowService->getNativeHandle(session.window);
    session.device =
        CreateDevice(session.graphicsService, session.graphicsServiceSize, nativeWindow);
    if (session.device == NULL)
    {
        SosLog(SOS_LOG_WARNING, "graphics device could not be created");
        session.windowService->destroy(session.window);
        return false;
    }
    session.inputService = QueryInput(modules);
    session.input =
        session.inputService == NULL ? NULL : session.inputService->create(nativeWindow);
    if (session.input == NULL)
    {
        SosLog(SOS_LOG_WARNING, "input is unavailable; the window cannot be controlled");
    }

    session.smokeFrames = options->smokeFrames;
    session.previousSeconds = LaiueApplicationMonotonicSeconds();
    session.windowService->setRawInputCallback(session.window, OnRawInput, &session);
    session.windowService->runLoop(session.window, OnFrame, &session);
    session.windowService->setRawInputCallback(session.window, NULL, NULL);

    if (session.input != NULL)
    {
        session.inputService->destroy(session.input);
    }
    session.graphicsService->destroyDevice(session.device);
    session.windowService->destroy(session.window);

    if (options->smokeFrames != 0u)
    {
        const bool passed = !session.frameFailed && session.presentedFrames >= options->smokeFrames;
        SosLog(passed ? SOS_LOG_INFO : SOS_LOG_ERROR, "smoke: %u of %u frames presented",
               (unsigned)session.presentedFrames, (unsigned)options->smokeFrames);
        *outExitCode = passed ? 0 : SOS_CLIENT_EXIT_FRAME_FAILED;
    }
    else
    {
        *outExitCode = 0;
    }
    return true;
}

int SosClientRun(const SosClientOptions *options)
{
    if (options == NULL)
    {
        return SOS_CLIENT_EXIT_INVALID_OPTIONS;
    }
    SosModules modules;
    if (!SosModulesOpen(&modules))
    {
        return SOS_CLIENT_EXIT_NO_ENGINE;
    }
    int exitCode = SOS_CLIENT_EXIT_NO_PRESENTATION;
    bool presented = false;
    for (size_t index = 0u;
         !presented && index < sizeof(graphicsProviders) / sizeof(graphicsProviders[0]); ++index)
    {
        const GraphicsProvider *provider = &graphicsProviders[index];
        if (options->graphics != SOS_GRAPHICS_ANY && options->graphics != provider->choice)
        {
            continue;
        }
        const char *const names[] = {"window", "input", provider->module};
        const LaiueModuleProviderSelectionV1 selection = {
            .structSize = sizeof(selection),
            .serviceName = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            .moduleId = provider->id,
        };
        SosLog(SOS_LOG_INFO, "trying %s", provider->id);
        if (SosModulesLoad(&modules, names, (uint32_t)(sizeof(names) / sizeof(names[0])),
                           &selection, 1u))
        {
            presented = RunPresentation(&modules, options, &exitCode);
        }
        SosModulesUnload(&modules);
    }
    SosModulesClose(&modules);
    if (!presented)
    {
        SosLog(SOS_LOG_ERROR, "no window and graphics provider could present on this machine");
    }
    return exitCode;
}
