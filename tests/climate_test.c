#include "sim/calendar.h"
#include "sim/climate.h"
#include "sim/random.h"
#include "sim/terrain.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
/* Climate clamps temperature to [-100 C, +70 C] and never lets the other
 * fields leave their units; the test pins the contract, not the internals. */
#define TEMPERATURE_FLOOR (-10000)
#define TEMPERATURE_CEILING 7000
#define PRECIPITATION_MOST 3000
#define WIND_SPEED_MOST 30000
/* The header promises a mean annual temperature at sea level from about -5 C
 * in the coldest zone to +28 C in the hottest. The test allows a small margin
 * for the weather systems that ride on top. */
#define ZONE_TEMPERATURE_FLOOR (-600)
#define ZONE_TEMPERATURE_CEILING 2900
/* The published lapse rate and the world's units, restated so the test fails
 * if the model stops cooling the air by 6.5 C per kilometre. */
#define LAPSE_CENTI_PER_KM 650
#define UNITS_PER_KM SOS_METRES(1000)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "climate test failed: %s\n", message);
        exit(1);
    }
}

static SosWeather WeatherAt(const SosClimate *climate, const SosTerrain *terrain, int64_t x,
                            int64_t y, uint64_t tick)
{
    SosWeather weather;
    Expect(SosClimateAt(climate, terrain, x, y, tick, &weather), "the climate is sampled");
    return weather;
}

static void TestRanges(const SosClimate *climate, const SosTerrain *terrain)
{
    const int64_t positions[] = {0, SOS_METRES(1), SOS_METRES(-5000), SOS_METRES(200000),
                                 SOS_METRES(-9000000)};
    const uint64_t ticks[] = {0u,
                              1u,
                              (uint64_t)SOS_TICKS_PER_DAY,
                              123456u,
                              (uint64_t)SOS_TICKS_PER_YEAR,
                              1000u * (uint64_t)SOS_TICKS_PER_YEAR,
                              UINT64_MAX};
    for (size_t position = 0u; position < sizeof(positions) / sizeof(positions[0]); ++position)
    {
        for (size_t index = 0u; index < sizeof(ticks) / sizeof(ticks[0]); ++index)
        {
            const SosWeather weather = WeatherAt(climate, terrain, positions[position],
                                                 -positions[position], ticks[index]);
            Expect(weather.temperature >= TEMPERATURE_FLOOR &&
                       weather.temperature <= TEMPERATURE_CEILING,
                   "temperature stays inside the survivable range");
            Expect(weather.humidity >= 0 && weather.humidity <= SOS_Q16_ONE,
                   "humidity is a fraction");
            Expect(weather.cloudCover >= 0 && weather.cloudCover <= SOS_Q16_ONE,
                   "cloud cover is a fraction");
            Expect(weather.precipitation <= PRECIPITATION_MOST, "precipitation is bounded");
            Expect(weather.windSpeed <= WIND_SPEED_MOST, "wind speed is bounded");
            Expect(weather.windDirection >= 0 && weather.windDirection < SOS_Q16_ONE,
                   "wind direction is a turn");
            Expect(!weather.snow || (weather.precipitation > 0u && weather.temperature <= 0),
                   "snow only falls when it is cold and wet");
        }
    }
}

/* The yearly mean temperature at a point put back at sea level: the seasons
 * and the daily cycle average out over a whole year sampled every six hours,
 * and the published lapse rate removes the height. */
static int64_t SeaLevelYearMean(const SosClimate *climate, const SosTerrain *terrain, int64_t x,
                                int64_t y)
{
    const int days = (int)SOS_DAYS_PER_YEAR;
    const int quarters = 4; /* 00:00, 06:00, 12:00 and 18:00 cancel the day. */
    int64_t total = 0;
    for (int day = 0; day < days; ++day)
    {
        for (int quarter = 0; quarter < quarters; ++quarter)
        {
            const uint64_t tick = (uint64_t)day * (uint64_t)SOS_TICKS_PER_DAY +
                                  (uint64_t)quarter * 6u * (uint64_t)SOS_TICKS_PER_HOUR;
            total += WeatherAt(climate, terrain, x, y, tick).temperature;
        }
    }
    const int64_t lapse = SosTerrainSurfaceZ(terrain, x, y) * LAPSE_CENTI_PER_KM / UNITS_PER_KM;
    return total / (int64_t)(days * quarters) + lapse;
}

/* Every zone's mean annual temperature at sea level must be habitable. The
 * zones used to span about +-50 C, so large regions, including the start site
 * at the origin, were too hot or too cold to live in. Zones are about 8400 km
 * across, so the sample points are more than a zone apart. */
static void TestZoneTemperatures(const SosClimate *climate, const SosTerrain *terrain)
{
    const int64_t spacing = SOS_METRES(1200000);
    for (int gridX = -5; gridX <= 5; ++gridX)
    {
        for (int gridY = -5; gridY <= 5; ++gridY)
        {
            const int64_t mean = SeaLevelYearMean(climate, terrain, (int64_t)gridX * spacing,
                                                  (int64_t)gridY * spacing);
            Expect(mean >= ZONE_TEMPERATURE_FLOOR && mean <= ZONE_TEMPERATURE_CEILING,
                   "the mean annual temperature at sea level is habitable");
        }
    }
    /* The start site is no exception. */
    const int64_t originMean = SeaLevelYearMean(climate, terrain, 0, 0);
    Expect(originMean >= ZONE_TEMPERATURE_FLOOR && originMean <= ZONE_TEMPERATURE_CEILING,
           "the start site is habitable");
}

/* Distance between two turn angles, in Q16. */
static int64_t TurnDistance(SosQ16 a, SosQ16 b)
{
    int64_t difference = ((int64_t)a - (int64_t)b) % SOS_Q16_ONE;
    if (difference < 0)
    {
        difference += SOS_Q16_ONE;
    }
    return difference <= SOS_Q16_ONE / 2 ? difference : SOS_Q16_ONE - difference;
}

/* The fields are smooth functions of place and time: a step of a metre or a
 * second barely moves them, so beings can walk and time can flow. */
static void TestContinuity(const SosClimate *climate, const SosTerrain *terrain)
{
    SosRandom random = SosRandomStream(SEED, 11u);
    for (int trial = 0; trial < 64; ++trial)
    {
        const int64_t x = SosRandomRange(&random, SOS_METRES(-100000), SOS_METRES(100000));
        const int64_t y = SosRandomRange(&random, SOS_METRES(-100000), SOS_METRES(100000));
        const uint64_t tick = (uint64_t)SosRandomRange(&random, 0, 5000) * SOS_TICKS_PER_HOUR;
        const SosWeather here = WeatherAt(climate, terrain, x, y, tick);
        const SosWeather near =
            WeatherAt(climate, terrain, x + SOS_METRES(1), y - SOS_METRES(1), tick + 30u);
        const int64_t temperatureStep = near.temperature - here.temperature;
        Expect(temperatureStep > -6 && temperatureStep < 6, "a metre and a second barely warm");
        Expect(TurnDistance(near.windDirection, here.windDirection) < 8, "the wind barely turns");
        const int64_t windStep = (int64_t)near.windSpeed - here.windSpeed;
        Expect(windStep > -8 && windStep < 8, "the wind barely rises");
        const int64_t humidityStep = (int64_t)near.humidity - here.humidity;
        Expect(humidityStep > -8 && humidityStep < 8, "the air barely wets");
        const int64_t cloudStep = (int64_t)near.cloudCover - here.cloudCover;
        Expect(cloudStep > -8 && cloudStep < 8, "the sky barely clouds");
    }
}

/* One point through a whole year: summer and winter must part by a wide
 * margin, since every zone swings at least 12 C from peak to trough. */
static void TestSeason(const SosClimate *climate, const SosTerrain *terrain)
{
    const int64_t x = SOS_METRES(3210);
    const int64_t y = SOS_METRES(-1777);
    int32_t warmest = INT32_MIN;
    int32_t coldest = INT32_MAX;
    for (int64_t day = 0; day < SOS_DAYS_PER_YEAR; ++day)
    {
        const SosWeather weather =
            WeatherAt(climate, terrain, x, y, (uint64_t)day * (uint64_t)SOS_TICKS_PER_DAY);
        if (weather.temperature > warmest)
        {
            warmest = weather.temperature;
        }
        if (weather.temperature < coldest)
        {
            coldest = weather.temperature;
        }
    }
    Expect(warmest - coldest > 800, "the seasons swing the temperature");
}

/* Places a fraction of a zone apart, so the season has the same phase but the
 * passing weather systems cancel when many are averaged. */
static const int64_t seasonPlaces[][2] = {
    {0, 0},
    {SOS_METRES(100000), 0},
    {0, SOS_METRES(100000)},
    {SOS_METRES(-100000), SOS_METRES(75000)},
    {SOS_METRES(250000), SOS_METRES(-130000)},
    {SOS_METRES(-400000), SOS_METRES(-90000)},
    {SOS_METRES(60000), SOS_METRES(310000)},
    {SOS_METRES(-70000), SOS_METRES(-500000)},
};

/* Mean temperature over the days [firstDay, lastDay) of the year, averaged
 * over many years and places. The weather systems and the time of day cancel,
 * leaving the season, so day comparisons reveal the phase. */
static int64_t SeasonMean(const SosClimate *climate, const SosTerrain *terrain, int firstDay,
                          int lastDay)
{
    const int years = 24;
    const size_t places = sizeof(seasonPlaces) / sizeof(seasonPlaces[0]);
    int64_t total = 0;
    int64_t samples = 0;
    for (int year = 0; year < years; ++year)
    {
        for (int day = firstDay; day < lastDay; ++day)
        {
            for (size_t place = 0u; place < places; ++place)
            {
                const uint64_t tick = (uint64_t)year * (uint64_t)SOS_TICKS_PER_YEAR +
                                      (uint64_t)day * (uint64_t)SOS_TICKS_PER_DAY;
                total += WeatherAt(climate, terrain, seasonPlaces[place][0], seasonPlaces[place][1],
                                   tick)
                             .temperature;
                ++samples;
            }
        }
    }
    return total / samples;
}

/* The calendar begins in spring: day 1 is the vernal equinox, so the year is
 * near its mean but warming, reaches a peak around day 92 and a trough around
 * day 275. Before the fix the year peaked at day 200, which put day 1 in the
 * depth of winter. */
static void TestSeasonPhase(const SosClimate *climate, const SosTerrain *terrain)
{
    int warmestDay = 0;
    int coldestDay = 0;
    int64_t warmest = INT64_MIN;
    int64_t coldest = INT64_MAX;
    for (int day = 0; day < (int)SOS_DAYS_PER_YEAR; ++day)
    {
        const int64_t mean = SeasonMean(climate, terrain, day, day + 1);
        if (mean > warmest)
        {
            warmest = mean;
            warmestDay = day + 1;
        }
        if (mean < coldest)
        {
            coldest = mean;
            coldestDay = day + 1;
        }
    }
    Expect(warmestDay > 70 && warmestDay < 130, "the warmest day is around midsummer");
    Expect(coldestDay > 240 && coldestDay < 310, "the coldest day is around midwinter");

    const int64_t earlySpring = SeasonMean(climate, terrain, 0, 10);
    const int64_t lateWinter = SeasonMean(climate, terrain, 274, 284);
    const int64_t midSpring = SeasonMean(climate, terrain, 30, 40);
    const int64_t midsummer = SeasonMean(climate, terrain, 88, 98);
    Expect(earlySpring > lateWinter + 400, "day 1 is spring, not the depth of winter");
    Expect(midSpring > earlySpring + 200, "spring warms as it advances");
    Expect(midsummer > earlySpring + 400, "summer is warmer than spring");
}

/* The steepest short slope near a few places fixes the lapse rate: climbing
 * cools the air by 6.5 C per kilometre and nothing else changes that much over
 * a few dozen metres. */
static void TestLapseRate(const SosClimate *climate, const SosTerrain *terrain)
{
    const int64_t origins[] = {0, SOS_METRES(4000), SOS_METRES(-9000)};
    const int64_t step = SOS_METRES(32);
    int tested = 0;
    for (size_t index = 0u; index < sizeof(origins) / sizeof(origins[0]); ++index)
    {
        int64_t bestX = origins[index];
        int64_t bestDrop = 0;
        for (int64_t x = origins[index] - SOS_METRES(3000); x <= origins[index] + SOS_METRES(3000);
             x += step)
        {
            const int64_t drop =
                SosTerrainSurfaceZ(terrain, x + step, 0) - SosTerrainSurfaceZ(terrain, x, 0);
            if (drop > bestDrop)
            {
                bestDrop = drop;
                bestX = x;
            }
        }
        if (bestDrop < SOS_METRES(10))
        {
            continue;
        }
        const uint64_t tick = 98765u;
        const int64_t lowZ = SosTerrainSurfaceZ(terrain, bestX, 0);
        const int64_t highZ = SosTerrainSurfaceZ(terrain, bestX + step, 0);
        const SosWeather low = WeatherAt(climate, terrain, bestX, 0, tick);
        const SosWeather high = WeatherAt(climate, terrain, bestX + step, 0, tick);
        if (low.temperature <= TEMPERATURE_FLOOR + 100 ||
            high.temperature >= TEMPERATURE_CEILING - 100)
        {
            continue; /* A clamped reading hides the lapse. */
        }
        const int64_t expected = -(highZ - lowZ) * LAPSE_CENTI_PER_KM / UNITS_PER_KM;
        const int64_t measured = high.temperature - low.temperature;
        Expect(highZ > lowZ, "the pair really climbs");
        Expect(measured - expected > -4 && measured - expected < 4,
               "climbing cools the air at the lapse rate");
        ++tested;
    }
    Expect(tested > 0, "the terrain offers a slope to measure the lapse rate on");
}

static void TestDeterminism(const SosClimate *first, const SosClimate *second,
                            const SosTerrain *terrain)
{
    bool differs = false;
    for (int64_t x = -2000; x <= 2000; x += 500)
    {
        for (int64_t y = -2000; y <= 2000; y += 500)
        {
            const uint64_t tick = (uint64_t)(x + 3000) * 7919u;
            const SosWeather a = WeatherAt(first, terrain, SOS_METRES(x), SOS_METRES(y), tick);
            const SosWeather b = WeatherAt(first, terrain, SOS_METRES(x), SOS_METRES(y), tick);
            const SosWeather c = WeatherAt(second, terrain, SOS_METRES(x), SOS_METRES(y), tick);
            Expect(a.temperature == b.temperature && a.humidity == b.humidity &&
                       a.precipitation == b.precipitation && a.windDirection == b.windDirection &&
                       a.windSpeed == b.windSpeed && a.cloudCover == b.cloudCover &&
                       a.snow == b.snow,
                   "the same world samples the same weather");
            differs = differs || a.temperature != c.temperature || a.humidity != c.humidity ||
                      a.precipitation != c.precipitation;
        }
    }
    Expect(differs, "different seeds give different weather");
}

/* Fixed points of the weather. Every platform and compiler must produce
 * exactly these readings. */
static void TestGoldenValues(const SosClimate *climate, const SosTerrain *terrain)
{
    uint64_t fingerprint = UINT64_C(0x9E3779B97F4A7C15);
    for (int index = 0; index < 24; ++index)
    {
        const int64_t x = SOS_METRES(index * 37 - 400);
        const int64_t y = SOS_METRES(index * 53 - 300);
        const uint64_t tick = (uint64_t)index * 12345u * (uint64_t)SOS_TICKS_PER_HOUR + 7654u;
        const SosWeather weather = WeatherAt(climate, terrain, x, y, tick);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)weather.temperature);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)weather.humidity);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)weather.precipitation);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)weather.windDirection);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)weather.windSpeed);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)weather.cloudCover);
        fingerprint = SosHashCombine(fingerprint, weather.snow ? 1u : 0u);
    }
    const uint64_t expected = UINT64_C(0xB45DF4EF43F86A85);
    if (fingerprint != expected)
    {
        fprintf(stderr, "climate fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the weather matches the reference");
    }
}

static void TestInvalid(const SosClimate *climate, const SosTerrain *terrain)
{
    SosWeather weather;
    Expect(!SosClimateAt(NULL, terrain, 0, 0, 0u, &weather), "a null climate is rejected");
    Expect(!SosClimateAt(climate, NULL, 0, 0, 0u, &weather), "null terrain is rejected");
    Expect(!SosClimateAt(climate, terrain, 0, 0, 0u, NULL), "a null output is rejected");
    SosClimate zeroed = {0};
    const SosWeather far = WeatherAt(&zeroed, terrain, INT64_MIN, INT64_MAX, UINT64_MAX);
    Expect(far.temperature >= TEMPERATURE_FLOOR && far.temperature <= TEMPERATURE_CEILING,
           "an uninitialised climate is still safe");
    SosClimateInit(NULL, SEED); /* Must not crash. */
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    SosClimate climate;
    SosClimateInit(&climate, SEED);
    SosClimate other;
    SosClimateInit(&other, SEED + 1u);
    TestRanges(&climate, &terrain);
    TestZoneTemperatures(&climate, &terrain);
    TestContinuity(&climate, &terrain);
    TestSeason(&climate, &terrain);
    TestSeasonPhase(&climate, &terrain);
    TestLapseRate(&climate, &terrain);
    TestDeterminism(&climate, &other, &terrain);
    TestGoldenValues(&climate, &terrain);
    TestInvalid(&climate, &terrain);
    return 0;
}
