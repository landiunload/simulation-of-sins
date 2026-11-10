#ifndef SOS_SIM_SKILLS_H
#define SOS_SIM_SKILLS_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Skills.
 *
 * A skill is any 32-bit id, so the set of skills is open-ended and future
 * technologies can name new ones without changing this module; the well-known
 * ones below cover the founding crafts. What a being has learnt of a skill is
 * experience, and its level follows from that experience through an
 * integer-square-root curve with diminishing returns: the first efforts pay
 * off, mastery comes ever more slowly.
 *
 * The unbounded world stores only what beings change. Experience for a skill a
 * being never practised does not exist as a record: every being has a seeded
 * innate talent for every skill, an offset added to the practised level, so
 * two people differ in skills nobody has taught them without a byte spent.
 * Practised skills live in a sparse table keyed (being, skill) and in a shared
 * SosLinks set keyed by the being, so a being's skills can be listed, counted
 * and forgotten when it dies.
 *
 * Rust is lazy. Experience above a retained floor halves every long half-life,
 * but it is only computed from the elapsed time when someone asks, exactly
 * like plant regrowth, so idle skills cost nothing while time passes. A being
 * keeps a floor of every skill it once learnt, so skills fade but are not
 * wholly forgotten. */

/* The largest innate talent, a quarter of a level; a being starts somewhere
 * inside [0, SOS_SKILL_TALENT_MAX) in every skill. */
#define SOS_SKILL_TALENT_MAX (SOS_Q16_ONE / 4)

/* The integer square root of experience at half level. Higher roots still pay
 * off, slowly: about ninety times this experience for nine tenths of a level,
 * so a lifetime of steady practice approaches mastery without reaching it. */
#define SOS_SKILL_LEVEL_HALF 4096

typedef enum SosSkill
{
    SOS_SKILL_NONE = 0,
    SOS_SKILL_FORAGING,
    SOS_SKILL_HUNTING,
    SOS_SKILL_KNAPPING,
    SOS_SKILL_FIRE_MAKING,
    SOS_SKILL_COOKING,
    SOS_SKILL_HEALING,
    SOS_SKILL_BUILDING,
    SOS_SKILL_PERSUASION,
    SOS_SKILL_FIGHTING,
    SOS_SKILL_CRAFTING,
    SOS_SKILL_STORYTELLING,
    /* Not a skill: the count of the well-known ones above. */
    SOS_SKILL_WELL_KNOWN_COUNT,
} SosSkill;

/* One learnt skill of a being, as seen by presentation and tools. */
typedef struct SosSkillEntry
{
    uint32_t skill;
    /* The being's level at the queried tick, in [0, SOS_Q16_ONE]. */
    SosQ16 level;
    /* Effective experience at the queried tick, after rust. */
    int64_t experience;
} SosSkillEntry;

typedef struct SosSkills SosSkills;

SosSkills *SosSkillsCreate(uint64_t seed);
void SosSkillsDestroy(SosSkills *skills);

/* The level `experience` buys before innate talent, in [0, SOS_Q16_ONE]:
 * sqrt(experience) / (sqrt(experience) + SOS_SKILL_LEVEL_HALF). Zero for
 * non-positive experience, and never quite one for finite experience. */
SosQ16 SosSkillsLevelCurve(int64_t experience);

/* The innate level offset of `being` in `skill`, in
 * [0, SOS_SKILL_TALENT_MAX), derived from `seed` alone and equal on every
 * machine. Zero when `being` or `skill` is zero, because neither can hold a
 * skill. */
SosQ16 SosSkillsTalent(uint64_t seed, uint64_t being, uint32_t skill);

/* Raises `being`'s experience of `skill` by `effortTicks` of effort scaled by
 * `aptitude` (a fraction, clamped to [0, 1]). False for a NULL object, a zero
 * being, skill or effort, or when memory runs out, leaving the object
 * unchanged. Creates the skill on first practice. */
bool SosSkillsPractice(SosSkills *skills, uint64_t being, uint32_t skill, uint64_t tick,
                       uint64_t effortTicks, SosQ16 aptitude);

/* The level of `being` in `skill` at `tick`: the rusted experience through the
 * curve plus innate talent, clamped to [0, 1]. An unpractised skill gives the
 * being's talent. Zero for a NULL object or a zero being or skill. */
SosQ16 SosSkillsLevel(const SosSkills *skills, uint64_t being, uint32_t skill, uint64_t tick);

/* `learner` studies `skill` from `teacher` for `durationTicks`. Learning is
 * faster than solitary practice, and the learner never passes the teacher's
 * level at `tick`, whatever the effort or talent. False for a NULL object, a
 * zero teacher, learner, skill or duration, when the two are the same being,
 * or when memory runs out; true, learning nothing, when the teacher is no help
 * because the learner is already at or above the teacher's level. */
bool SosSkillsTeach(SosSkills *skills, uint64_t teacher, uint64_t learner, uint32_t skill,
                    uint64_t tick, uint64_t durationTicks, SosQ16 learnerAptitude);

/* Writes up to `capacity` of `being`'s learnt skills into `out` and returns how
 * many there are, which may exceed `capacity`. The order is the order the
 * skills were first practised, oldest first, and is the same on every machine.
 * Zero for a NULL object or a zero being; a NULL `out` with a non-zero
 * `capacity` is rejected. `out` may be NULL with a zero capacity to ask only
 * for the count. */
uint32_t SosSkillsOf(const SosSkills *skills, uint64_t being, uint64_t tick, SosSkillEntry *out,
                     uint32_t capacity);

/* Drops every skill of `being`, for death. Does nothing for a NULL object or a
 * zero being. */
void SosSkillsForget(SosSkills *skills, uint64_t being);

/* Folds the learnt skills at `tick` into one value. Rusted experience is what
 * counts, so two worlds whose skills have decayed by the same amount hash
 * equally however many steps got them there, and records are summed so the
 * order of the table does not matter. Zero for a NULL object. */
uint64_t SosSkillsHash(const SosSkills *skills, uint64_t tick);

#endif
