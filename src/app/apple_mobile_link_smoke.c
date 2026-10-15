#include "app/headless.h"

/* Build-only composition root: it verifies the final Apple mobile link. */
int main(void)
{
    const SosHeadlessOptions options = {.ticks = 30u, .timeScale = 1000.0};
    return SosHeadlessRun(&options);
}
