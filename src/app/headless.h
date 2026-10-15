#ifndef SOS_APP_HEADLESS_H
#define SOS_APP_HEADLESS_H

#include <stdint.h>

/* The game without presentation: a dedicated server or an accelerated run of
 * the simulation. */
typedef struct SosHeadlessOptions
{
    /* Stops after this many ticks; zero runs until the process is asked to
     * terminate. */
    uint64_t ticks;
    /* 1 is real time; larger values fast-forward as far as the machine
     * allows. */
    double timeScale;
} SosHeadlessOptions;

/* Returns the process exit code. */
int SosHeadlessRun(const SosHeadlessOptions *options);

#endif
