#ifndef SOS_SIM_BODY_H
#define SOS_SIM_BODY_H

#include "sim/biology.h"
#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Bodies.
 *
 * A being's body is never stored: it follows from the adult potentials it was
 * born with, its sex and its age, so an unbounded number of beings costs
 * nothing to age and every machine computes the same body from the same
 * inputs. The potentials are genome percentiles, near-uniform in [0, ONE], so
 * a potential of SOS_Q16_ONE/2 describes a median adult. Growth runs along
 * piecewise-linear integer curves -- a fast infancy, an adolescent spurt about
 * two years earlier for girls, an adult plateau and a slow decline after forty
 * -- while mass follows height through a BMI-like relation. The potentials
 * describe what an adult could be, later read from the genome; this module
 * knows only how they become a body at a moment in time. */

typedef struct SosBodyPotential
{
    /* Adult height as a genome percentile of the sex's range: SOS_Q16_ONE is
     * the tallest of that sex, zero the shortest. Clamped to [0, SOS_Q16_ONE]. */
    SosQ16 height;
    /* Adult strength, endurance and speed on the same percentile scale:
     * SOS_Q16_ONE is an exceptional adult. Each is clamped to [0, SOS_Q16_ONE]. */
    SosQ16 strength;
    SosQ16 endurance;
    SosQ16 speed;
} SosBodyPotential;

typedef struct SosBody
{
    int32_t heightMm;
    int32_t massGrams;
    /* Current strength and endurance, on the potential percentile scale. */
    SosQ16 strength;
    SosQ16 endurance;
    /* Current walking speed in world units (1/1024 m) per second: the adult
     * speed potential mapped to 1.1-1.7 m/s and then scaled by the age curve.
     * Read it through SosBodyWalkSpeed, which also guards a zeroed body. */
    SosQ16 speed;
} SosBody;

/* The body of a being of `sex` that is `ageTicks` old, grown from its adult
 * `potential`. False when `potential` or `out` is NULL, `ageTicks` is
 * negative or `sex` is not SOS_SEX_FEMALE or SOS_SEX_MALE, and `out` is then
 * left unchanged. Potentials outside [0, SOS_Q16_ONE] are clamped; ages past
 * the oldest point of the curves stay at that point, so any tick is safe. */
bool SosBodyAt(const SosBodyPotential *potential, SosSex sex, int64_t ageTicks, SosBody *out);

/* Walking speed in world units (1/1024 m) per second. The adult a speed
 * potential describes walks at 1.1 m/s at the slowest and 1.7 m/s at the
 * fastest, so a median (0.5) adult walks at the 1.4 m/s the rest of the
 * simulation assumes; that adult pace is then scaled by the age curve, so
 * infants barely move, children walk slower than adults and the old slow down.
 * Zero when `body` is NULL or carries no positive speed. */
int64_t SosBodyWalkSpeed(const SosBody *body);

/* The mass the body lifts without strain, in grams: a fraction of the body's
 * own mass scaled by its strength. Zero when `body` is NULL. */
int32_t SosBodyCarryGrams(const SosBody *body);

#endif
