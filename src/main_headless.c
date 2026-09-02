#include "app/core_smoke.h"

#include <stdio.h>
#include <string.h>

int main(int argumentCount, char **arguments)
{
    if (argumentCount == 1 || (argumentCount == 2 && strcmp(arguments[1], "--smoke") == 0))
    {
        return SimulationCoreSmokeTest() ? 0 : 10;
    }

    fputs("usage: SimulationOfSinsHeadless [--smoke]\n", stderr);
    return 2;
}
