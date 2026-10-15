#ifndef SOS_SIM_LIFECYCLE_H
#define SOS_SIM_LIFECYCLE_H

#include "sim/biology.h"
#include "sim/fixed.h"
#include "sim/random.h"

#include <stdbool.h>
#include <stdint.h>

/* Age-dependent biology, in integer arithmetic.
 *
 * A being's age decides everything that changes with time but is not the
 * business of any other module: its life stage, whether and how strongly it can
 * have children, and how likely it is to die. All of it is a pure function of
 * the age and a genome-supplied frailty, so it holds no state and never has to
 * be ticked: a step, a world hash or a screen can ask for any age at any time
 * and get the same answer on every machine.
 *
 * Frailty is the one free parameter, meant to come from the genome's longevity
 * later. It is a Q16 trait whose neutral mid-value SOS_Q16_ONE / 2 is the
 * reference life table: a frailty of zero halves the hazard, the neutral value
 * leaves it unchanged and one raises it by half. */

/* Age spans. A being is unborn while its birth tick lies in the future, and
 * dies of old age at the very latest at SosLifecycleMaxAgeTicks. */
typedef enum SosLifeStage
{
    SOS_LIFE_UNBORN = 0,
    SOS_LIFE_INFANT,
    SOS_LIFE_CHILD,
    SOS_LIFE_ADOLESCENT,
    SOS_LIFE_ADULT,
    SOS_LIFE_ELDER,
} SosLifeStage;

/* The stage an age falls in: unborn below zero, infant under two years, child
 * under twelve, adolescent under sixteen, adult under forty-five, elder after.
 * Any age, however extreme, gives a stage; unborn is safe for a being not yet
 * conceived. */
SosLifeStage SosLifecycleStage(int64_t ageTicks);

/* Fertility at an age, in [0, SOS_Q16_ONE]: the chance per conception attempt
 * rather than a rate. Women are infertile until about fourteen, peak in their
 * twenties and are infertile again by about fifty; men rise a little later and
 * decline slowly for the rest of their lives. An unborn being, an unknown sex
 * or any age below zero gives zero. */
SosQ16 SosLifecycleFertility(SosSex sex, int64_t ageTicks);

/* The yearly death hazard at an age, a Gompertz-Makeham curve with an infant
 * hump: a constant background, a hump that halves about every half year after
 * birth, and an adult part that doubles about every eight years from age
 * fifteen. Frailty scales the whole curve as described at the top. The hazard
 * is zero before birth and saturates at the maximum from SosLifecycleMaxAgeTicks
 * on, where death is a certainty. */
SosQ16 SosLifecycleMortality(int64_t ageTicks, SosQ16 frailty);

/* The chance in [0, SOS_Q16_ONE] of living through `durationTicks` from an
 * age: 2^(-hazard * duration / ln 2), with the hazard taken at the starting
 * age and held constant across the duration. Durations short against the
 * hazard's own rate of change (a year at adult ages) are accurate; nothing
 * changes when a duration is split, so a year in one call and a year in 365
 * daily calls agree. An unborn being, or any zero duration, always survives;
 * an age at or past SosLifecycleMaxAgeTicks never does. */
SosQ16 SosLifecycleSurvive(int64_t ageTicks, uint64_t durationTicks, SosQ16 frailty);

/* Whether a being dies within `durationTicks`, drawing once from `random` with
 * exactly the probability SosLifecycleSurvive reports. NULL `random`, an
 * unborn being or a zero duration never dies; an age at or past
 * SosLifecycleMaxAgeTicks always does. */
bool SosLifecycleDies(SosRandom *random, int64_t ageTicks, uint64_t durationTicks, SosQ16 frailty);

/* How long a pregnancy lasts: about 266 days. */
int64_t SosLifecycleGestationTicks(void);

/* The oldest age anyone reaches: death is certain from here on. */
int64_t SosLifecycleMaxAgeTicks(void);

#endif
