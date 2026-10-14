#ifndef SOS_SIM_CLOCK_H
#define SOS_SIM_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Fixed-step simulation clock.
 *
 * The world advances only in whole ticks of one fixed length, so its state
 * depends on the number of ticks and never on frame rate, machine speed or
 * how real time happened to be sliced. That is what lets a server, every
 * client and a replay of recorded history compute the same world. Real time
 * enters the clock and leaves it as a count of ticks to run now; how finely
 * the world steps through them is the world's decision.
 *
 * The time scale belongs to the player who controls time: 0 pauses the world,
 * 1 runs it in real time and larger values fast-forward it, up to one game
 * year per real second. */

#define SOS_CLOCK_TICKS_PER_SECOND 30u
#define SOS_CLOCK_MAX_TICKS_PER_SECOND 1000u
/* One game year of 365.25 days per real second. */
#define SOS_CLOCK_MAX_TIME_SCALE 31557600.0
/* An advance never counts more real time than this. Longer gaps (a stalled
 * frame, a debugger, a suspended laptop) are dropped and counted, so a stall
 * slows the world down instead of making it leap. */
#define SOS_CLOCK_DEFAULT_MAX_ADVANCE_SECONDS 0.25

typedef struct SosClock
{
    /* Ticks simulated since the world began. */
    uint64_t tick;
    uint32_t ticksPerSecond;
    double maxAdvanceSeconds;
    double timeScale;
    /* Fraction of a tick carried over to the next advance, in [0, 1). */
    double pendingTicks;
    /* Real time dropped by the advance limit, for diagnostics. */
    double droppedSeconds;
} SosClock;

/* False leaves the clock untouched: the rate must be 1..1000 ticks per second
 * and the advance limit a positive finite number of seconds. */
bool SosClockInit(SosClock *clock, uint32_t ticksPerSecond, double maxAdvanceSeconds);

/* Accepts a finite scale in [0, SOS_CLOCK_MAX_TIME_SCALE]. */
bool SosClockSetTimeScale(SosClock *clock, double timeScale);

/* Turns elapsed real seconds into the number of ticks to run now and counts
 * them in `tick`. A negative, non-finite or zero interval yields no ticks. */
uint64_t SosClockAdvance(SosClock *clock, double realSeconds);

/* Real seconds until the next tick at the current scale; zero when it is due
 * now, and a negative number while the world is paused. */
double SosClockSecondsToNextTick(const SosClock *clock);

/* Simulated seconds covered by one tick. */
double SosClockTickSeconds(const SosClock *clock);

/* How far real time has moved past the last tick, in [0, 1): presentation
 * interpolates between the last two simulated states by this factor. */
float SosClockInterpolation(const SosClock *clock);

#endif
