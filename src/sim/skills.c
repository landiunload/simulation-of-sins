#include "sim/skills.h"

#include "sim/calendar.h"
#include "sim/links.h"
#include "sim/random.h"
#include "sim/space.h"
#include "sim/table.h"

#include <stdlib.h>

/* Experience above this floor is never forgotten: a being keeps the basics of
 * every skill it once learnt. */
#define RUST_FLOOR INT64_C(64)
/* Skills fade over years, not days, so a lifetime of learning lasts. */
#define RUST_HALF_LIFE ((uint64_t)SOS_TICKS_PER_YEAR)
/* Effort beyond this many ticks is more than any being can bring to one skill,
 * and clamping it keeps the gain arithmetic in range. About a thousand years
 * of unbroken practice. */
#define MAX_EFFORT (UINT64_C(1) << 40)
/* Experience a lifetime cannot reach; saturating there loses no level. */
#define MAX_EXPERIENCE (INT64_C(1) << 56)
/* A lesson teaches this many times as much as the same time alone. */
#define TEACH_FACTOR INT64_C(4)

/* One skill of one being. A being's skills are an entry in the shared
 * SosLinks set, which supplies the count and the listing. */
typedef struct SkillRecord
{
    int64_t experience;
    /* The tick `experience` was last settled at; rust counts from here. */
    int64_t tick;
} SkillRecord;

struct SosSkills
{
    uint64_t seed;
    /* Keyed (being, skill), both non-zero. */
    SosTable *table;
    /* (being, skill) pairs: a being's skills in the order learnt. */
    SosLinks *links;
};

static const SkillRecord *ReadRecord(const SosSkills *skills, uint64_t being, uint32_t skill)
{
    return (const SkillRecord *)SosTableFind(skills->table, (SosTableKey){being, skill});
}

static SkillRecord *EditRecord(SosSkills *skills, uint64_t being, uint32_t skill)
{
    return (SkillRecord *)SosTableEdit(skills->table, (SosTableKey){being, skill});
}

SosSkills *SosSkillsCreate(uint64_t seed)
{
    SosSkills *skills = calloc(1u, sizeof(*skills));
    if (skills == NULL)
    {
        return NULL;
    }
    skills->table = SosTableCreate(sizeof(SkillRecord));
    skills->links = SosLinksCreate();
    if (skills->table == NULL || skills->links == NULL)
    {
        SosSkillsDestroy(skills);
        return NULL;
    }
    skills->seed = seed;
    return skills;
}

void SosSkillsDestroy(SosSkills *skills)
{
    if (skills != NULL)
    {
        SosTableDestroy(skills->table);
        SosLinksDestroy(skills->links);
        free(skills);
    }
}

SosQ16 SosSkillsLevelCurve(int64_t experience)
{
    if (experience <= 0)
    {
        return 0;
    }
    const uint64_t root = SosIntegerSqrt((uint64_t)experience);
    /* root / (root + HALF) as one minus HALF / (root + HALF). */
    const SosQ16 decayed = SosQ16Ratio(SOS_SKILL_LEVEL_HALF, (int64_t)root + SOS_SKILL_LEVEL_HALF);
    return (SosQ16)(SOS_Q16_ONE - decayed);
}

SosQ16 SosSkillsTalent(uint64_t seed, uint64_t being, uint32_t skill)
{
    if (being == 0u || skill == 0u)
    {
        return 0;
    }
    const uint64_t hash =
        SosHashCombine(SosHashCombine(SosHash64(seed), SosHash64(being)), (uint64_t)skill);
    return (SosQ16)(hash % (uint64_t)SOS_SKILL_TALENT_MAX);
}

/* Experience `record` still holds at `tick`, rusting whatever lies above the
 * floor and leaving the floor itself alone. */
static int64_t EffectiveExperience(const SkillRecord *record, uint64_t tick)
{
    if ((int64_t)tick <= record->tick || record->experience <= RUST_FLOOR)
    {
        return record->experience;
    }
    const uint64_t elapsed = (uint64_t)((int64_t)tick - record->tick);
    return RUST_FLOOR + SosDecay(record->experience - RUST_FLOOR, elapsed, RUST_HALF_LIFE);
}

static SosQ16 LevelAt(const SosSkills *skills, uint64_t being, uint32_t skill, uint64_t tick)
{
    const SkillRecord *record = ReadRecord(skills, being, skill);
    const SosQ16 talent = SosSkillsTalent(skills->seed, being, skill);
    if (record == NULL)
    {
        return talent;
    }
    return SosQ16Clamp(SosSkillsLevelCurve(EffectiveExperience(record, tick)) + talent, 0,
                       SOS_Q16_ONE);
}

/* The experience an effort of `effortTicks` yields at `aptitude`, saturating
 * at the point no further level is won. */
static int64_t Gain(uint64_t effortTicks, SosQ16 aptitude)
{
    if (effortTicks > MAX_EFFORT)
    {
        effortTicks = MAX_EFFORT;
    }
    const uint64_t scaled = effortTicks * (uint64_t)aptitude / (uint64_t)SOS_Q16_ONE;
    return scaled > (uint64_t)MAX_EXPERIENCE ? MAX_EXPERIENCE : (int64_t)scaled;
}

static int64_t SaturatingAdd(int64_t value, int64_t gain)
{
    if (gain <= 0)
    {
        return value;
    }
    return value > MAX_EXPERIENCE - gain ? MAX_EXPERIENCE : value + gain;
}

/* The greatest experience whose curve stays at or below `level`, used to hold
 * a learner under its teacher. */
static int64_t ExperienceForLevel(SosQ16 level)
{
    if (level <= 0)
    {
        return 0;
    }
    if (level >= SOS_Q16_ONE)
    {
        return MAX_EXPERIENCE;
    }
    const int64_t root = (int64_t)level * SOS_SKILL_LEVEL_HALF / (SOS_Q16_ONE - level);
    if (root >= (int64_t)SosIntegerSqrt((uint64_t)MAX_EXPERIENCE))
    {
        return MAX_EXPERIENCE;
    }
    return root * root;
}

/* The skill record of `being`, added to the shared links when missing. False
 * when memory ran out, leaving the object unchanged. */
static bool EnsureSkill(SosSkills *skills, uint64_t being, uint32_t skill, SkillRecord **outRecord)
{
    SkillRecord *record = EditRecord(skills, being, skill);
    if (record != NULL)
    {
        *outRecord = record;
        return true;
    }
    bool created = false;
    record = (SkillRecord *)SosTableInsert(skills->table, (SosTableKey){being, skill}, &created);
    if (record == NULL)
    {
        return false;
    }
    if (created && !SosLinksAdd(skills->links, being, skill))
    {
        (void)SosTableRemove(skills->table, (SosTableKey){being, skill});
        return false;
    }
    *outRecord = record;
    return true;
}

bool SosSkillsPractice(SosSkills *skills, uint64_t being, uint32_t skill, uint64_t tick,
                       uint64_t effortTicks, SosQ16 aptitude)
{
    if (skills == NULL || being == 0u || skill == 0u || effortTicks == 0u)
    {
        return false;
    }
    SkillRecord *record = NULL;
    if (!EnsureSkill(skills, being, skill, &record))
    {
        return false;
    }
    const int64_t gain = Gain(effortTicks, SosQ16Clamp(aptitude, 0, SOS_Q16_ONE));
    record->experience = SaturatingAdd(EffectiveExperience(record, tick), gain);
    record->tick = (int64_t)tick;
    return true;
}

SosQ16 SosSkillsLevel(const SosSkills *skills, uint64_t being, uint32_t skill, uint64_t tick)
{
    if (skills == NULL || being == 0u || skill == 0u)
    {
        return 0;
    }
    return LevelAt(skills, being, skill, tick);
}

bool SosSkillsTeach(SosSkills *skills, uint64_t teacher, uint64_t learner, uint32_t skill,
                    uint64_t tick, uint64_t durationTicks, SosQ16 learnerAptitude)
{
    if (skills == NULL || teacher == 0u || learner == 0u || skill == 0u || durationTicks == 0u ||
        teacher == learner)
    {
        return false;
    }
    const SosQ16 talent = SosSkillsTalent(skills->seed, learner, skill);
    const int64_t room = (int64_t)LevelAt(skills, teacher, skill, tick) - talent;
    if (room <= 0)
    {
        return true;
    }
    const int64_t cap = ExperienceForLevel((SosQ16)room);
    const SkillRecord *existing = ReadRecord(skills, learner, skill);
    const int64_t base = existing != NULL ? EffectiveExperience(existing, tick) : 0;
    if (base >= cap)
    {
        return true;
    }
    int64_t gain = Gain(durationTicks, SosQ16Clamp(learnerAptitude, 0, SOS_Q16_ONE));
    if (gain > MAX_EXPERIENCE / TEACH_FACTOR)
    {
        gain = MAX_EXPERIENCE;
    }
    else
    {
        gain *= TEACH_FACTOR;
    }
    int64_t target = SaturatingAdd(base, gain);
    if (target > cap)
    {
        target = cap;
    }
    SkillRecord *record = NULL;
    if (!EnsureSkill(skills, learner, skill, &record))
    {
        return false;
    }
    record->experience = target;
    record->tick = (int64_t)tick;
    return true;
}

uint32_t SosSkillsOf(const SosSkills *skills, uint64_t being, uint64_t tick, SosSkillEntry *out,
                     uint32_t capacity)
{
    if (skills == NULL || being == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t found = 0u;
    for (uint32_t skill = (uint32_t)SosLinksFirst(skills->links, being); skill != 0u;
         skill = (uint32_t)SosLinksNext(skills->links, being, skill))
    {
        const SkillRecord *record = ReadRecord(skills, being, skill);
        if (record == NULL)
        {
            continue;
        }
        if (found < capacity)
        {
            out[found] = (SosSkillEntry){
                .skill = skill,
                .level = LevelAt(skills, being, skill, tick),
                .experience = EffectiveExperience(record, tick),
            };
        }
        ++found;
    }
    return found;
}

void SosSkillsForget(SosSkills *skills, uint64_t being)
{
    if (skills == NULL || being == 0u)
    {
        return;
    }
    /* The being's links name exactly the skills to drop, so forgetting costs
     * O(degree) rather than a sweep of the whole table. Removing skills leaves
     * the links untouched, so the walk stays valid; the links are then removed
     * in one go. */
    for (uint32_t skill = (uint32_t)SosLinksFirst(skills->links, being); skill != 0u;
         skill = (uint32_t)SosLinksNext(skills->links, being, skill))
    {
        (void)SosTableRemove(skills->table, (SosTableKey){being, skill});
    }
    (void)SosLinksRemoveOwner(skills->links, being);
}

static uint64_t HashSkill(SosTableKey key, const void *record, void *context)
{
    if (key.second == 0u)
    {
        return 0u;
    }
    const uint64_t tick = *(const uint64_t *)context;
    const int64_t experience = EffectiveExperience((const SkillRecord *)record, tick);
    return SosHash64(SosHashCombine(SosHashCombine(key.first, key.second), (uint64_t)experience));
}

uint64_t SosSkillsHash(const SosSkills *skills, uint64_t tick)
{
    if (skills == NULL)
    {
        return 0u;
    }
    return SosHashCombine(skills->seed, SosTableFold(skills->table, HashSkill, &tick));
}
