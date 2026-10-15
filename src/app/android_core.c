#include "app/headless.h"

#if defined(__GNUC__)
#define SOS_ANDROID_EXPORT __attribute__((visibility("default")))
#else
#define SOS_ANDROID_EXPORT
#endif

/*
 * Stable native entry for the future Android application shell. CI links the
 * whole game core and the engine bootstrap into this shared library, so every
 * object takes part in one final NDK link.
 */
SOS_ANDROID_EXPORT int SimulationOfSinsAndroidCoreSmoke(void)
{
    const SosHeadlessOptions options = {.ticks = 30u, .timeScale = 1000.0};
    return SosHeadlessRun(&options);
}
