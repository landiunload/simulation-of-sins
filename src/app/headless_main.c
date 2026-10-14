#include "app/arguments.h"
#include "app/headless.h"

#include <stdio.h>
#include <string.h>

int main(int argumentCount, char **arguments)
{
    SosHeadlessOptions options = {.ticks = 0u, .timeScale = 1.0};
    for (int index = 1; index < argumentCount; ++index)
    {
        const char *argument = arguments[index];
        const char *value = index + 1 < argumentCount ? arguments[index + 1] : NULL;
        if (strcmp(argument, "--ticks") == 0 && SosParseUint64(value, &options.ticks))
        {
            ++index;
        }
        else if (strcmp(argument, "--time-scale") == 0 && SosParseDouble(value, &options.timeScale))
        {
            ++index;
        }
        else
        {
            fputs("usage: SimulationOfSinsHeadless [--ticks N] [--time-scale X]\n", stderr);
            return 2;
        }
    }
    return SosHeadlessRun(&options);
}
