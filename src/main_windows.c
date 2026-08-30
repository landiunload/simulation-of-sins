#include "app/application.h"

#include <wchar.h>
#include <windows.h>

// The parameter order is fixed by the Windows application entry-point ABI.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, wchar_t *commandLine,
                      int showCommand)
{
    (void)instance;
    (void)previousInstance;
    (void)showCommand;

    if (commandLine != NULL && wcscmp(commandLine, L"--smoke") == 0)
    {
        return SimulationApplicationSmokeTest() ? 0 : 10;
    }
    if (commandLine != NULL && wcscmp(commandLine, L"--render-smoke") == 0)
    {
        return SimulationApplicationRun(SIMULATION_RUN_RENDER_SMOKE);
    }
    if (commandLine != NULL && wcscmp(commandLine, L"--rebase-render-smoke") == 0)
    {
        return SimulationApplicationRun(SIMULATION_RUN_REBASE_RENDER_SMOKE);
    }
    return SimulationApplicationRun(SIMULATION_RUN_INTERACTIVE);
}
