#include "app/core_smoke.h"

#if defined(__GNUC__)
#define SOS_ANDROID_EXPORT __attribute__((visibility("default")))
#else
#define SOS_ANDROID_EXPORT
#endif

/*
 * Stable native entry for the future Android application shell.  CI
 * whole-archives the complete game and engine static core into this shared
 * library, so every object participates in one final NDK link.
 */
SOS_ANDROID_EXPORT int SimulationOfSinsAndroidCoreSmoke(void)
{
    return SimulationCoreSmokeTest() ? 0 : 1;
}
