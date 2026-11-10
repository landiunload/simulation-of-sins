#include "sim/tech.h"

#include "sim/calendar.h"

#include <stddef.h>

/* An easy discovery under full attention reaches certainty in about this
 * long; a harder one takes proportionally longer. */
#define BASE_INSIGHT_TICKS (30 * SOS_TICKS_PER_DAY)

/* The discoveries of the world, indexed by the bit they occupy. Bits 0 and 1
 * are the founders' knowledge and the roots of the tree. Every prerequisite is
 * an earlier entry, so the catalog is acyclic and a being that only learns
 * what it is ready for builds a valid mask. Difficulty is a Q16 factor: one
 * for the easiest, larger for a longer reach. */
static const struct SosTechInfo catalog[SOS_TECH_COUNT] = {
    [0] = {.name = "speech",
           .prerequisites = 0u,
           .difficulty = SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [1] = {.name = "foraging",
           .prerequisites = 0u,
           .difficulty = SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [2] = {.name = "fire",
           .prerequisites = 0u,
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [3] = {.name = "flint knapping",
           .prerequisites = 0u,
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [4] = {.name = "spear",
           .prerequisites = (UINT64_C(1) << 3),
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [5] = {.name = "hunting",
           .prerequisites = (UINT64_C(1) << 4) | (UINT64_C(1) << 1),
           .difficulty = 3 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [6] = {.name = "cooking",
           .prerequisites = (UINT64_C(1) << 2),
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [7] = {.name = "clothing",
           .prerequisites = (UINT64_C(1) << 5),
           .difficulty = 3 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [8] = {.name = "shelter",
           .prerequisites = (UINT64_C(1) << 2),
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [9] = {.name = "cordage",
           .prerequisites = (UINT64_C(1) << 1),
           .difficulty = 2 * SOS_Q16_ONE,
           .era = SOS_TECH_ERA_STONE},
    [10] = {.name = "basketry",
            .prerequisites = (UINT64_C(1) << 9),
            .difficulty = 2 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [11] = {.name = "burial",
            .prerequisites = (UINT64_C(1) << 0) | (UINT64_C(1) << 2),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [12] = {.name = "art",
            .prerequisites = (UINT64_C(1) << 0) | (UINT64_C(1) << 3),
            .difficulty = 2 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [13] = {.name = "music",
            .prerequisites = (UINT64_C(1) << 0),
            .difficulty = 2 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [14] = {.name = "counting",
            .prerequisites = (UINT64_C(1) << 0),
            .difficulty = 2 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [15] = {.name = "herbal medicine",
            .prerequisites = (UINT64_C(1) << 1),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [16] = {.name = "fishing",
            .prerequisites = (UINT64_C(1) << 9) | (UINT64_C(1) << 5),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [17] = {.name = "trapping",
            .prerequisites = (UINT64_C(1) << 5) | (UINT64_C(1) << 9),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [18] = {.name = "boats",
            .prerequisites = (UINT64_C(1) << 2) | (UINT64_C(1) << 9),
            .difficulty = 4 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [19] = {.name = "archery",
            .prerequisites = (UINT64_C(1) << 4) | (UINT64_C(1) << 9),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_STONE},
    [20] = {.name = "pottery",
            .prerequisites = (UINT64_C(1) << 2) | (UINT64_C(1) << 10),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [21] = {.name = "domestication",
            .prerequisites = (UINT64_C(1) << 5) | (UINT64_C(1) << 17),
            .difficulty = 4 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [22] = {.name = "farming",
            .prerequisites = (UINT64_C(1) << 21) | (UINT64_C(1) << 15),
            .difficulty = 4 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [23] = {.name = "weaving",
            .prerequisites = (UINT64_C(1) << 9) | (UINT64_C(1) << 7),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [24] = {.name = "masonry",
            .prerequisites = (UINT64_C(1) << 8) | (UINT64_C(1) << 3),
            .difficulty = 3 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [25] = {.name = "irrigation",
            .prerequisites = (UINT64_C(1) << 22) | (UINT64_C(1) << 20),
            .difficulty = 4 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_NEOLITHIC},
    [26] = {.name = "the wheel",
            .prerequisites = (UINT64_C(1) << 14) | (UINT64_C(1) << 22),
            .difficulty = 5 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_BRONZE},
    [27] = {.name = "copper working",
            .prerequisites = (UINT64_C(1) << 2) | (UINT64_C(1) << 20),
            .difficulty = 5 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_BRONZE},
    [28] = {.name = "writing",
            .prerequisites = (UINT64_C(1) << 14) | (UINT64_C(1) << 12),
            .difficulty = 5 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_BRONZE},
    [29] = {.name = "sailing",
            .prerequisites = (UINT64_C(1) << 18) | (UINT64_C(1) << 23),
            .difficulty = 5 * SOS_Q16_ONE,
            .era = SOS_TECH_ERA_BRONZE},
};

/* The single-bit mask of `id`; the caller has checked it is below the count. */
static uint64_t Bit(uint32_t id)
{
    return UINT64_C(1) << id;
}

/* Whether `knowledge` holds every prerequisite of `id`; `id` is in range. */
static bool HasPrerequisites(uint64_t knowledge, uint32_t id)
{
    return (knowledge & catalog[id].prerequisites) == catalog[id].prerequisites;
}

/* One bit of `set`, uniform over its bits, so teaching is deterministic given
 * the stream. `set` must not be zero. */
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
        if (pick == 0u)
        {
            return rest & (~rest + 1u);
        }
        --pick;
    }
    return 0u;
}

bool SosTechInfo(uint32_t id, struct SosTechInfo *out)
{
    if (out == NULL || id >= SOS_TECH_COUNT)
    {
        return false;
    }
    *out = catalog[id];
    return true;
}

bool SosTechCanDiscover(uint64_t knowledge, uint32_t id)
{
    return id < SOS_TECH_COUNT && (knowledge & Bit(id)) == 0u && HasPrerequisites(knowledge, id);
}

uint64_t SosTechAvailable(uint64_t knowledge)
{
    uint64_t available = 0u;
    for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
    {
        if ((knowledge & Bit(id)) == 0u && HasPrerequisites(knowledge, id))
        {
            available |= Bit(id);
        }
    }
    return available;
}

SosQ16 SosTechDiscoveryChance(uint64_t knowledge, uint32_t id, SosQ16 intelligence,
                              SosQ16 curiosity, uint64_t exposureTicks)
{
    if (id >= SOS_TECH_COUNT || (knowledge & Bit(id)) != 0u || !HasPrerequisites(knowledge, id) ||
        exposureTicks == 0u || intelligence <= 0 || curiosity <= 0)
    {
        return 0;
    }
    const SosQ16 focused = SosQ16Clamp(intelligence, 0, SOS_Q16_ONE);
    const SosQ16 eager = SosQ16Clamp(curiosity, 0, SOS_Q16_ONE);
    const SosQ16 aptitude = SosQ16Mul(focused, eager);
    if (aptitude <= 0)
    {
        return 0;
    }
    /* Exposure ticks at which this discovery would certainly be reached. */
    const uint64_t span =
        (uint64_t)catalog[id].difficulty * (uint64_t)BASE_INSIGHT_TICKS / SOS_Q16_ONE;
    if (span == 0u || exposureTicks >= span)
    {
        return SOS_Q16_ONE;
    }
    /* Both factors stay below 2^47, so this cannot overflow. */
    const uint64_t reached = (uint64_t)aptitude * exposureTicks / span;
    return reached >= (uint64_t)SOS_Q16_ONE ? SOS_Q16_ONE : (SosQ16)reached;
}

bool SosTechTryDiscover(SosRandom *random, uint64_t knowledge, uint32_t id, SosQ16 intelligence,
                        SosQ16 curiosity, uint64_t exposureTicks)
{
    if (random == NULL)
    {
        return false;
    }
    const SosQ16 chance =
        SosTechDiscoveryChance(knowledge, id, intelligence, curiosity, exposureTicks);
    return SosRandomChance(random, (uint32_t)chance, (uint32_t)SOS_Q16_ONE) != 0;
}

uint64_t SosTechTeachable(uint64_t teacher, uint64_t learner)
{
    return SosTechAvailable(learner) & teacher;
}

uint64_t SosTechTeach(uint64_t teacher, uint64_t learner, SosRandom *random, SosQ16 aptitude)
{
    if (random == NULL)
    {
        return learner;
    }
    const uint64_t teachable = SosTechTeachable(teacher, learner);
    if (teachable == 0u)
    {
        return learner;
    }
    const SosQ16 willing = SosQ16Clamp(aptitude, 0, SOS_Q16_ONE);
    if (SosRandomChance(random, (uint32_t)willing, (uint32_t)SOS_Q16_ONE) == 0)
    {
        return learner;
    }
    return learner | PickBit(random, teachable);
}

SosTechEra SosTechEraOf(uint64_t knowledge)
{
    SosTechEra era = SOS_TECH_ERA_STONE;
    for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
    {
        if ((knowledge & Bit(id)) != 0u && catalog[id].era > era)
        {
            era = catalog[id].era;
        }
    }
    return era;
}
