#include "sim/calendar.h"
#include "sim/random.h"
#include "sim/skills.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define ALICE UINT64_C(1)
#define BOB UINT64_C(2)
#define CAROL UINT64_C(3)
#define DAVE UINT64_C(4)
#define YEAR ((uint64_t)SOS_TICKS_PER_YEAR)
#define HALF_SQUARED ((int64_t)SOS_SKILL_LEVEL_HALF * SOS_SKILL_LEVEL_HALF)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "skills test failed: %s\n", message);
        exit(1);
    }
}

/* The level curve rises without bound but with ever smaller returns, and half
 * of it is exactly the half-root. */
static void TestCurve(void)
{
    Expect(SosSkillsLevelCurve(0) == 0 && SosSkillsLevelCurve(-1) == 0,
           "no experience and less is level zero");
    Expect(SosSkillsLevelCurve(HALF_SQUARED) == SOS_Q16_ONE / 2, "half root is half level");
    const int64_t experiences[] = {1, 16, 256, 4096, 65536, 1048576, 268435456};
    SosQ16 previous = 0;
    for (size_t index = 0u; index < sizeof(experiences) / sizeof(experiences[0]); ++index)
    {
        const SosQ16 level = SosSkillsLevelCurve(experiences[index]);
        Expect(level > previous && level < SOS_Q16_ONE, "the curve rises toward one");
        previous = level;
    }
    const SosQ16 twice = SosSkillsLevelCurve(2 * HALF_SQUARED);
    const SosQ16 quadruple = SosSkillsLevelCurve(4 * HALF_SQUARED);
    Expect(twice > SOS_Q16_ONE / 2 && quadruple > twice && quadruple < SOS_Q16_ONE,
           "four times the experience is less than twice the level");
    Expect(quadruple - twice < twice - SOS_Q16_ONE / 2, "returns diminish with experience");
}

/* Talent is a pure function of seed, being and skill, so it never needs
 * storage and two machines agree. */
static void TestTalent(void)
{
    const SosQ16 talent = SosSkillsTalent(SEED, ALICE, SOS_SKILL_FORAGING);
    Expect(talent == SosSkillsTalent(SEED, ALICE, SOS_SKILL_FORAGING), "talent replays");
    Expect(talent >= 0 && talent < SOS_SKILL_TALENT_MAX, "talent stays below a quarter");
    Expect(talent != SosSkillsTalent(SEED, BOB, SOS_SKILL_FORAGING), "beings differ in talent");
    Expect(talent != SosSkillsTalent(SEED, ALICE, SOS_SKILL_HUNTING), "skills differ in talent");
    Expect(SosSkillsTalent(SEED, 0u, SOS_SKILL_FORAGING) == 0 &&
               SosSkillsTalent(SEED, ALICE, 0u) == 0,
           "nobody and no skill have no talent");

    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    Expect(SosSkillsLevel(skills, ALICE, SOS_SKILL_HUNTING, 0u) ==
               SosSkillsTalent(SEED, ALICE, SOS_SKILL_HUNTING),
           "an unpractised skill is exactly the being's talent");
    Expect(SosSkillsLevel(NULL, ALICE, SOS_SKILL_HUNTING, 0u) == 0 &&
               SosSkillsLevel(skills, 0u, SOS_SKILL_HUNTING, 0u) == 0 &&
               SosSkillsLevel(skills, ALICE, 0u, 0u) == 0,
           "a missing object, being or skill has no level");
    SosSkillsDestroy(skills);
}

static void TestPractice(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    Expect(!SosSkillsPractice(NULL, ALICE, SOS_SKILL_CRAFTING, 0u, 100u, SOS_Q16_ONE),
           "practice needs an object");
    Expect(!SosSkillsPractice(skills, 0u, SOS_SKILL_CRAFTING, 0u, 100u, SOS_Q16_ONE),
           "practice needs a being");
    Expect(!SosSkillsPractice(skills, ALICE, 0u, 0u, 100u, SOS_Q16_ONE), "practice needs a skill");
    Expect(!SosSkillsPractice(skills, ALICE, SOS_SKILL_CRAFTING, 0u, 0u, SOS_Q16_ONE),
           "practice needs effort");
    SosSkills *untouched = SosSkillsCreate(SEED);
    Expect(untouched != NULL, "skills are created");
    Expect(SosSkillsHash(skills, 0u) == SosSkillsHash(untouched, 0u),
           "an empty object hashes like an empty world");
    SosSkillsDestroy(untouched);

    const SosQ16 before = SosSkillsLevel(skills, ALICE, SOS_SKILL_CRAFTING, 1000u);
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_CRAFTING, 1000u, 200000u, SOS_Q16_ONE),
           "a being practises");
    const SosQ16 after = SosSkillsLevel(skills, ALICE, SOS_SKILL_CRAFTING, 1000u);
    Expect(after > before && after <= SOS_Q16_ONE, "practice raises the level, capped at one");

    /* Effort and aptitude both matter, and a second practice at the same tick
     * adds to the first. */
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_CRAFTING, 1000u, 200000u, SOS_Q16_ONE),
           "practice again");
    Expect(SosSkillsLevel(skills, ALICE, SOS_SKILL_CRAFTING, 1000u) > after,
           "more practice raises the level further");
    SosSkillsDestroy(skills);
}

/* Experience rusts toward a floor with a long half-life, lazily. */
static void TestRust(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    const uint64_t start = 1000u;
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_HEALING, start, 400000000u, SOS_Q16_ONE),
           "a long practice");
    const SosQ16 fresh = SosSkillsLevel(skills, ALICE, SOS_SKILL_HEALING, start);
    const SosQ16 later = SosSkillsLevel(skills, ALICE, SOS_SKILL_HEALING, start + 2u * YEAR);
    Expect(later < fresh && later > SosSkillsTalent(SEED, ALICE, SOS_SKILL_HEALING),
           "rust lowers the level but keeps a floor above talent");
    /* After a hundred half-lives what is above the floor is gone; a hundred
     * more changes nothing, so the floor is what remains. */
    const SosQ16 settled = SosSkillsLevel(skills, ALICE, SOS_SKILL_HEALING, start + 100u * YEAR);
    Expect(settled == SosSkillsLevel(skills, ALICE, SOS_SKILL_HEALING, start + 200u * YEAR),
           "the floor does not rust away");
    Expect(settled > SosSkillsTalent(SEED, ALICE, SOS_SKILL_HEALING) && settled < fresh,
           "a practised skill keeps something above talent");
    /* Time before the record was written decays nothing. */
    Expect(SosSkillsLevel(skills, ALICE, SOS_SKILL_HEALING, start) == fresh, "the past is stable");
    SosSkillsDestroy(skills);
}

/* Teaching is faster than practice but never lifts the learner past the
 * teacher. */
static void TestTeach(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_STORYTELLING, 0u, 400000000u, SOS_Q16_ONE),
           "the teacher practises long");
    const SosQ16 teacher = SosSkillsLevel(skills, ALICE, SOS_SKILL_STORYTELLING, 0u);
    Expect(SosSkillsTeach(skills, ALICE, BOB, SOS_SKILL_STORYTELLING, 0u, 100000u, SOS_Q16_ONE),
           "the teacher teaches");
    const SosQ16 learner = SosSkillsLevel(skills, BOB, SOS_SKILL_STORYTELLING, 0u);
    Expect(learner <= teacher, "the learner does not pass the teacher");
    Expect(learner > SosSkillsTalent(SEED, BOB, SOS_SKILL_STORYTELLING), "the learner gains");

    /* The same time learnt alone is worth less than a lesson. */
    SosSkills *alone = SosSkillsCreate(SEED);
    Expect(alone != NULL, "skills are created");
    Expect(SosSkillsPractice(alone, BOB, SOS_SKILL_STORYTELLING, 0u, 100000u, SOS_Q16_ONE),
           "the learner practises alone");
    Expect(learner > SosSkillsLevel(alone, BOB, SOS_SKILL_STORYTELLING, 0u),
           "a lesson beats the same time alone");
    SosSkillsDestroy(alone);

    /* A lesser teacher still holds a learner below it. */
    Expect(SosSkillsPractice(skills, CAROL, SOS_SKILL_STORYTELLING, 0u, 20000000u, SOS_Q16_ONE),
           "a middling teacher practises");
    const SosQ16 middling = SosSkillsLevel(skills, CAROL, SOS_SKILL_STORYTELLING, 0u);
    Expect(SosSkillsTeach(skills, CAROL, DAVE, SOS_SKILL_STORYTELLING, 0u, 400000000u, SOS_Q16_ONE),
           "the middling teacher teaches");
    Expect(SosSkillsLevel(skills, DAVE, SOS_SKILL_STORYTELLING, 0u) <= middling,
           "the learner stops at the teacher's level");
    Expect(SosSkillsLevel(skills, DAVE, SOS_SKILL_STORYTELLING, 0u) >
               SosSkillsTalent(SEED, DAVE, SOS_SKILL_STORYTELLING),
           "the learner still gains from a lesser teacher");

    Expect(!SosSkillsTeach(NULL, ALICE, BOB, SOS_SKILL_STORYTELLING, 0u, 10u, SOS_Q16_ONE) &&
               !SosSkillsTeach(skills, 0u, BOB, SOS_SKILL_STORYTELLING, 0u, 10u, SOS_Q16_ONE) &&
               !SosSkillsTeach(skills, ALICE, BOB, 0u, 0u, 10u, SOS_Q16_ONE) &&
               !SosSkillsTeach(skills, ALICE, BOB, SOS_SKILL_STORYTELLING, 0u, 0u, SOS_Q16_ONE) &&
               !SosSkillsTeach(skills, ALICE, ALICE, SOS_SKILL_STORYTELLING, 0u, 10u, SOS_Q16_ONE),
           "teaching validates its arguments");
    SosSkillsDestroy(skills);
}

static void TestEnumerateAndForget(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_FORAGING, 0u, 50000u, SOS_Q16_ONE) &&
               SosSkillsPractice(skills, ALICE, SOS_SKILL_HUNTING, 0u, 150000u, SOS_Q16_ONE) &&
               SosSkillsPractice(skills, ALICE, SOS_SKILL_COOKING, 0u, 90000u, SOS_Q16_ONE),
           "three skills are learnt");
    SosSkillEntry entries[8];
    const uint32_t count = SosSkillsOf(skills, ALICE, 0u, entries, 8u);
    Expect(count == 3u, "all three skills are listed");
    Expect(SosSkillsOf(skills, ALICE, 0u, entries, 1u) == 3u, "the count exceeds the capacity");
    Expect(SosSkillsOf(skills, ALICE, 0u, NULL, 0u) == 3u, "the count needs no buffer");
    Expect(entries[0].skill == SOS_SKILL_FORAGING, "oldest skill first");
    Expect(entries[0].level > SosSkillsTalent(SEED, ALICE, SOS_SKILL_FORAGING),
           "a listed skill has a practised level");
    Expect(SosSkillsOf(skills, 0u, 0u, entries, 8u) == 0u &&
               SosSkillsOf(NULL, ALICE, 0u, entries, 8u) == 0u &&
               SosSkillsOf(skills, ALICE, 0u, NULL, 8u) == 0u,
           "listing validates its arguments");

    SosSkillsForget(skills, ALICE);
    Expect(SosSkillsOf(skills, ALICE, 0u, NULL, 0u) == 0u, "forgetting drops every skill");
    Expect(SosSkillsLevel(skills, ALICE, SOS_SKILL_FORAGING, 0u) ==
               SosSkillsTalent(SEED, ALICE, SOS_SKILL_FORAGING),
           "a forgotten skill is only talent again");
    SosSkillsForget(skills, ALICE);
    SosSkillsForget(skills, 999u);
    SosSkillsForget(NULL, ALICE);
    Expect(SosSkillsOf(skills, ALICE, 0u, NULL, 0u) == 0u, "forgetting twice is harmless");
    SosSkillsDestroy(skills);
}

/* Forgetting one being drops exactly its own skills and leaves every other
 * being's records and lists intact. */
static void TestForgetIsolation(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    Expect(SosSkillsPractice(skills, ALICE, SOS_SKILL_FORAGING, 0u, 50000u, SOS_Q16_ONE) &&
               SosSkillsPractice(skills, ALICE, SOS_SKILL_HUNTING, 0u, 150000u, SOS_Q16_ONE),
           "the forgotten being learns two skills");
    Expect(SosSkillsPractice(skills, BOB, SOS_SKILL_FORAGING, 0u, 80000u, SOS_Q16_ONE) &&
               SosSkillsPractice(skills, CAROL, SOS_SKILL_COOKING, 0u, 120000u, SOS_Q16_ONE),
           "other beings learn skills of their own");
    SosSkillEntry before[4];
    const uint32_t bobCount = SosSkillsOf(skills, BOB, 0u, before, 4u);
    const int64_t bobExperience = before[0].experience;
    const SosQ16 bobLevel = before[0].level;
    const uint32_t carolCount = SosSkillsOf(skills, CAROL, 0u, before, 4u);
    const int64_t carolExperience = before[0].experience;
    Expect(bobCount == 1u && carolCount == 1u, "each survivor has one skill");

    SosSkillsForget(skills, ALICE);
    Expect(SosSkillsOf(skills, ALICE, 0u, NULL, 0u) == 0u, "the forgotten being keeps nothing");
    SosSkillEntry after[4];
    Expect(SosSkillsOf(skills, BOB, 0u, after, 4u) == bobCount &&
               after[0].skill == SOS_SKILL_FORAGING && after[0].experience == bobExperience &&
               after[0].level == bobLevel,
           "another being's skills are untouched");
    Expect(SosSkillsOf(skills, CAROL, 0u, after, 4u) == carolCount &&
               after[0].skill == SOS_SKILL_COOKING && after[0].experience == carolExperience,
           "the other being's skills are untouched");
    SosSkillsDestroy(skills);
}

/* A fixed set of work, used to pin the deterministic result and to compare
 * equal worlds. */
static void RunScenario(SosSkills *skills)
{
    (void)SosSkillsPractice(skills, ALICE, SOS_SKILL_FORAGING, 0u, 500000u, 40000);
    (void)SosSkillsPractice(skills, ALICE, SOS_SKILL_FORAGING, 500000u, 300000u, 60000);
    (void)SosSkillsPractice(skills, BOB, SOS_SKILL_KNAPPING, 100000u, 900000u, 65536);
    (void)SosSkillsTeach(skills, ALICE, BOB, SOS_SKILL_FORAGING, 400000u, 250000u, 50000);
    (void)SosSkillsPractice(skills, CAROL, SOS_SKILL_FIRE_MAKING, 200000u, 700000u, 32768);
    (void)SosSkillsForget(skills, CAROL);
    (void)SosSkillsPractice(skills, CAROL, SOS_SKILL_FIRE_MAKING, 300000u, 1000u, 65536);
}

static void TestHash(void)
{
    /* Equal histories hash equally however the table happens to hold them. */
    SosSkills *first = SosSkillsCreate(SEED);
    SosSkills *second = SosSkillsCreate(SEED);
    Expect(first != NULL && second != NULL, "skills are created");
    RunScenario(first);
    RunScenario(second);
    const uint64_t tick = 900000u;
    Expect(SosSkillsHash(first, tick) == SosSkillsHash(second, tick),
           "equal histories hash equally");
    Expect(SosSkillsHash(NULL, tick) == 0u, "no object has no hash");

    /* The order skills were first learnt in must not change the hash. */
    SosSkills *forward = SosSkillsCreate(SEED);
    SosSkills *backward = SosSkillsCreate(SEED);
    Expect(forward != NULL && backward != NULL, "skills are created");
    (void)SosSkillsPractice(forward, ALICE, SOS_SKILL_HUNTING, 0u, 100000u, SOS_Q16_ONE);
    (void)SosSkillsPractice(forward, ALICE, SOS_SKILL_CRAFTING, 0u, 100000u, SOS_Q16_ONE);
    (void)SosSkillsPractice(backward, ALICE, SOS_SKILL_CRAFTING, 0u, 100000u, SOS_Q16_ONE);
    (void)SosSkillsPractice(backward, ALICE, SOS_SKILL_HUNTING, 0u, 100000u, SOS_Q16_ONE);
    Expect(SosSkillsHash(forward, 0u) == SosSkillsHash(backward, 0u),
           "the order of learning does not matter");
    /* Rust is a function of the tick, so the two agree at every tick. */
    const uint64_t far = 5u * YEAR;
    Expect(SosSkillsHash(forward, far) == SosSkillsHash(backward, far),
           "equal rust means equal hashes");
    SosSkillsDestroy(forward);
    SosSkillsDestroy(backward);
    SosSkillsDestroy(first);
    SosSkillsDestroy(second);
}

/* Fixed points of the module. Every platform and compiler must produce these
 * levels and this hash. */
static void TestGoldenValues(void)
{
    SosSkills *skills = SosSkillsCreate(SEED);
    Expect(skills != NULL, "skills are created");
    RunScenario(skills);
    const uint64_t tick = 750000u;
    const uint64_t hash = SosSkillsHash(skills, tick);
    const SosQ16 aliceForaging = SosSkillsLevel(skills, ALICE, SOS_SKILL_FORAGING, tick);
    const SosQ16 bobForaging = SosSkillsLevel(skills, BOB, SOS_SKILL_FORAGING, tick);
    const SosQ16 carolFire = SosSkillsLevel(skills, CAROL, SOS_SKILL_FIRE_MAKING, tick);
    uint64_t fingerprint = hash;
    fingerprint = SosHashCombine(fingerprint, (uint64_t)aliceForaging);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)bobForaging);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)carolFire);
    const uint64_t expected = UINT64_C(0x7F3AFD54C12E6E8D);
    if (fingerprint != expected)
    {
        fprintf(stderr, "skills fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        fprintf(stderr, "hash 0x%016" PRIX64 ", levels %d %d %d\n", hash, aliceForaging,
                bobForaging, carolFire);
        Expect(0, "the skills result matches the reference");
    }
    SosSkillsDestroy(skills);
}

int main(void)
{
    TestCurve();
    TestTalent();
    TestPractice();
    TestRust();
    TestTeach();
    TestEnumerateAndForget();
    TestForgetIsolation();
    TestHash();
    TestGoldenValues();
    return 0;
}
