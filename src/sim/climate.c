#include "sim/climate.h"

#include "sim/calendar.h"
#include "sim/noise.h"
#include "sim/random.h"

#include <stddef.h>

/* Every random or noisy quantity is Q16: SOS_Q16_ONE stands for 1.0. The
 * weather is assembled from independent fields, each a pure function of the
 * seed, the position and (for weather systems) the tick. */

/* Climate zones. The world has no latitude, so warmth and wetness come from
 * very low-frequency noise instead: one zone spans thousands of kilometres,
 * and the detail octave breaks the zones up without changing their scale. */
#define ZONE_SHIFT 33u        /* 2^33 world units, about 8400 km across. */
#define ZONE_DETAIL_SHIFT 30u /* About 1050 km across. */
/* A zone's mean annual temperature at sea level. The coldest zones average
 * -5 C and the hottest +28 C, so every region of the unbounded world is
 * livable and the seasons and the daily cycle ride on top of this. */
#define TEMPERATURE_ZONE_MEAN 1150 /* The middle zone, +11.5 C. */
#define TEMPERATURE_ZONE_SPAN 1650 /* Half of the -5 C..+28 C spread. */

/* The air cools 6.5 C for every kilometre climbed above the surface. */
#define LAPSE_CENTI_PER_KM 650
#define UNITS_PER_KM SOS_METRES(1000)

/* Seasons. The calendar year begins on the first day of spring: day 1 is the
 * vernal equinox, so the world that starts at dawn of day 1 starts with the
 * year warming. The temperature peaks around day 92 (midsummer) and troughs
 * around day 275 (midwinter); drier, clearer regions swing harder than wet
 * ones (a monsoon coast against a desert). */
#define SEASON_PEAK_DAY 91 /* Days from day 1 to the warmest day (day 92). */
#define SEASON_PEAK_TURN ((SOS_Q16_ONE * SEASON_PEAK_DAY) / 365)
#define SEASON_SWING_MIN 600 /* 6 C from the yearly mean, at least. */
#define SEASON_SWING_SPAN 1800

/* The daily cycle. The ground warms until mid-afternoon; a clear sky swings
 * about 8 C, an overcast one only 3 C. */
#define DIURNAL_PEAK_TURN ((SOS_Q16_ONE * 15) / 24) /* 15:00 */
#define DIURNAL_SWING_MIN 150
#define DIURNAL_SWING_SPAN 250

/* Passing weather systems a few hundred kilometres across, blown along by
 * the drift below: several hundred kilometres a day, so a fast-forwarded
 * week walks a front past. Their finer octave gives squalls inside a system. */
#define SYSTEM_SHIFT 30u          /* About 1050 km across. */
#define SYSTEM_DETAIL_SHIFT 27u   /* About 130 km across. */
#define SYSTEM_DRIFT_PER_TICK 128 /* World units, about 320 km per day. */

/* Rain needs a cloud sky and moist air; even then it tops out at 30 mm/h,
 * a downpour that ponders the ground. */
#define PRECIPITATION_CLOUD_MIN ((SOS_Q16_ONE * 3) / 5)
#define PRECIPITATION_MAX 3000
/* A warm or cold air mass shifts the temperature by up to 8 C. */
#define SYSTEM_TEMPERATURE_SPAN 800

/* Prevailing wind direction turns slowly over thousands of kilometres; a
 * faster field lets a system carry its own wind. Wind never exceeds 30 m/s. */
#define WIND_SHIFT 32u
#define WIND_DETAIL_SHIFT 29u
#define WIND_SPEED_MAX 30000

/* Temperatures are clamped to what the world's life can meet: -100 C to
 * +70 C, outside the range that its extremes would otherwise reach. */
#define TEMPERATURE_MIN (-10000)
#define TEMPERATURE_MAX 7000

/* sin over the first quadrant in 32 steps: sineTable[i] is sin of
 * i * 90 / 32 degrees in Q16. Interpolation between neighbours keeps the
 * curve smooth, so seasons and hours do not step. */
#define SINE_STEPS 32
static const int32_t sineTable[SINE_STEPS + 1] = {
    0,     3216,  6424,  9616,  12785, 15924, 19024, 22078, 25080, 28020, 30893,
    33692, 36410, 39040, 41576, 44011, 46341, 48559, 50660, 52639, 54491, 56212,
    57798, 59244, 60547, 61705, 62714, 63572, 64277, 64827, 65220, 65457, 65536,
};

enum
{
    SALT_ZONE = 0x300,
    SALT_ZONE_DETAIL,
    SALT_HUMIDITY,
    SALT_SYSTEM,
    SALT_SYSTEM_DETAIL,
    SALT_WIND,
    SALT_WIND_DETAIL,
};

static int64_t Clamp(int64_t value, int64_t minimum, int64_t maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

/* A `value` in Q16 scaled into `scale` units. */
static int64_t ScaleQ16(int64_t value, int64_t scale)
{
    return value * scale / SOS_Q16_ONE;
}

/* sin of an angle in Q16 turns, in [-SOS_Q16_ONE, SOS_Q16_ONE]. */
static int64_t SinTurn(int64_t turn)
{
    const int64_t quarter = SOS_Q16_ONE / 4;
    turn %= SOS_Q16_ONE;
    if (turn < 0)
    {
        turn += SOS_Q16_ONE;
    }
    const int64_t quadrant = turn / quarter;
    int64_t within = turn % quarter;
    if ((quadrant & 1) != 0)
    {
        within = quarter - within;
    }
    int64_t magnitude;
    if (within >= quarter)
    {
        magnitude = sineTable[SINE_STEPS];
    }
    else
    {
        const int64_t step = quarter / SINE_STEPS;
        const int64_t index = within / step;
        const int64_t fraction = within % step;
        magnitude = sineTable[index] + (sineTable[index + 1] - sineTable[index]) * fraction *
                                           (SOS_Q16_ONE / step) / SOS_Q16_ONE;
    }
    return quadrant >= 2 ? -magnitude : magnitude;
}

/* cos of an angle in Q16 turns. */
static int64_t CosTurn(int64_t turn)
{
    return SinTurn(turn + SOS_Q16_ONE / 4);
}

void SosClimateInit(SosClimate *climate, uint64_t worldSeed)
{
    if (climate != NULL)
    {
        climate->seed = SosHashCombine(worldSeed, UINT64_C(0x636c696d617465)); /* "climate" */
    }
}

/* Everything derived from the noise fields, kept together so the tick and the
 * position are read once. */
static void SampleNoise(const SosClimate *climate, int64_t x, int64_t y, uint64_t tick,
                        int64_t *outZoneWarmth, int64_t *outZoneHumidity, int64_t *outSystem,
                        int64_t *outWindDirection)
{
    /* Weather systems drift with time. The unsigned additions wrap for extreme
     * ticks, which keeps every step defined and only matters after millions of
     * years of simulated time. */
    const int64_t systemX = (int64_t)((uint64_t)x - tick * (uint64_t)SYSTEM_DRIFT_PER_TICK);
    const int64_t systemY = (int64_t)((uint64_t)y - tick * (uint64_t)(SYSTEM_DRIFT_PER_TICK / 2));
    const uint64_t zoneSeed = SosHashCombine(climate->seed, SALT_ZONE);
    const uint64_t humiditySeed = SosHashCombine(climate->seed, SALT_HUMIDITY);
    const uint64_t systemSeed = SosHashCombine(climate->seed, SALT_SYSTEM);
    const uint64_t windSeed = SosHashCombine(climate->seed, SALT_WIND);
    /* Averaging a coarse octave with a finer one keeps the field in [-1, 1]. */
    *outZoneWarmth =
        (2 * SosNoise2(zoneSeed, x, y, ZONE_SHIFT) +
         SosNoise2(SosHashCombine(climate->seed, SALT_ZONE_DETAIL), x, y, ZONE_DETAIL_SHIFT)) /
        3;
    *outZoneHumidity = (SosNoise2(humiditySeed, x, y, ZONE_SHIFT) + SOS_Q16_ONE) / 2;
    *outSystem = (2 * SosNoise2(systemSeed, systemX, systemY, SYSTEM_SHIFT) +
                  SosNoise2(SosHashCombine(climate->seed, SALT_SYSTEM_DETAIL), systemX, systemY,
                            SYSTEM_DETAIL_SHIFT)) /
                 3;
    const int64_t wind = (2 * SosNoise2(windSeed, x, y, WIND_SHIFT) +
                          SosNoise2(SosHashCombine(climate->seed, SALT_WIND_DETAIL), systemX,
                                    systemY, WIND_DETAIL_SHIFT)) /
                         3;
    /* Map [-1, 1] to a turn [0, SOS_Q16_ONE), never reaching exactly a full
     * turn so a north wind is distinct from the value just below it. */
    *outWindDirection = Clamp((wind + SOS_Q16_ONE) / 2, 0, SOS_Q16_ONE - 1);
}

bool SosClimateAt(const SosClimate *climate, const SosTerrain *terrain, int64_t x, int64_t y,
                  uint64_t tick, SosWeather *out)
{
    if (climate == NULL || terrain == NULL || out == NULL)
    {
        return false;
    }
    int64_t zoneWarmth = 0;
    int64_t zoneHumidity = 0;
    int64_t system = 0;
    int64_t windDirection = 0;
    SampleNoise(climate, x, y, tick, &zoneWarmth, &zoneHumidity, &system, &windDirection);

    const int64_t cloud = Clamp((system + SOS_Q16_ONE) / 2, 0, SOS_Q16_ONE);
    const int64_t humidity = Clamp(zoneHumidity + system / 4, 0, SOS_Q16_ONE);

    const int64_t zoneTemperature =
        TEMPERATURE_ZONE_MEAN + ScaleQ16(zoneWarmth, TEMPERATURE_ZONE_SPAN);
    /* The year is measured continuously from the day's start, so midnight
     * brings no step in the seasons. */
    const uint64_t yearTick =
        (tick + (uint64_t)SOS_START_TICK_OF_DAY) % (uint64_t)SOS_TICKS_PER_YEAR;
    const int64_t yearPhase =
        (int64_t)(yearTick * (uint64_t)SOS_Q16_ONE / (uint64_t)SOS_TICKS_PER_YEAR) -
        SEASON_PEAK_TURN;
    const int64_t seasonSwing =
        SEASON_SWING_MIN + ScaleQ16(SOS_Q16_ONE - zoneHumidity, SEASON_SWING_SPAN);
    const int64_t seasonTemperature = ScaleQ16(CosTurn(yearPhase), seasonSwing);
    const int64_t dayPhase =
        SosCalendarTickOfDay(tick) * SOS_Q16_ONE / SOS_TICKS_PER_DAY - DIURNAL_PEAK_TURN;
    const int64_t diurnalSwing =
        DIURNAL_SWING_MIN + ScaleQ16(SOS_Q16_ONE - cloud, DIURNAL_SWING_SPAN);
    const int64_t diurnalTemperature = ScaleQ16(CosTurn(dayPhase), diurnalSwing);
    const int64_t surfaceZ = SosTerrainSurfaceZ(terrain, x, y);
    const int64_t lapseTemperature = -(surfaceZ * LAPSE_CENTI_PER_KM) / UNITS_PER_KM;
    const int64_t systemTemperature = ScaleQ16(system, SYSTEM_TEMPERATURE_SPAN);
    const int64_t temperature = Clamp(zoneTemperature + seasonTemperature + diurnalTemperature +
                                          lapseTemperature + systemTemperature,
                                      TEMPERATURE_MIN, TEMPERATURE_MAX);

    uint32_t precipitation = 0u;
    const int64_t excess = cloud - PRECIPITATION_CLOUD_MIN;
    if (excess > 0)
    {
        const int64_t moist = excess * humidity / SOS_Q16_ONE;
        precipitation = (uint32_t)ScaleQ16(moist, PRECIPITATION_MAX);
    }
    const uint32_t windSpeed =
        (uint32_t)(ScaleQ16(Clamp(system + SOS_Q16_ONE, 0, 2 * (int64_t)SOS_Q16_ONE),
                            WIND_SPEED_MAX) /
                   2);

    *out = (SosWeather){
        .temperature = (int32_t)temperature,
        .humidity = (SosQ16)humidity,
        .precipitation = precipitation,
        .windDirection = (SosQ16)windDirection,
        .windSpeed = windSpeed,
        .cloudCover = (SosQ16)cloud,
        .snow = precipitation > 0u && temperature <= 0,
    };
    return true;
}
