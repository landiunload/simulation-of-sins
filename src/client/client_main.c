#include "app/arguments.h"
#include "client/client.h"

#include <stdio.h>
#include <string.h>

static int Run(int argumentCount, char **arguments)
{
    SosClientOptions options = {.graphics = SOS_GRAPHICS_ANY, .timeScale = 1.0};
    for (int index = 1; index < argumentCount; ++index)
    {
        const char *argument = arguments[index];
        const char *value = index + 1 < argumentCount ? arguments[index + 1] : NULL;
        if (strcmp(argument, "--smoke") == 0)
        {
            options.smokeFrames = 3u;
        }
        else if (strcmp(argument, "--graphics") == 0 && value != NULL &&
                 (strcmp(value, "d3d12") == 0 || strcmp(value, "vulkan") == 0))
        {
            options.graphics = value[0] == 'd' ? SOS_GRAPHICS_D3D12 : SOS_GRAPHICS_VULKAN;
            ++index;
        }
        else if (strcmp(argument, "--time-scale") == 0 && SosParseDouble(value, &options.timeScale))
        {
            ++index;
        }
        else
        {
            fputs("usage: SimulationOfSins [--smoke] [--graphics d3d12|vulkan] "
                  "[--time-scale X]\n",
                  stderr);
            return SOS_CLIENT_EXIT_INVALID_OPTIONS;
        }
    }
    return SosClientRun(&options);
}

#if defined(_WIN32)

#include <windows.h>

#include <shellapi.h>

#define SOS_MAX_ARGUMENTS 64
#define SOS_MAX_ARGUMENT_BYTES 1024

/* A windowed program has no console of its own. When it is started from one,
 * diagnostics go there; a redirected stream is left as the parent set it. */
static void AttachParentConsole(void)
{
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if ((error == NULL || error == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE *stream = NULL;
        (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, PWSTR commandLine,
                    int showCommand)
{
    (void)instance;
    (void)previousInstance;
    (void)commandLine;
    (void)showCommand;
    AttachParentConsole();

    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == NULL || count < 1 || count > SOS_MAX_ARGUMENTS)
    {
        LocalFree((HLOCAL)wide);
        return SOS_CLIENT_EXIT_INVALID_OPTIONS;
    }
    static char storage[SOS_MAX_ARGUMENTS][SOS_MAX_ARGUMENT_BYTES];
    char *arguments[SOS_MAX_ARGUMENTS];
    for (int index = 0; index < count; ++index)
    {
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1, storage[index],
                                SOS_MAX_ARGUMENT_BYTES, NULL, NULL) == 0)
        {
            LocalFree((HLOCAL)wide);
            return SOS_CLIENT_EXIT_INVALID_OPTIONS;
        }
        arguments[index] = storage[index];
    }
    LocalFree((HLOCAL)wide);
    return Run(count, arguments);
}

#else

int main(int argumentCount, char **arguments)
{
    return Run(argumentCount, arguments);
}

#endif
