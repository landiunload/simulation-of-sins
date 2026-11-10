#include "sim/body.h"

#include "sim/calendar.h"
#include "sim/space.h"

#include <stddef.h>

/* Adult height, in millimetres, as a line over the height potential: each sex
 * spans its own range because men's heights spread wider. The median man is
 * 175 cm and the median woman 162 cm at a potential of 0.5, and the extremes
 * across both sexes reach 150 cm and 195 cm. */
#define SOS_BODY_FEMALE_HEIGHT_BASE_MM 1500
#define SOS_BODY_FEMALE_HEIGHT_SPAN_MM 240
#define SOS_BODY_MALE_HEIGHT_BASE_MM 1550
#define SOS_BODY_MALE_HEIGHT_SPAN_MM 400
#define SOS_BODY_HEIGHT_LIMIT_MM (SOS_BODY_MALE_HEIGHT_BASE_MM + SOS_BODY_MALE_HEIGHT_SPAN_MM)

/* Mass follows height as a BMI in tenths of kg/m^2: a 1.70 m adult of BMI 22
 * weighs about 64 kg, near the middle of the living range. */
#define SOS_BODY_BMI_TENTHS 220
#define SOS_BODY_MAX_MASS_GRAMS                                                                    \
    (SOS_BODY_BMI_TENTHS * SOS_BODY_HEIGHT_LIMIT_MM * SOS_BODY_HEIGHT_LIMIT_MM / 10000)

/* Walking speed, in world units (1/1024 m) per second, for the adult a speed
 * potential describes: the slowest adult manages 1.1 m/s and the fastest 1.7,
 * so the median adult (0.5) walks at the 1.4 m/s the rest of the simulation
 * assumes. The age curve scales this whole speed later, so an infant barely
 * moves instead of inheriting the adult floor. */
#define SOS_BODY_WALK_MIN_UNITS_PER_SECOND (SOS_METRES(11) / 10)
#define SOS_BODY_WALK_MAX_UNITS_PER_SECOND (SOS_METRES(17) / 10)

/* Lifting without strain is this fraction of the body's own mass scaled by
 * strength: a strong adult carries about half its mass. */
#define SOS_BODY_CARRY_NUMERATOR 1
#define SOS_BODY_CARRY_DENOMINATOR 2

/* A Q16 fraction for a whole percentage, so the curves read as the stated
 * percentages and stay integer constant expressions. */
#define PERCENT(value) ((SosQ16)(SOS_Q16_ONE * (value) / 100))

#define COUNT_OF(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))

/* One vertex of a curve: the value at a whole year of age. Neighbouring
 * points are joined by a straight line, so a curve needs only its bends. */
typedef struct CurvePoint
{
    int32_t ageYears;
    SosQ16 fraction;
} CurvePoint;

/* The fraction of the adult height reached by each age. Infancy grows
 * fastest, the adolescent spurt of girls comes about two years before that of
 * boys, and both tables end at the adult height. */
static const CurvePoint growthFemale[] = {
    {.ageYears = 0, .fraction = PERCENT(30)}, /* birth: near half a metre */
    {.ageYears = 1, .fraction = PERCENT(45)}, /* infancy */
    {.ageYears = 3, .fraction = PERCENT(56)},
    {.ageYears = 6, .fraction = PERCENT(67)}, /* childhood */
    {.ageYears = 9, .fraction = PERCENT(76)},
    {.ageYears = 11, .fraction = PERCENT(86)}, /* spurt */
    {.ageYears = 12, .fraction = PERCENT(93)},
    {.ageYears = 13, .fraction = PERCENT(97)},
    {.ageYears = 14, .fraction = PERCENT(99)},
    {.ageYears = 16, .fraction = PERCENT(100)}, /* adult */
};
static const CurvePoint growthMale[] = {
    {.ageYears = 0, .fraction = PERCENT(30)},   {.ageYears = 1, .fraction = PERCENT(44)},
    {.ageYears = 3, .fraction = PERCENT(55)},   {.ageYears = 6, .fraction = PERCENT(66)},
    {.ageYears = 9, .fraction = PERCENT(75)},   {.ageYears = 12, .fraction = PERCENT(82)},
    {.ageYears = 13, .fraction = PERCENT(88)},  {.ageYears = 14, .fraction = PERCENT(93)},
    {.ageYears = 15, .fraction = PERCENT(97)},  {.ageYears = 16, .fraction = PERCENT(99)},
    {.ageYears = 18, .fraction = PERCENT(100)},
};

/* The fraction of the adult strength and endurance reached by each age. It
 * climbs with growth, holds through the adult plateau and falls away after
 * forty. */
static const CurvePoint powerCurve[] = {
    {.ageYears = 0, .fraction = PERCENT(10)},
    {.ageYears = 1, .fraction = PERCENT(25)},
    {.ageYears = 3, .fraction = PERCENT(42)},
    {.ageYears = 6, .fraction = PERCENT(58)},
    {.ageYears = 10, .fraction = PERCENT(74)},
    {.ageYears = 13, .fraction = PERCENT(86)},
    {.ageYears = 16, .fraction = PERCENT(96)},
    {.ageYears = 18, .fraction = PERCENT(100)}, /* adult plateau */
    {.ageYears = 40, .fraction = PERCENT(100)},
    {.ageYears = 50, .fraction = PERCENT(96)},
    {.ageYears = 60, .fraction = PERCENT(88)},
    {.ageYears = 70, .fraction = PERCENT(76)},
    {.ageYears = 80, .fraction = PERCENT(62)},
    {.ageYears = 90, .fraction = PERCENT(52)},
    {.ageYears = 100, .fraction = PERCENT(45)},
};

/* Speed rises and, in old age, falls faster than strength: reactions slow
 * before muscles weaken. */
static const CurvePoint speedCurve[] = {
    {.ageYears = 0, .fraction = PERCENT(8)},    {.ageYears = 1, .fraction = PERCENT(24)},
    {.ageYears = 3, .fraction = PERCENT(44)},   {.ageYears = 6, .fraction = PERCENT(62)},
    {.ageYears = 10, .fraction = PERCENT(78)},  {.ageYears = 13, .fraction = PERCENT(90)},
    {.ageYears = 16, .fraction = PERCENT(98)},  {.ageYears = 18, .fraction = PERCENT(100)},
    {.ageYears = 40, .fraction = PERCENT(100)}, {.ageYears = 50, .fraction = PERCENT(94)},
    {.ageYears = 60, .fraction = PERCENT(83)},  {.ageYears = 70, .fraction = PERCENT(68)},
    {.ageYears = 80, .fraction = PERCENT(52)},  {.ageYears = 90, .fraction = PERCENT(40)},
    {.ageYears = 100, .fraction = PERCENT(34)},
};

/* The curve value at an age, interpolating between the two points around it
 * and holding the ends. `count` is at least one. */
static SosQ16 CurveValue(const CurvePoint *points, uint32_t count, int64_t ageTicks)
{
    for (uint32_t index = 0u; index + 1u < count; ++index)
    {
        const int64_t fromTick = (int64_t)points[index].ageYears * SOS_TICKS_PER_YEAR;
        const int64_t toTick = (int64_t)points[index + 1u].ageYears * SOS_TICKS_PER_YEAR;
        if (ageTicks < toTick)
        {
            const SosQ16 t = SosQ16Ratio(ageTicks - fromTick, toTick - fromTick);
            return SosQ16Lerp(points[index].fraction, points[index + 1u].fraction, t);
        }
    }
    return points[count - 1u].fraction;
}

/* The height an adult of `sex` reaches at `height` on the potential scale. */
static int32_t AdultHeightMm(SosQ16 height, SosSex sex)
{
    const bool male = sex == SOS_SEX_MALE;
    const int64_t base = male ? SOS_BODY_MALE_HEIGHT_BASE_MM : SOS_BODY_FEMALE_HEIGHT_BASE_MM;
    const int64_t span = male ? SOS_BODY_MALE_HEIGHT_SPAN_MM : SOS_BODY_FEMALE_HEIGHT_SPAN_MM;
    return (int32_t)(base + (int64_t)height * span / SOS_Q16_ONE);
}

/* Mass for a height, from the BMI: grams = BMI * (mm / 1000)^2 * 1000. */
static int32_t MassGrams(int32_t heightMm)
{
    return (int32_t)((int64_t)SOS_BODY_BMI_TENTHS * heightMm * heightMm / 10000);
}

static int64_t ClampInt64(int64_t value, int64_t minimum, int64_t maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

/* The walking speed of an adult with this speed potential, in world units per
 * second: a line from the slowest adult to the fastest. */
static int64_t AdultWalkSpeed(SosQ16 speed)
{
    const int64_t potential = SosQ16Clamp(speed, 0, SOS_Q16_ONE);
    const int64_t span = SOS_BODY_WALK_MAX_UNITS_PER_SECOND - SOS_BODY_WALK_MIN_UNITS_PER_SECOND;
    return SOS_BODY_WALK_MIN_UNITS_PER_SECOND + span * potential / SOS_Q16_ONE;
}

bool SosBodyAt(const SosBodyPotential *potential, SosSex sex, int64_t ageTicks, SosBody *out)
{
    if (potential == NULL || out == NULL || ageTicks < 0 ||
        (sex != SOS_SEX_FEMALE && sex != SOS_SEX_MALE))
    {
        return false;
    }
    const bool male = sex == SOS_SEX_MALE;
    const SosQ16 height = SosQ16Clamp(potential->height, 0, SOS_Q16_ONE);
    const SosQ16 growth =
        CurveValue(male ? growthMale : growthFemale,
                   male ? COUNT_OF(growthMale) : COUNT_OF(growthFemale), ageTicks);
    const int32_t heightMm = (int32_t)((int64_t)AdultHeightMm(height, sex) * growth / SOS_Q16_ONE);
    const SosQ16 power = CurveValue(powerCurve, COUNT_OF(powerCurve), ageTicks);
    const SosQ16 swift = CurveValue(speedCurve, COUNT_OF(speedCurve), ageTicks);
    *out = (SosBody){
        .heightMm = heightMm,
        .massGrams = MassGrams(heightMm),
        .strength = SosQ16Mul(SosQ16Clamp(potential->strength, 0, SOS_Q16_ONE), power),
        .endurance = SosQ16Mul(SosQ16Clamp(potential->endurance, 0, SOS_Q16_ONE), power),
        .speed = (SosQ16)(AdultWalkSpeed(potential->speed) * swift / SOS_Q16_ONE),
    };
    return true;
}

int64_t SosBodyWalkSpeed(const SosBody *body)
{
    if (body == NULL)
    {
        return 0;
    }
    return ClampInt64(body->speed, 0, SOS_BODY_WALK_MAX_UNITS_PER_SECOND);
}

int32_t SosBodyCarryGrams(const SosBody *body)
{
    if (body == NULL)
    {
        return 0;
    }
    const int64_t mass = ClampInt64(body->massGrams, 0, SOS_BODY_MAX_MASS_GRAMS);
    const int64_t strength = ClampInt64(body->strength, 0, SOS_Q16_ONE);
    return (int32_t)(mass * strength * SOS_BODY_CARRY_NUMERATOR /
                     ((int64_t)SOS_Q16_ONE * SOS_BODY_CARRY_DENOMINATOR));
}
