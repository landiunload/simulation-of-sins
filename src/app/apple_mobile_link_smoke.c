#include "app/core_smoke.h"

/* Build-only composition root: it verifies the final Apple mobile link. */
int main(void)
{
    return SimulationCoreSmokeTest() ? 0 : 1;
}
