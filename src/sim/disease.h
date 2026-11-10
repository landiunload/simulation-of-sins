#ifndef SOS_SIM_DISEASE_H
#define SOS_SIM_DISEASE_H

#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/random.h"

#include <stdbool.h>
#include <stdint.h>

/* Infectious disease.
 *
 * A pathogen is a world function, not stored state: its properties follow from
 * the world seed and a strain id, so any strain can be recalled from its id
 * forever and millions of them cost nothing to keep. The high bits of an id
 * name a lineage, hashed with the seed; the low bits are bounded antigenic
 * drift. A strain derived through SosDiseaseMutate keeps its parent's lineage,
 * so its properties only shift by a small, bounded step.
 *
 * Each being carries one SosInfection value next to its other state. A being
 * step keeps the value as it was at the start of the span, computes the health
 * it lost over the span with SosDiseaseDamage, then advances the value with
 * SosDiseaseAdvance and applies the returned events: a recovery means the
 * being rolls the pathogen's lethality exactly once. This module owns no
 * memory, so it needs no create, destroy or hash. */

/* The low bits of a strain id are drift; the rest name the lineage. */
#define SOS_DISEASE_DRIFT_BITS 24u

/* The contact time at which transmissibility 1.0 at full closeness is certain
 * to pass the infection on. Shorter contacts scale the chance down and longer
 * ones saturate it. */
#define SOS_DISEASE_EXPOSURE_TICKS SOS_TICKS_PER_MINUTE

typedef enum SosPathogenRoute
{
    SOS_PATHOGEN_ROUTE_CONTACT = 0,
    SOS_PATHOGEN_ROUTE_AIR,
    SOS_PATHOGEN_ROUTE_WATER,
    SOS_PATHOGEN_ROUTE_FOOD,
    SOS_PATHOGEN_ROUTE_COUNT,
} SosPathogenRoute;

typedef struct SosPathogen
{
    /* Chance per contact unit that one exposure infects, in [0, SOS_Q16_ONE]. */
    SosQ16 transmissibility;
    /* Exposed and not yet spreading. */
    uint64_t incubationTicks;
    /* Spreading: contagious and doing damage. */
    uint64_t infectiousTicks;
    /* Health lost per day, as a Q16 share of the being health scale (a full
     * bar is SOS_NEED_MAX in biology.h). */
    SosQ16 severity;
    /* Chance the infection is fatal when it ends, for the being step to roll. */
    SosQ16 lethality;
    /* How long recovery protects against a new infection. */
    uint64_t immunityTicks;
    SosPathogenRoute route;
} SosPathogen;

typedef enum SosInfectionStage
{
    SOS_INFECTION_SUSCEPTIBLE = 0,
    SOS_INFECTION_EXPOSED,
    SOS_INFECTION_INFECTIOUS,
    SOS_INFECTION_RECOVERED,
} SosInfectionStage;

typedef struct SosInfection
{
    /* The pathogen id, 0 when the being has none. */
    uint64_t pathogen;
    SosInfectionStage stage;
    /* The tick the current stage began at. */
    uint64_t stageTick;
    /* Recovery protects until this tick; ignored while not recovered. */
    uint64_t immuneUntil;
} SosInfection;

/* What one SosDiseaseAdvance changed, so the being step learns about the
 * stages even a coarse step crossed in a single call. */
typedef struct SosDiseaseOutcome
{
    /* The incubation ended: the being is now contagious. */
    bool becameInfectious;
    /* The infectious period ended in this advance. The being step rolls the
     * pathogen's lethality now, once, even when the same advance also cleared
     * the infection and the final stage is susceptible. */
    bool recovered;
    /* Immunity ended and the infection returned to susceptible. */
    bool cleared;
} SosDiseaseOutcome;

/* The pathogen with the strain id `id` under `seed`. False for a NULL output or
 * id 0, which name no pathogen, leaving the output unchanged. */
bool SosDiseasePathogen(uint64_t seed, uint64_t id, SosPathogen *out);

/* Tries to infect `inf` with pathogen `p` (the pathogen of `id`). `closeness`
 * in [0, SOS_Q16_ONE] is how close the contact was and `durationTicks` how long
 * it lasted; closer and longer contacts are likelier. A being already exposed
 * or infectious, or still immune after recovery, is not infected again and
 * keeps its state. True only when the infection starts. False for a NULL
 * infection or pathogen or random, for id 0 and for an impossible contact. */
bool SosDiseaseExpose(SosInfection *inf, const SosPathogen *p, uint64_t id, uint64_t tick,
                      SosQ16 closeness, uint64_t durationTicks, SosRandom *random);

/* Moves `inf` through exposed, infectious and recovered to susceptible as far
 * as `tick` allows, in one call when a long time passed, and reports which
 * stages it entered. `p` must be the pathogen of inf->pathogen. A NULL `inf` or
 * `p`, or a tick before the current stage began, changes nothing and reports no
 * transitions. */
SosDiseaseOutcome SosDiseaseAdvance(SosInfection *inf, const SosPathogen *p, uint64_t tick);

/* Health lost over [fromTick, fromTick + ticks): the severity accumulated
 * while infectious, on the being health scale (a full bar is SOS_NEED_MAX).
 * `inf` must be the infection as it was at `fromTick`, so the being step calls
 * this before SosDiseaseAdvance: a span may then cross any number of stage
 * boundaries, including recovery, and every tick is counted once. Returns 0 for
 * a NULL infection or pathogen, a susceptible being, a non-positive severity or
 * an empty span. */
uint32_t SosDiseaseDamage(const SosInfection *inf, const SosPathogen *p, uint64_t fromTick,
                          uint64_t ticks);

/* Whether `inf` is spreading at `tick`. Call SosDiseaseAdvance first so the
 * stage is current. False for a NULL infection. */
bool SosDiseaseContagious(const SosInfection *inf, uint64_t tick);

/* The strain derived from `id`: the same lineage and route with the drift in
 * the low bits moved by one small random step, so every property stays within a
 * bounded distance of its parent's. Zero for a NULL `random` or id 0. */
uint64_t SosDiseaseMutate(uint64_t seed, uint64_t id, SosRandom *random);

#endif
