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
 * enters the clock and leaves it as a count of ticks to run now.
 *
 * The time scale belongs to the player who controls time: 0 pauses the world,
 * 1 runs it in real time and larger values fast-forward it. */

#define SOS_CLOCK_DEFAULT_TICKS_PER_SECOND 30u
/* A frame never runs more ticks than this. Time beyond the limit is dropped
 * and counted, so a stall slows the world down instead of freezing the frame
 * while it catches up. */
#define SOS_CLOCK_DEFAULT_MAX_TICKS_PER_ADVANCE 8u
#define SOS_CLOCK_MAX_TICKS_PER_SECOND 1000u
/* Fast-forward reaches one game year (365.25 days) per real second. Fine
 * ticks cannot keep that pace: beyond what the machine runs, ticks are
 * dropped, and the world needs coarser steps for what nobody watches. */
#define SOS_CLOCK_MAX_TIME_SCALE 31557600.0

typedef struct SosClock
{
    /* Ticks simulated since the world began. */
    uint64_t tick;
    uint32_t ticksPerSecond;
    uint32_t maxTicksPerAdvance;
    double timeScale;
    /* Fraction of a tick carried over to the next advance, in [0, 1). */
    double pendingTicks;
    /* Ticks lost to the catch-up limit, for diagnostics. */
    uint64_t droppedTicks;
} SosClock;

/* False leaves the clock untouched: the rate must be 1..1000 ticks per second
 * and the catch-up limit at least one tick. */
bool SosClockInit(SosClock *clock, uint32_t ticksPerSecond, uint32_t maxTicksPerAdvance);

/* Accepts a finite scale in [0, SOS_CLOCK_MAX_TIME_SCALE]. */
bool SosClockSetTimeScale(SosClock *clock, double timeScale);

/* Turns elapsed real seconds into the number of ticks to run now and counts
 * them in `tick`. A negative, non-finite or zero interval yields no ticks. */
uint32_t SosClockAdvance(SosClock *clock, double realSeconds);

/* Simulated seconds covered by one tick. */
double SosClockTickSeconds(const SosClock *clock);

/* How far real time has moved past the last tick, in [0, 1): presentation
 * interpolates between the last two simulated states by this factor. */
float SosClockInterpolation(const SosClock *clock);

#endif
