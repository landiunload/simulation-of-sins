#ifndef SOS_SIM_CLIMATE_H
#define SOS_SIM_CLIMATE_H

#include "sim/fixed.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Weather and climate.
 *
 * Like the terrain, the weather is a pure function of the world seed, the
 * position and the tick: nothing is stored, so any machine that knows those
 * three values computes the same temperature, rain and wind, and time can be
 * fast-forwarded by calling again at a later tick. The world has no latitude,
 * so climate zones instead come from very low-frequency noise over thousands
 * of kilometres: a region is cold or hot, dry or wet for a whole journey.
 * Elevation cools the air at the usual lapse rate, the day of the year gives
 * seasons, the hour a daily cycle, and passing weather systems ride over all
 * of it on noise that drifts with time. A zone's mean annual temperature at
 * sea level runs from about -5 C in the coldest zones to +28 C in the hottest,
 * so no region is uninhabitable. The calendar year starts on the first day of
 * spring: day 1 is the vernal equinox, the year is warmest around day 92 and
 * coldest around day 275. A caller that changes the weather (a fire, a shelter)
 * adds its own sparse layer over this function, exactly as it does for the
 * terrain. */

typedef struct SosClimate
{
    uint64_t seed;
} SosClimate;

/* One reading of the weather, all integer so every machine agrees. */
typedef struct SosWeather
{
    /* Air temperature in centi-degrees Celsius (hundredths of a degree). */
    int32_t temperature;
    /* Water held in the air, 0 (bone dry) to SOS_Q16_ONE (saturated). */
    SosQ16 humidity;
    /* Rain or snow falling, hundredths of a millimetre per hour. */
    uint32_t precipitation;
    /* Bearing the wind blows from, a fraction of a turn clockwise from north:
     * 0 is a north wind, SOS_Q16_ONE / 4 an east wind. */
    SosQ16 windDirection;
    /* Wind speed in millimetres per second. */
    uint32_t windSpeed;
    /* Sky hidden by cloud, 0 (clear) to SOS_Q16_ONE (overcast). */
    SosQ16 cloudCover;
    /* True when the precipitation falls as snow rather than rain. */
    bool snow;
} SosWeather;

/* Prepares a climate for `worldSeed`. A NULL climate is ignored. Every other
 * function works on a zeroed climate too, so failing to initialise one loses
 * the seed's variety but never safety. */
void SosClimateInit(SosClimate *climate, uint64_t worldSeed);

/* The weather at (x, y), in world units, at `tick`. False and `out` untouched
 * when any pointer is NULL; otherwise true and `out` filled. Every field
 * stays inside its documented range for any position, however far from the
 * origin, and any tick, however large, and equal inputs give equal weather on
 * every machine. */
bool SosClimateAt(const SosClimate *climate, const SosTerrain *terrain, int64_t x, int64_t y,
                  uint64_t tick, SosWeather *out);

#endif
