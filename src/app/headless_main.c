#include "app/arguments.h"
#include "app/headless.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(int argumentCount, char **arguments)
{
    SosHeadlessOptions options = {.ticks = 0u, .timeScale = 1.0};
    /* Every option takes one value. */
    for (int index = 1; index < argumentCount; index += 2)
    {
        const char *argument = arguments[index];
        const char *value = index + 1 < argumentCount ? arguments[index + 1] : NULL;
        bool parsed = false;
        if (strcmp(argument, "--ticks") == 0)
        {
            parsed = SosParseUint64(value, &options.ticks);
        }
        else if (strcmp(argument, "--time-scale") == 0)
        {
            parsed = SosParseDouble(value, &options.timeScale);
        }
        if (!parsed)
        {
            fputs("usage: SimulationOfSinsHeadless [--ticks N] [--time-scale X]\n", stderr);
            return 2;
        }
    }
    return SosHeadlessRun(&options);
}
