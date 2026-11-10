#ifndef SOS_SIM_CENSUS_H
#define SOS_SIM_CENSUS_H

#include "sim/beings.h"
#include "sim/fixed.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Demography of the living.
 *
 * A census is a snapshot that a UI or a balancing tool reads in a single pass
 * over the beings: how many live, how they split by sex, age, activity and
 * pregnancy, how healthy and how sated they are, and two summary numbers, the
 * median age and the dependency ratio. Nothing is stored between calls, so
 * taking a census changes nothing and always sees the same world the same way.
 * The beings are read only through SosBeingsSlotCount and SosBeingsView, so
 * the census never depends on how beings are kept. */

/* Ages are counted in five-year buckets; the last bucket holds everyone of a
 * hundred and over, so there are indices 0..20. Sexes are indexed by SosSex,
 * activities by SosActivity. */
#define SOS_CENSUS_AGE_BUCKET_YEARS 5u
#define SOS_CENSUS_AGE_BUCKETS 21u
#define SOS_CENSUS_SEX_COUNT 2u
#define SOS_CENSUS_ACTIVITY_COUNT 6u

/* Ages that make a dependent: younger than 15, or 65 and over. */
#define SOS_CENSUS_DEPENDENT_YOUNG 15u
#define SOS_CENSUS_DEPENDENT_OLD 65u

/* One need summarised over the living. */
typedef struct SosCensusMeasure
{
    /* Mean as a Q16 share of SOS_NEED_MAX: SOS_Q16_ONE is the full scale, as
     * in perfect health. Zero when nobody lives. */
    SosQ16 mean;
    /* Smallest raw value among the living, in [0, SOS_NEED_MAX]; zero when
     * nobody lives. */
    uint32_t minimum;
} SosCensusMeasure;

typedef struct SosCensus
{
    /* The tick the census was taken at. */
    uint64_t tick;
    uint32_t total;
    uint32_t bySex[SOS_CENSUS_SEX_COUNT];
    /* age[sex][bucket]: bucket b covers ages [5b, 5b + 5) years, and the last
     * bucket everything from 100 years on. */
    uint32_t age[SOS_CENSUS_SEX_COUNT][SOS_CENSUS_AGE_BUCKETS];
    /* Living beings by SosActivity; an unknown activity is not counted. */
    uint32_t byActivity[SOS_CENSUS_ACTIVITY_COUNT];
    uint32_t pregnant;
    SosCensusMeasure health;
    SosCensusMeasure hunger;
    SosCensusMeasure thirst;
    SosCensusMeasure fatigue;
    /* Lower median of the ages in whole years: the age at position
     * (total - 1) / 2 of the ages sorted ascending, so a tied middle pair
     * reports the younger. Zero when nobody lives. */
    uint64_t medianAgeYears;
    /* (under 15 and 65 and over) / (the rest), as Q16. Zero when nobody of
     * working age lives. */
    SosQ16 dependencyRatio;
    /* Serial of the oldest being, the greatest serial when several share the
     * oldest age. Zero when nobody lives. */
    uint64_t eldestSerial;
} SosCensus;

/* Fills `out` from the living beings at `tick`, leaving the beings untouched.
 * False when `beings` or `out` is NULL, leaving `out` unchanged; an empty
 * population is not an error and fills a census of zeroes. */
bool SosCensusTake(const SosBeings *beings, uint64_t tick, SosCensus *out);

/* The yearly growth of the total between two censuses, as a Q16 fraction of
 * the earlier total: SOS_Q16_ONE means the population grows by its own size
 * each year, and a negative value means it shrank. `ticksBetween` is the ticks
 * between the two snapshots. Zero when either census is NULL, nobody lived
 * before, or no time passed. */
SosQ16 SosCensusGrowthPerYear(const SosCensus *before, const SosCensus *after,
                              uint64_t ticksBetween);

/* Writes a compact multi-line report of `c` into `out`, never writing past
 * `capacity` bytes; the text always ends in a NUL. False when `out` is NULL,
 * `capacity` is zero, or the report did not fit whole, in which case the
 * truncated text is still NUL-terminated. */
bool SosCensusFormat(const SosCensus *c, char *out, size_t capacity);

#endif
