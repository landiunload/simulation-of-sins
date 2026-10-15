#include "sim/beings.h"

#include "sim/calendar.h"
#include "sim/random.h"

#include <stddef.h>
#include <stdlib.h>

#define MINUTES(count) ((int64_t)(count) * SOS_TICKS_PER_MINUTE)
#define HOURS(count) ((int64_t)(count) * SOS_TICKS_PER_HOUR)
#define DAYS(count) ((int64_t)(count) * SOS_TICKS_PER_DAY)
#define YEARS(count) ((int64_t)(count) * SOS_TICKS_PER_YEAR)
#define NEED_FRACTION(numerator, denominator)                                                      \
    ((uint32_t)((uint64_t)SOS_NEED_MAX * (numerator) / (denominator)))

/* Time for a need to fill from nothing to unbearable, or to be satisfied. */
#define HUNGER_PERIOD DAYS(3)
#define THIRST_PERIOD DAYS(2)
#define AWAKE_PERIOD HOURS(18)
#define SLEEP_PERIOD HOURS(8)
#define EAT_PERIOD MINUTES(30)
#define DRINK_PERIOD MINUTES(5)
#define STARVATION_PERIOD DAYS(3)
#define HEALING_PERIOD DAYS(10)
#define WANDER_PERIOD HOURS(2)

#define CHILDHOOD_END YEARS(12)
#define ADULTHOOD YEARS(16)
#define FEMALE_FERTILITY_END YEARS(45)
#define MALE_FERTILITY_END YEARS(70)
#define PREGNANCY DAYS(270)
/* A mother nursing a baby does not conceive again for this long. */
#define BIRTH_INTERVAL YEARS(2)
/* Average time a fertile couple spends together before a conception. */
#define CONCEPTION_PERIOD DAYS(20)
/* Average time a child near a parent needs to learn one thing it knows. */
#define LEARNING_PERIOD DAYS(10)
#define LIFESPAN_MINIMUM YEARS(55)
#define LIFESPAN_SPREAD YEARS(35)

/* People within this distance share a camp: a couple there may conceive and
 * a child there learns from its parents. They wander within WANDER_RADIUS
 * of the middle between themselves and the one they keep close to, so a
 * family stays in one camp. */
#define CAMP_DISTANCE SOS_METRES(50)
#define WANDER_RADIUS SOS_METRES(30)
/* Those who know foraging know where food grows around them; others eat only
 * what they come across. Without food in reach an adult sets out to look up
 * to SEARCH_DISTANCE away. A hungry being looks again every FORAGE_PERIOD and
 * gives up walking to food it saw after as long. */
#define FORAGING_RADIUS SOS_METRES(64)
#define NOTICE_RADIUS SOS_METRES(8)
#define SEARCH_DISTANCE SOS_METRES(100)
#define FORAGE_PERIOD HOURS(1)
/* 1.4 m/s for adults, 1 m/s for children, in units per tick. */
#define ADULT_SPEED (SOS_METRES(14) / 10 / SOS_TICKS_PER_SECOND)
#define CHILD_SPEED (SOS_METRES(1) / SOS_TICKS_PER_SECOND)

#define NO_PREGNANCY INT64_MIN
#define INITIAL_CAPACITY 16u

struct SosBeings
{
    uint64_t seed;
    uint64_t nextSerial;
    uint32_t capacity;
    uint32_t slotCount;
    uint32_t living;
    uint32_t freeCount;
    SosBeingsStats stats;
    uint32_t *freeSlots;
    uint8_t *alive;
    uint64_t *serial;
    uint8_t *sex;
    int64_t *birthTick;
    int64_t *lifespan;
    SosPosition *position;
    SosPosition *target;
    uint32_t *health;
    uint32_t *hunger;
    uint32_t *thirst;
    uint32_t *fatigue;
    uint64_t *knowledge;
    uint64_t *mother;
    uint64_t *father;
    int64_t *conceivedTick;
    int64_t *fertileFrom;
    uint64_t *unbornFather;
    uint8_t *activity;
    int64_t *activityEnd;
    SosRandom *random;
};

/* Every column, so growth and teardown cannot miss one. */
#define BEING_COLUMNS(X)                                                                           \
    X(freeSlots)                                                                                   \
    X(alive)                                                                                       \
    X(serial)                                                                                      \
    X(sex)                                                                                         \
    X(birthTick)                                                                                   \
    X(lifespan)                                                                                    \
    X(position)                                                                                    \
    X(target)                                                                                      \
    X(health)                                                                                      \
    X(hunger)                                                                                      \
    X(thirst)                                                                                      \
    X(fatigue)                                                                                     \
    X(knowledge)                                                                                   \
    X(mother)                                                                                      \
    X(father)                                                                                      \
    X(conceivedTick)                                                                               \
    X(fertileFrom)                                                                                 \
    X(unbornFather)                                                                                \
    X(activity)                                                                                    \
    X(activityEnd)                                                                                 \
    X(random)

static bool Grow(SosBeings *beings)
{
    if (beings->capacity > UINT32_MAX / 2u)
    {
        return false;
    }
    const uint32_t capacity = beings->capacity == 0u ? INITIAL_CAPACITY : beings->capacity * 2u;
    /* A failed column keeps the old capacity valid: the grown ones are only
     * larger than needed. */
#define GROW_COLUMN(column)                                                                        \
    {                                                                                              \
        void *grown = realloc(beings->column, (size_t)capacity * sizeof(*beings->column));         \
        if (grown == NULL)                                                                         \
        {                                                                                          \
            return false;                                                                          \
        }                                                                                          \
        beings->column = grown;                                                                    \
    }
    BEING_COLUMNS(GROW_COLUMN)
#undef GROW_COLUMN
    beings->capacity = capacity;
    return true;
}

SosBeings *SosBeingsCreate(uint64_t worldSeed)
{
    SosBeings *beings = calloc(1u, sizeof(*beings));
    if (beings == NULL)
    {
        return NULL;
    }
    beings->seed = SosHashCombine(worldSeed, UINT64_C(0x6265696e6773)); /* "beings" */
    beings->nextSerial = 1u;
    if (!Grow(beings))
    {
        SosBeingsDestroy(beings);
        return NULL;
    }
    return beings;
}

void SosBeingsDestroy(SosBeings *beings)
{
    if (beings == NULL)
    {
        return;
    }
#define FREE_COLUMN(column) free(beings->column);
    BEING_COLUMNS(FREE_COLUMN)
#undef FREE_COLUMN
    free(beings);
}

static uint32_t Raise(uint32_t value, int64_t ticks, int64_t period)
{
    if (ticks >= period)
    {
        return SOS_NEED_MAX;
    }
    const uint64_t raised =
        (uint64_t)value + (uint64_t)SOS_NEED_MAX * (uint64_t)ticks / (uint64_t)period;
    return raised >= SOS_NEED_MAX ? SOS_NEED_MAX : (uint32_t)raised;
}

static uint32_t Lower(uint32_t value, int64_t ticks, int64_t period)
{
    if (ticks >= period)
    {
        return 0u;
    }
    const uint64_t amount = (uint64_t)SOS_NEED_MAX * (uint64_t)ticks / (uint64_t)period;
    return amount >= value ? 0u : value - (uint32_t)amount;
}

/* True with probability ticks / period: an event that happens on average once
 * per period, sampled over a step of any length. */
static bool Chance(SosRandom *random, int64_t ticks, int64_t period)
{
    if (ticks >= period)
    {
        return true;
    }
    return (int64_t)(SosRandomNext(random) % (uint64_t)period) < ticks;
}

static int64_t Age(const SosBeings *beings, uint32_t slot, uint64_t tick)
{
    return (int64_t)tick - beings->birthTick[slot];
}

static uint64_t DistanceSquared(SosPosition from, SosPosition to)
{
    const int64_t dx = to.x - from.x;
    const int64_t dy = to.y - from.y;
    /* Beyond 2^31 units (two thousand kilometres) nothing in this file cares
     * about the exact value. */
    const int64_t limit = INT64_C(1) << 31;
    if (dx > limit || dx < -limit || dy > limit || dy < -limit)
    {
        return UINT64_MAX;
    }
    return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

static bool Within(SosPosition from, SosPosition to, int64_t distance)
{
    return DistanceSquared(from, to) <= (uint64_t)(distance * distance);
}

static uint32_t FindSerial(const SosBeings *beings, uint64_t serial)
{
    if (serial != 0u)
    {
        for (uint32_t slot = 0u; slot < beings->slotCount; ++slot)
        {
            if (beings->alive[slot] && beings->serial[slot] == serial)
            {
                return slot;
            }
        }
    }
    return UINT32_MAX;
}

/* A parent and its child never form a couple. */
static bool ParentAndChild(const SosBeings *beings, uint32_t first, uint32_t second)
{
    const uint64_t a = beings->serial[first];
    const uint64_t b = beings->serial[second];
    return beings->mother[first] == b || beings->father[first] == b ||
           beings->mother[second] == a || beings->father[second] == a;
}

static uint32_t Parent(const SosBeings *beings, uint32_t slot)
{
    const uint32_t mother = FindSerial(beings, beings->mother[slot]);
    return mother != UINT32_MAX ? mother : FindSerial(beings, beings->father[slot]);
}

/* Whom a being stays close to: a young child its mother or father, an adult
 * the nearest possible partner, and an adult without one its family. */
static uint32_t Companion(const SosBeings *beings, uint32_t slot, uint64_t tick)
{
    if (Age(beings, slot, tick) < CHILDHOOD_END)
    {
        return Parent(beings, slot);
    }
    uint32_t nearest = UINT32_MAX;
    uint64_t nearestDistance = UINT64_MAX;
    for (uint32_t other = 0u; other < beings->slotCount; ++other)
    {
        if (other == slot || !beings->alive[other] || beings->sex[other] == beings->sex[slot] ||
            Age(beings, other, tick) < ADULTHOOD || ParentAndChild(beings, slot, other))
        {
            continue;
        }
        const uint64_t distance = DistanceSquared(beings->position[slot], beings->position[other]);
        if (distance < nearestDistance)
        {
            nearest = other;
            nearestDistance = distance;
        }
    }
    return nearest != UINT32_MAX ? nearest : Parent(beings, slot);
}

static void SetActivity(SosBeings *beings, uint32_t slot, SosActivity activity, int64_t end)
{
    beings->activity[slot] = (uint8_t)activity;
    beings->activityEnd[slot] = end;
}

/* Sets out for the nearest food in reach. Without any, a child goes back to
 * its family and an adult heads off in a random direction to look. */
static void SeekFood(SosBeings *beings, const SosEnvironment *environment, uint32_t slot,
                     uint64_t tick)
{
    const int64_t now = (int64_t)tick;
    const SosPosition here = beings->position[slot];
    const int64_t radius =
        (beings->knowledge[slot] & SOS_KNOWLEDGE_FORAGING) != 0u ? FORAGING_RADIUS : NOTICE_RADIUS;
    SosPlant plant;
    if (SosPlantsFindRipe(environment->plants, environment->terrain, here, radius, tick, &plant))
    {
        beings->target[slot] = plant.position;
        SetActivity(beings, slot, SOS_ACTIVITY_FORAGE, now + FORAGE_PERIOD);
        return;
    }
    const uint32_t parent =
        Age(beings, slot, tick) < CHILDHOOD_END ? Parent(beings, slot) : UINT32_MAX;
    if (parent != UINT32_MAX)
    {
        beings->target[slot] = beings->position[parent];
    }
    else
    {
        SosRandom *random = &beings->random[slot];
        beings->target[slot] = (SosPosition){
            .x = here.x + SosRandomRange(random, -SEARCH_DISTANCE, SEARCH_DISTANCE),
            .y = here.y + SosRandomRange(random, -SEARCH_DISTANCE, SEARCH_DISTANCE),
            .z = here.z,
        };
    }
    /* Hungry, it looks again sooner than it would wander. */
    SetActivity(beings, slot, SOS_ACTIVITY_WANDER, now + FORAGE_PERIOD);
}

static void Decide(SosBeings *beings, const SosEnvironment *environment, uint32_t slot,
                   uint64_t tick)
{
    const int64_t now = (int64_t)tick;
    if (beings->thirst[slot] > NEED_FRACTION(3, 5))
    {
        SetActivity(beings, slot, SOS_ACTIVITY_DRINK, now + DRINK_PERIOD);
    }
    else if (beings->hunger[slot] > NEED_FRACTION(3, 5))
    {
        SeekFood(beings, environment, slot, tick);
    }
    else if (beings->fatigue[slot] > NEED_FRACTION(9, 10) ||
             (SosCalendarIsNight(tick) && beings->fatigue[slot] > NEED_FRACTION(3, 10)))
    {
        SetActivity(beings, slot, SOS_ACTIVITY_SLEEP, now + SLEEP_PERIOD);
    }
    else
    {
        /* Wander around the middle between oneself and the companion: two
         * people deciding at once each head for the other's old place and
         * would only swap, while the middle brings them together. */
        const uint32_t companion = Companion(beings, slot, tick);
        const SosPosition here = beings->position[slot];
        const SosPosition there = companion != UINT32_MAX ? beings->position[companion] : here;
        const SosPosition anchor = {
            here.x + (there.x - here.x) / 2,
            here.y + (there.y - here.y) / 2,
            here.z,
        };
        SosRandom *random = &beings->random[slot];
        beings->target[slot] = (SosPosition){
            .x = anchor.x + SosRandomRange(random, -WANDER_RADIUS, WANDER_RADIUS),
            .y = anchor.y + SosRandomRange(random, -WANDER_RADIUS, WANDER_RADIUS),
            .z = anchor.z,
        };
        SetActivity(beings, slot, SOS_ACTIVITY_WANDER, now + WANDER_PERIOD);
    }
}

/* Walks toward the target; true on arrival. */
static bool Walk(SosBeings *beings, uint32_t slot, int64_t speed, int64_t ticks)
{
    SosPosition *position = &beings->position[slot];
    const SosPosition target = beings->target[slot];
    int64_t dx = target.x - position->x;
    int64_t dy = target.y - position->y;
    const uint64_t squared = DistanceSquared(*position, target);
    const int64_t reach = ticks > INT64_MAX / speed ? INT64_MAX : speed * ticks;
    if (squared == UINT64_MAX)
    {
        /* Too far for exact geometry: head along the longer axis, never past
         * the target. */
        const int64_t magnitudeX = dx < 0 ? -dx : dx;
        const int64_t magnitudeY = dy < 0 ? -dy : dy;
        int64_t *axis = magnitudeX >= magnitudeY ? &position->x : &position->y;
        const int64_t delta = magnitudeX >= magnitudeY ? dx : dy;
        const int64_t magnitude = delta < 0 ? -delta : delta;
        const int64_t move = reach < magnitude ? reach : magnitude;
        *axis += delta < 0 ? -move : move;
        return false;
    }
    const int64_t distance = (int64_t)SosIntegerSqrt(squared);
    if (reach >= distance)
    {
        position->x = target.x;
        position->y = target.y;
        return true;
    }
    /* |dx| <= 2^31 and reach < distance <= 2^32: the products fit. */
    position->x += dx * reach / distance;
    position->y += dy * reach / distance;
    return false;
}

static void Die(SosBeings *beings, uint32_t slot)
{
    beings->alive[slot] = 0u;
    beings->living -= 1u;
    beings->stats.deaths += 1u;
    beings->freeSlots[beings->freeCount++] = slot;
}

/* Walks toward the target over the ground; true on arrival. */
static bool Travel(SosBeings *beings, const SosTerrain *terrain, uint32_t slot, uint64_t tick,
                   int64_t ticks)
{
    const int64_t speed = Age(beings, slot, tick) < ADULTHOOD ? CHILD_SPEED : ADULT_SPEED;
    const bool arrived = Walk(beings, slot, speed, ticks);
    SosPosition *position = &beings->position[slot];
    position->z = SosTerrainSurfaceZ(terrain, position->x, position->y);
    return arrived;
}

static void Live(SosBeings *beings, const SosEnvironment *environment, uint32_t slot, uint64_t tick,
                 int64_t ticks)
{
    const int64_t now = (int64_t)tick;
    if (Age(beings, slot, tick) >= beings->lifespan[slot])
    {
        Die(beings, slot);
        return;
    }

    const SosActivity activity = (SosActivity)beings->activity[slot];
    beings->hunger[slot] = Raise(beings->hunger[slot], ticks, HUNGER_PERIOD);
    beings->thirst[slot] = Raise(beings->thirst[slot], ticks, THIRST_PERIOD);
    if (activity == SOS_ACTIVITY_EAT)
    {
        beings->hunger[slot] = Lower(beings->hunger[slot], ticks, EAT_PERIOD);
    }
    else if (activity == SOS_ACTIVITY_DRINK)
    {
        beings->thirst[slot] = Lower(beings->thirst[slot], ticks, DRINK_PERIOD);
    }
    beings->fatigue[slot] = activity == SOS_ACTIVITY_SLEEP
                                ? Lower(beings->fatigue[slot], ticks, SLEEP_PERIOD)
                                : Raise(beings->fatigue[slot], ticks, AWAKE_PERIOD);

    if (beings->hunger[slot] >= SOS_NEED_MAX || beings->thirst[slot] >= SOS_NEED_MAX)
    {
        beings->health[slot] = Lower(beings->health[slot], ticks, STARVATION_PERIOD);
        if (beings->health[slot] == 0u)
        {
            Die(beings, slot);
            return;
        }
    }
    else
    {
        beings->health[slot] = Raise(beings->health[slot], ticks, HEALING_PERIOD);
    }

    bool finished = now >= beings->activityEnd[slot];
    if (activity == SOS_ACTIVITY_WANDER)
    {
        /* Walks to the spot and stays there until the wander ends. */
        (void)Travel(beings, environment->terrain, slot, tick, ticks);
    }
    else if (activity == SOS_ACTIVITY_FORAGE)
    {
        if (Travel(beings, environment->terrain, slot, tick, ticks))
        {
            /* Someone may have eaten the food first; then look again. */
            const SosPosition at = beings->position[slot];
            if (SosPlantsHarvest(environment->plants, environment->terrain,
                                 SosFloorShift(at.x, SOS_PLANT_CELL_SHIFT),
                                 SosFloorShift(at.y, SOS_PLANT_CELL_SHIFT), tick))
            {
                SetActivity(beings, slot, SOS_ACTIVITY_EAT, now + EAT_PERIOD);
                return;
            }
            finished = true;
        }
    }
    else if (activity == SOS_ACTIVITY_EAT)
    {
        finished = finished || beings->hunger[slot] == 0u;
    }
    else if (activity == SOS_ACTIVITY_DRINK)
    {
        finished = finished || beings->thirst[slot] == 0u;
    }
    else if (activity == SOS_ACTIVITY_SLEEP)
    {
        finished = finished || (beings->fatigue[slot] == 0u && !SosCalendarIsNight(tick));
    }
    if (activity == SOS_ACTIVITY_IDLE || finished)
    {
        Decide(beings, environment, slot, tick);
    }
}

static uint64_t PickBit(SosRandom *random, uint64_t set)
{
    uint32_t count = 0u;
    for (uint64_t rest = set; rest != 0u; rest &= rest - 1u)
    {
        ++count;
    }
    uint32_t pick = SosRandomBelow(random, count);
    for (uint64_t rest = set; rest != 0u; rest &= rest - 1u)
    {
        if (pick-- == 0u)
        {
            return rest & (~rest + 1u);
        }
    }
    return 0u;
}

static void Learn(SosBeings *beings, uint32_t child, uint32_t parent, int64_t ticks)
{
    if (parent == UINT32_MAX ||
        !Within(beings->position[child], beings->position[parent], CAMP_DISTANCE))
    {
        return;
    }
    const uint64_t unknown = beings->knowledge[parent] & ~beings->knowledge[child];
    if (unknown != 0u && Chance(&beings->random[child], ticks, LEARNING_PERIOD))
    {
        beings->knowledge[child] |= PickBit(&beings->random[child], unknown);
    }
}

static bool Fertile(const SosBeings *beings, uint32_t slot, uint64_t tick)
{
    const int64_t age = Age(beings, slot, tick);
    const int64_t end =
        beings->sex[slot] == SOS_SEX_FEMALE ? FEMALE_FERTILITY_END : MALE_FERTILITY_END;
    return beings->alive[slot] && age >= ADULTHOOD && age < end &&
           (int64_t)tick >= beings->fertileFrom[slot] && beings->health[slot] > SOS_NEED_MAX / 2u;
}

static void GiveBirth(SosBeings *beings, const SosTerrain *terrain, uint32_t mother, uint64_t tick)
{
    const SosBeingSpawn child = {
        .sex = SosRandomBelow(&beings->random[mother], 2u) == 0u ? SOS_SEX_FEMALE : SOS_SEX_MALE,
        .birthTick = (int64_t)tick,
        .position = beings->position[mother],
        .mother = beings->serial[mother],
        .father = beings->unbornFather[mother],
    };
    beings->conceivedTick[mother] = NO_PREGNANCY;
    beings->fertileFrom[mother] = (int64_t)tick + BIRTH_INTERVAL;
    beings->unbornFather[mother] = 0u;
    if (SosBeingsSpawn(beings, terrain, tick, &child) != 0u)
    {
        beings->stats.births += 1u;
    }
}

static void Socialize(SosBeings *beings, const SosTerrain *terrain, uint32_t slot, uint64_t tick,
                      int64_t ticks)
{
    const int64_t age = Age(beings, slot, tick);
    if (age < ADULTHOOD)
    {
        Learn(beings, slot, FindSerial(beings, beings->mother[slot]), ticks);
        Learn(beings, slot, FindSerial(beings, beings->father[slot]), ticks);
    }
    if (beings->sex[slot] != SOS_SEX_FEMALE)
    {
        return;
    }
    if (beings->conceivedTick[slot] != NO_PREGNANCY)
    {
        if ((int64_t)tick - beings->conceivedTick[slot] >= PREGNANCY)
        {
            GiveBirth(beings, terrain, slot, tick);
        }
        return;
    }
    if (!Fertile(beings, slot, tick))
    {
        return;
    }
    for (uint32_t other = 0u; other < beings->slotCount; ++other)
    {
        if (beings->sex[other] == SOS_SEX_MALE && Fertile(beings, other, tick) &&
            !ParentAndChild(beings, slot, other) &&
            Within(beings->position[slot], beings->position[other], CAMP_DISTANCE))
        {
            if (Chance(&beings->random[slot], ticks, CONCEPTION_PERIOD))
            {
                beings->conceivedTick[slot] = (int64_t)tick;
                beings->unbornFather[slot] = beings->serial[other];
            }
            return;
        }
    }
}

uint64_t SosBeingsSpawn(SosBeings *beings, const SosTerrain *terrain, uint64_t tick,
                        const SosBeingSpawn *spawn)
{
    if (beings == NULL || terrain == NULL || spawn == NULL ||
        (spawn->sex != SOS_SEX_FEMALE && spawn->sex != SOS_SEX_MALE) ||
        spawn->birthTick > (int64_t)tick)
    {
        return 0u;
    }
    uint32_t slot = 0u;
    if (beings->freeCount != 0u)
    {
        slot = beings->freeSlots[--beings->freeCount];
    }
    else
    {
        if (beings->slotCount == beings->capacity && !Grow(beings))
        {
            return 0u;
        }
        slot = beings->slotCount++;
    }

    const uint64_t serial = beings->nextSerial++;
    SosRandom random = SosRandomStream(beings->seed, serial);
    SosPosition position = spawn->position;
    position.z = SosTerrainSurfaceZ(terrain, position.x, position.y);
    beings->alive[slot] = 1u;
    beings->serial[slot] = serial;
    beings->sex[slot] = (uint8_t)spawn->sex;
    beings->birthTick[slot] = spawn->birthTick;
    beings->lifespan[slot] = LIFESPAN_MINIMUM + SosRandomRange(&random, 0, LIFESPAN_SPREAD);
    beings->position[slot] = position;
    beings->target[slot] = position;
    beings->health[slot] = SOS_NEED_MAX;
    beings->hunger[slot] = 0u;
    beings->thirst[slot] = 0u;
    beings->fatigue[slot] = 0u;
    beings->knowledge[slot] = spawn->knowledge;
    beings->mother[slot] = spawn->mother;
    beings->father[slot] = spawn->father;
    beings->conceivedTick[slot] = NO_PREGNANCY;
    beings->fertileFrom[slot] = INT64_MIN;
    beings->unbornFather[slot] = 0u;
    beings->activity[slot] = SOS_ACTIVITY_IDLE;
    beings->activityEnd[slot] = (int64_t)tick;
    beings->random[slot] = random;
    beings->living += 1u;
    return serial;
}

void SosBeingsStep(SosBeings *beings, const SosEnvironment *environment, uint64_t tick,
                   uint64_t ticks)
{
    if (beings == NULL || environment == NULL || environment->terrain == NULL ||
        environment->plants == NULL || ticks == 0u)
    {
        return;
    }
    /* Larger steps change nothing: every need saturates within days. */
    const int64_t step = ticks > (uint64_t)YEARS(1) ? YEARS(1) : (int64_t)ticks;
    const uint64_t now = tick + ticks;
    /* Beings born during this step start living in the next one. */
    const uint32_t slots = beings->slotCount;
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        if (beings->alive[slot])
        {
            Live(beings, environment, slot, now, step);
        }
    }
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        /* A newborn may take a slot freed earlier in this step; it waits too. */
        if (beings->alive[slot] && beings->birthTick[slot] < (int64_t)now)
        {
            Socialize(beings, environment->terrain, slot, now, step);
        }
    }
}

uint32_t SosBeingsLivingCount(const SosBeings *beings)
{
    return beings == NULL ? 0u : beings->living;
}

uint32_t SosBeingsSlotCount(const SosBeings *beings)
{
    return beings == NULL ? 0u : beings->slotCount;
}

bool SosBeingsView(const SosBeings *beings, uint32_t slot, SosBeingView *outView)
{
    if (beings == NULL || outView == NULL || slot >= beings->slotCount || !beings->alive[slot])
    {
        return false;
    }
    *outView = (SosBeingView){
        .serial = beings->serial[slot],
        .sex = (SosSex)beings->sex[slot],
        .birthTick = beings->birthTick[slot],
        .position = beings->position[slot],
        .health = beings->health[slot],
        .hunger = beings->hunger[slot],
        .thirst = beings->thirst[slot],
        .fatigue = beings->fatigue[slot],
        .knowledge = beings->knowledge[slot],
        .mother = beings->mother[slot],
        .father = beings->father[slot],
        .pregnant = beings->conceivedTick[slot] != NO_PREGNANCY,
        .activity = (SosActivity)beings->activity[slot],
    };
    return true;
}

SosBeingsStats SosBeingsGetStats(const SosBeings *beings)
{
    return beings == NULL ? (SosBeingsStats){0} : beings->stats;
}

uint64_t SosBeingsHash(const SosBeings *beings)
{
    if (beings == NULL)
    {
        return 0u;
    }
    uint64_t hash = SosHashCombine(beings->seed, beings->nextSerial);
    hash = SosHashCombine(hash, ((uint64_t)beings->slotCount << 32) | beings->living);
    for (uint32_t slot = 0u; slot < beings->slotCount; ++slot)
    {
        if (!beings->alive[slot])
        {
            continue;
        }
        const uint64_t fields[] = {
            slot,
            beings->serial[slot],
            beings->sex[slot],
            (uint64_t)beings->birthTick[slot],
            (uint64_t)beings->lifespan[slot],
            (uint64_t)beings->position[slot].x,
            (uint64_t)beings->position[slot].y,
            (uint64_t)beings->position[slot].z,
            (uint64_t)beings->target[slot].x,
            (uint64_t)beings->target[slot].y,
            ((uint64_t)beings->health[slot] << 32) | beings->hunger[slot],
            ((uint64_t)beings->thirst[slot] << 32) | beings->fatigue[slot],
            beings->knowledge[slot],
            beings->mother[slot],
            beings->father[slot],
            (uint64_t)beings->conceivedTick[slot],
            (uint64_t)beings->fertileFrom[slot],
            beings->unbornFather[slot],
            beings->activity[slot],
            (uint64_t)beings->activityEnd[slot],
            beings->random[slot].state,
        };
        for (size_t index = 0u; index < sizeof(fields) / sizeof(fields[0]); ++index)
        {
            hash = SosHashCombine(hash, fields[index]);
        }
    }
    return hash;
}
