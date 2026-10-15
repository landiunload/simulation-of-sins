#include "sim/biology.h"
#include "sim/calendar.h"
#include "sim/disease.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "disease test failed: %s\n", message);
        exit(1);
    }
}

static void ExpectNear(int64_t actual, int64_t parent, int64_t slack, const char *message)
{
    const int64_t difference = actual > parent ? actual - parent : parent - actual;
    Expect(difference <= slack, message);
}

/* A disease with every duration long enough to stand on a boundary. */
static SosPathogen FixedPathogen(void)
{
    return (SosPathogen){
        .transmissibility = SOS_Q16_ONE,
        .incubationTicks = 2 * SOS_TICKS_PER_DAY,
        .infectiousTicks = 5 * SOS_TICKS_PER_DAY,
        .severity = SOS_Q16_ONE / 4,
        .lethality = SOS_Q16_ONE / 10,
        .immunityTicks = 30 * SOS_TICKS_PER_DAY,
        .route = SOS_PATHOGEN_ROUTE_AIR,
    };
}

/* Infects a fresh being with a certain contact and returns the exposure tick. */
static uint64_t Infect(SosInfection *inf, const SosPathogen *p, uint64_t id)
{
    SosRandom random = SosRandomStream(SEED, id);
    const uint64_t tick = 5000u;
    Expect(SosDiseaseExpose(inf, p, id, tick, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random),
           "a full-closeness contact infects");
    return tick;
}

static void TestStageTimeline(void)
{
    const SosPathogen p = FixedPathogen();
    const uint64_t id = 91u;
    SosInfection inf = {0};
    Expect(inf.stage == SOS_INFECTION_SUSCEPTIBLE && inf.pathogen == 0u,
           "a fresh being is susceptible");
    const uint64_t at = Infect(&inf, &p, id);
    Expect(inf.pathogen == id && inf.stage == SOS_INFECTION_EXPOSED && inf.stageTick == at,
           "exposure starts the incubation");
    Expect(inf.immuneUntil == 0u, "an active infection grants no protection");
    Expect(!SosDiseaseContagious(&inf, at), "an exposed being does not spread");

    SosDiseaseOutcome outcome = SosDiseaseAdvance(&inf, &p, at + p.incubationTicks - 1u);
    Expect(!outcome.becameInfectious && !outcome.recovered && !outcome.cleared,
           "nothing happens before the incubation ends");
    Expect(inf.stage == SOS_INFECTION_EXPOSED, "still incubating before the boundary");
    outcome = SosDiseaseAdvance(&inf, &p, at + p.incubationTicks);
    Expect(outcome.becameInfectious && !outcome.recovered && !outcome.cleared,
           "the incubation end is reported as infectious");
    Expect(inf.stage == SOS_INFECTION_INFECTIOUS && inf.stageTick == at + p.incubationTicks,
           "incubation ends at the boundary");
    Expect(SosDiseaseContagious(&inf, at + p.incubationTicks), "an infectious being spreads");
    Expect(!SosDiseaseContagious(&inf, at - 1u), "spreading cannot start before its stage");

    outcome = SosDiseaseAdvance(&inf, &p, at + p.incubationTicks + p.infectiousTicks - 1u);
    Expect(!outcome.recovered, "no recovery before the boundary");
    Expect(inf.stage == SOS_INFECTION_INFECTIOUS, "still spreading before the boundary");
    const uint64_t recoveredAt = at + p.incubationTicks + p.infectiousTicks;
    outcome = SosDiseaseAdvance(&inf, &p, recoveredAt);
    Expect(!outcome.becameInfectious && outcome.recovered && !outcome.cleared,
           "recovery is reported for the lethality roll");
    Expect(inf.stage == SOS_INFECTION_RECOVERED && inf.immuneUntil == recoveredAt + p.immunityTicks,
           "recovery starts immunity");
    Expect(!SosDiseaseContagious(&inf, recoveredAt), "a recovered being does not spread");
    outcome = SosDiseaseAdvance(&inf, &p, recoveredAt + p.immunityTicks - 1u);
    Expect(!outcome.cleared, "immune until the boundary");
    Expect(inf.stage == SOS_INFECTION_RECOVERED, "still immune before the boundary");
    outcome = SosDiseaseAdvance(&inf, &p, recoveredAt + p.immunityTicks);
    Expect(!outcome.becameInfectious && !outcome.recovered && outcome.cleared,
           "the immunity end is reported as cleared");
    Expect(inf.stage == SOS_INFECTION_SUSCEPTIBLE && inf.pathogen == 0u,
           "immunity ends at the boundary");

    /* One advance far ahead steps through the whole timeline and reports every
     * stage it crossed, even though the being ends up susceptible. */
    SosInfection skipped = {0};
    (void)Infect(&skipped, &p, id);
    const SosDiseaseOutcome skip =
        SosDiseaseAdvance(&skipped, &p, recoveredAt + p.immunityTicks + SOS_TICKS_PER_DAY);
    Expect(skip.becameInfectious && skip.recovered && skip.cleared,
           "a coarse step reports every crossed stage");
    Expect(skipped.stage == SOS_INFECTION_SUSCEPTIBLE, "a long gap skips to susceptible");

    /* A disease with no durations is over at the exposure tick. */
    SosPathogen flash = p;
    flash.incubationTicks = 0;
    flash.infectiousTicks = 0;
    flash.immunityTicks = 0;
    SosInfection gone = {0};
    const uint64_t flashAt = Infect(&gone, &flash, id);
    const SosDiseaseOutcome goneOutcome = SosDiseaseAdvance(&gone, &flash, flashAt);
    Expect(goneOutcome.becameInfectious && goneOutcome.recovered && goneOutcome.cleared,
           "a flash infection reports every stage");
    Expect(gone.stage == SOS_INFECTION_SUSCEPTIBLE, "a flash infection ends at once");
}

static void TestImmunity(void)
{
    const SosPathogen p = FixedPathogen();
    const uint64_t id = 77u;
    SosInfection inf = {0};
    const uint64_t at = Infect(&inf, &p, id);
    const uint64_t recoveredAt = at + p.incubationTicks + p.infectiousTicks;
    (void)SosDiseaseAdvance(&inf, &p, recoveredAt);
    Expect(inf.stage == SOS_INFECTION_RECOVERED, "recovered before the immunity test");

    SosRandom random = SosRandomStream(SEED, 3u);
    const SosInfection before = inf;
    Expect(!SosDiseaseExpose(&inf, &p, id, recoveredAt + 1u, SOS_Q16_ONE,
                             10u * SOS_DISEASE_EXPOSURE_TICKS, &random),
           "immunity blocks a new infection");
    Expect(inf.stage == before.stage && inf.pathogen == before.pathogen &&
               inf.stageTick == before.stageTick,
           "a blocked exposure changes nothing");
    Expect(!SosDiseaseExpose(&inf, &p, id + 1u, recoveredAt + 1u, SOS_Q16_ONE,
                             10u * SOS_DISEASE_EXPOSURE_TICKS, &random),
           "immunity also blocks another strain after recovery");

    const uint64_t later = recoveredAt + p.immunityTicks;
    Expect(SosDiseaseExpose(&inf, &p, id, later, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random),
           "a being is infected again once immunity ends");
    Expect(inf.stage == SOS_INFECTION_EXPOSED && inf.stageTick == later && inf.immuneUntil == 0u,
           "the new infection replaces the old state");

    /* An exposed or infectious being is not infected a second time. */
    SosInfection active = {0};
    (void)Infect(&active, &p, id);
    const SosInfection snapshot = active;
    Expect(!SosDiseaseExpose(&active, &p, id, active.stageTick, SOS_Q16_ONE,
                             10u * SOS_DISEASE_EXPOSURE_TICKS, &random),
           "an exposed being is not exposed again");
    Expect(active.stage == snapshot.stage && active.stageTick == snapshot.stageTick,
           "a refused exposure changes nothing");
    (void)SosDiseaseAdvance(&active, &p, active.stageTick + p.incubationTicks);
    Expect(active.stage == SOS_INFECTION_INFECTIOUS, "the being spreads");
    Expect(!SosDiseaseExpose(&active, &p, id, active.stageTick, SOS_Q16_ONE,
                             10u * SOS_DISEASE_EXPOSURE_TICKS, &random),
           "an infectious being is not exposed again");
}

static void TestExposureProbability(void)
{
    SosPathogen p = FixedPathogen();
    p.transmissibility = SOS_Q16_ONE / 2;
    SosRandom random = SosRandomStream(SEED, 5u);
    const int trials = 100000;
    int hits = 0;
    for (int trial = 0; trial < trials; ++trial)
    {
        SosInfection inf = {0};
        if (SosDiseaseExpose(&inf, &p, 7u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random))
        {
            ++hits;
        }
    }
    Expect(hits > trials * 49 / 100 && hits < trials * 51 / 100,
           "half transmissibility infects about half the time");

    /* A longer contact of the same strength reaches certainty. */
    p.transmissibility = SOS_Q16_ONE;
    for (int trial = 0; trial < 20; ++trial)
    {
        SosInfection inf = {0};
        Expect(SosDiseaseExpose(&inf, &p, 7u, 0u, SOS_Q16_ONE, 2u * SOS_DISEASE_EXPOSURE_TICKS,
                                &random),
               "a full-length contact always infects");
    }

    /* Impossible contacts never infect. */
    SosInfection inf = {0};
    p.transmissibility = 0;
    Expect(!SosDiseaseExpose(&inf, &p, 7u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random),
           "no transmissibility never infects");
    p.transmissibility = SOS_Q16_ONE;
    Expect(!SosDiseaseExpose(&inf, &p, 7u, 0u, 0, SOS_DISEASE_EXPOSURE_TICKS, &random),
           "no closeness never infects");
    Expect(!SosDiseaseExpose(&inf, &p, 7u, 0u, SOS_Q16_ONE, 0u, &random),
           "no duration never infects");
    Expect(!SosDiseaseExpose(&inf, &p, 0u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random),
           "strain 0 names no pathogen");
}

static void TestDamage(void)
{
    SosPathogen p = FixedPathogen();
    p.incubationTicks = 2 * SOS_TICKS_PER_DAY;
    p.infectiousTicks = 4 * SOS_TICKS_PER_DAY;
    p.severity = SOS_Q16_ONE / 4; /* a quarter of a bar per day */
    const uint64_t start = 100u;
    const SosInfection inf = {
        .pathogen = 1u, .stage = SOS_INFECTION_INFECTIOUS, .stageTick = start, .immuneUntil = 0u};
    Expect(SosDiseaseDamage(&inf, &p, start, 4u * SOS_TICKS_PER_DAY) == SOS_NEED_MAX,
           "severity over the infectious span costs a full bar");
    Expect(SosDiseaseDamage(&inf, &p, start, 2u * SOS_TICKS_PER_DAY) == SOS_NEED_MAX / 2u,
           "half the span does half the damage");
    Expect(SosDiseaseDamage(&inf, &p, start, SOS_TICKS_PER_DAY / 2u) > 0u,
           "a partial day is worth some damage");
    Expect(SosDiseaseDamage(&inf, &p, start - SOS_TICKS_PER_DAY, SOS_TICKS_PER_DAY) == 0u,
           "before the window nothing is lost");
    Expect(SosDiseaseDamage(&inf, &p, start + 4u * SOS_TICKS_PER_DAY, SOS_TICKS_PER_DAY) == 0u,
           "after the window nothing is lost");
    Expect(SosDiseaseDamage(&inf, &p, start, 0u) == 0u, "an empty span loses nothing");
    Expect(SosDiseaseDamage(&inf, &p, 0u, UINT64_MAX) == SOS_NEED_MAX,
           "an enormous span is clamped to a full bar");

    const SosInfection exposed = {
        .pathogen = 1u, .stage = SOS_INFECTION_EXPOSED, .stageTick = start, .immuneUntil = 0u};
    Expect(SosDiseaseDamage(&exposed, &p, start, p.incubationTicks) == 0u,
           "incubation does not damage");
    Expect(SosDiseaseDamage(&exposed, &p, start + p.incubationTicks, 4u * SOS_TICKS_PER_DAY) ==
               SOS_NEED_MAX,
           "the infectious window follows the incubation");

    const SosInfection recovered = {.pathogen = 1u,
                                    .stage = SOS_INFECTION_RECOVERED,
                                    .stageTick = start + 4u * SOS_TICKS_PER_DAY,
                                    .immuneUntil = 0u};
    Expect(SosDiseaseDamage(&recovered, &p, start, 4u * SOS_TICKS_PER_DAY) == SOS_NEED_MAX,
           "a recovered being still names its past window");

    const SosInfection susceptible = {0};
    Expect(SosDiseaseDamage(&susceptible, &p, 0u, UINT64_MAX) == 0u,
           "a susceptible being loses nothing");
    SosPathogen mild = p;
    mild.severity = 0;
    Expect(SosDiseaseDamage(&inf, &mild, start, 4u * SOS_TICKS_PER_DAY) == 0u,
           "a harmless strain does no damage");
}

/* A span that crosses stage boundaries counts only the infectious ticks, from
 * the state as it was at the start of the span. */
static void TestDamageAcrossBoundaries(void)
{
    SosPathogen p = FixedPathogen();
    p.incubationTicks = 2 * SOS_TICKS_PER_DAY;
    p.infectiousTicks = 4 * SOS_TICKS_PER_DAY;
    p.severity = SOS_Q16_ONE / 4;
    const uint64_t start = 1000u;
    const uint64_t infectious = p.infectiousTicks;

    /* Starting in incubation, a span that ends after recovery still counts the
     * whole infectious window exactly once, however far past recovery it runs. */
    const SosInfection exposed = {
        .pathogen = 1u, .stage = SOS_INFECTION_EXPOSED, .stageTick = start, .immuneUntil = 0u};
    Expect(SosDiseaseDamage(&exposed, &p, start,
                            p.incubationTicks + infectious + 10u * SOS_TICKS_PER_DAY) ==
               SOS_NEED_MAX,
           "a span from exposure past recovery costs the whole window");
    Expect(SosDiseaseDamage(&exposed, &p, start + p.incubationTicks + infectious / 2u,
                            infectious) == SOS_NEED_MAX / 2u,
           "a span from mid-infectious past recovery costs the rest");
    Expect(SosDiseaseDamage(&exposed, &p, start, p.incubationTicks + infectious) == SOS_NEED_MAX,
           "a span ending exactly at recovery costs the whole window");

    /* The same window split over many spans adds up to the same total, up to
     * the integer rounding each short span introduces. */
    uint64_t split = 0u;
    for (uint64_t tick = start;
         tick < start + p.incubationTicks + infectious + 3u * SOS_TICKS_PER_DAY;
         tick += SOS_TICKS_PER_DAY / 3u)
    {
        const SosInfection snapshot = exposed; /* the state at the span start */
        split += SosDiseaseDamage(&snapshot, &p, tick, SOS_TICKS_PER_DAY / 3u);
    }
    Expect(split <= SOS_NEED_MAX && split + 1000u >= SOS_NEED_MAX,
           "fine spans sum to the coarse damage up to rounding");
}

/* One coarse advance must equal many fine advances in both the damage and the
 * reported stages, including a step that crosses every stage at once. */
static void TestAdvanceEquivalence(void)
{
    SosPathogen p = FixedPathogen();
    p.severity = SOS_Q16_ONE / 8; /* whole-infectious damage stays below one bar */
    const uint64_t id = 42u;
    const uint64_t start = 1000u;
    const uint64_t span =
        p.incubationTicks + p.infectiousTicks + p.immunityTicks + 3u * SOS_TICKS_PER_DAY;

    SosInfection coarse = {
        .pathogen = id, .stage = SOS_INFECTION_EXPOSED, .stageTick = start, .immuneUntil = 0u};
    const uint64_t coarseDamage = SosDiseaseDamage(&coarse, &p, start, span);
    const SosDiseaseOutcome coarseOutcome = SosDiseaseAdvance(&coarse, &p, start + span);

    SosInfection fine = {
        .pathogen = id, .stage = SOS_INFECTION_EXPOSED, .stageTick = start, .immuneUntil = 0u};
    uint64_t fineDamage = 0u;
    SosDiseaseOutcome fineOutcome = {0};
    for (uint64_t tick = start; tick < start + span;)
    {
        const uint64_t remaining = start + span - tick;
        const uint64_t step = remaining < SOS_TICKS_PER_DAY ? remaining : SOS_TICKS_PER_DAY;
        fineDamage += SosDiseaseDamage(&fine, &p, tick, step);
        const SosDiseaseOutcome outcome = SosDiseaseAdvance(&fine, &p, tick + step);
        fineOutcome.becameInfectious |= outcome.becameInfectious;
        fineOutcome.recovered |= outcome.recovered;
        fineOutcome.cleared |= outcome.cleared;
        tick += step;
    }

    Expect(coarseDamage == (uint64_t)SOS_NEED_MAX * 5u / 8u,
           "the coarse span costs the whole window");
    Expect(coarseDamage == fineDamage, "a coarse span damages exactly like fine steps");
    Expect(coarseOutcome.becameInfectious == fineOutcome.becameInfectious &&
               coarseOutcome.recovered == fineOutcome.recovered &&
               coarseOutcome.cleared == fineOutcome.cleared,
           "a coarse step reports the same stages as fine steps");
    Expect(coarse.stage == fine.stage && coarse.pathogen == fine.pathogen &&
               coarse.stageTick == fine.stageTick && coarse.immuneUntil == fine.immuneUntil,
           "a coarse step reaches the same state as fine steps");
    Expect(coarseOutcome.recovered, "the coarse step still reports the recovery");
}

/* A typical strain costs about a quarter of the health bar, so the average
 * infection does not kill on its own; deaths come from lethality and from the
 * rare severe strains in the tail. */
static void TestMeanDamage(void)
{
    const int strains = 20000;
    uint64_t total = 0u;
    for (int index = 0; index < strains; ++index)
    {
        /* Hash the index so the sample spreads over many lineages, not just one
         * lineage's drift. */
        uint64_t id = SosHash64((uint64_t)index + 1u);
        if (id == 0u)
        {
            id = 1u;
        }
        SosPathogen p;
        Expect(SosDiseasePathogen(SEED, id, &p), "a strain exists");
        const SosInfection inf = {
            .pathogen = id, .stage = SOS_INFECTION_INFECTIOUS, .stageTick = 0u, .immuneUntil = 0u};
        total += SosDiseaseDamage(&inf, &p, 0u, p.infectiousTicks);
    }
    const uint64_t mean = total / (uint64_t)strains;
    if (!(mean > (uint64_t)SOS_NEED_MAX / 6u && mean < (uint64_t)SOS_NEED_MAX / 3u))
    {
        fprintf(stderr, "disease mean damage: %" PRIu64 " of %" PRIu64 "\n", mean,
                (uint64_t)SOS_NEED_MAX);
        Expect(0, "the mean infection costs about a quarter of the health bar");
    }
}

static void TestMutation(void)
{
    SosRandom random = SosRandomStream(SEED, 11u);
    for (int trial = 0; trial < 300; ++trial)
    {
        uint64_t id = SosRandomNext(&random);
        if (id == 0u)
        {
            id = 1u;
        }
        SosPathogen parent;
        Expect(SosDiseasePathogen(SEED, id, &parent), "a parent pathogen exists");
        const uint64_t child = SosDiseaseMutate(SEED, id, &random);
        Expect(child != 0u && child != id, "a mutation is a new strain");
        Expect((child >> SOS_DISEASE_DRIFT_BITS) == (id >> SOS_DISEASE_DRIFT_BITS),
               "a mutation keeps the lineage");
        SosPathogen mutant;
        Expect(SosDiseasePathogen(SEED, child, &mutant), "a mutant pathogen exists");
        ExpectNear(mutant.transmissibility, parent.transmissibility, SOS_Q16_ONE / 8 + 2,
                   "transmissibility stays close");
        ExpectNear((int64_t)mutant.incubationTicks, (int64_t)parent.incubationTicks,
                   SOS_TICKS_PER_DAY + 2, "incubation stays close");
        ExpectNear((int64_t)mutant.infectiousTicks, (int64_t)parent.infectiousTicks,
                   2 * SOS_TICKS_PER_DAY + 2, "infectiousness stays close");
        ExpectNear(mutant.severity, parent.severity, SOS_Q16_ONE / 64 + 2, "severity stays close");
        ExpectNear(mutant.lethality, parent.lethality, SOS_Q16_ONE / 8 + 2,
                   "lethality stays close");
        ExpectNear((int64_t)mutant.immunityTicks, (int64_t)parent.immunityTicks,
                   SOS_TICKS_PER_YEAR / 4 + 2, "immunity stays close");
        Expect(mutant.route == parent.route, "a mutation keeps the route");
    }

    /* The same stream and strain mutate the same way. */
    SosRandom a = SosRandomStream(SEED, 21u);
    SosRandom b = SosRandomStream(SEED, 21u);
    for (int trial = 0; trial < 50; ++trial)
    {
        const uint64_t id = SosRandomNext(&a) | 1u;
        Expect(id == (SosRandomNext(&b) | 1u), "a stream replays");
        Expect(SosDiseaseMutate(SEED, id, &a) == SosDiseaseMutate(SEED, id, &b),
               "a mutation replays");
    }
}

static void TestPathogens(void)
{
    const uint64_t ids[] = {1u, 2u, 0x00000000ABCDEF01u, 0x0123456789ABCDEFu, UINT64_MAX};
    for (size_t index = 0u; index < sizeof(ids) / sizeof(ids[0]); ++index)
    {
        SosPathogen first;
        SosPathogen again;
        Expect(SosDiseasePathogen(SEED, ids[index], &first), "a pathogen exists");
        Expect(SosDiseasePathogen(SEED, ids[index], &again), "a pathogen is rebuilt");
        Expect(first.transmissibility == again.transmissibility &&
                   first.incubationTicks == again.incubationTicks &&
                   first.infectiousTicks == again.infectiousTicks &&
                   first.severity == again.severity && first.lethality == again.lethality &&
                   first.immunityTicks == again.immunityTicks && first.route == again.route,
               "a pathogen is a function of the seed and the id");
        Expect(first.transmissibility >= 0 && first.transmissibility <= SOS_Q16_ONE,
               "transmissibility is a chance");
        Expect(first.incubationTicks > 0u && first.incubationTicks <= 8 * SOS_TICKS_PER_DAY,
               "incubation is hours to days");
        Expect(first.infectiousTicks > 0u && first.infectiousTicks <= 12 * SOS_TICKS_PER_DAY,
               "infectiousness lasts days");
        Expect(first.severity >= 0 && first.severity <= SOS_Q16_ONE, "severity is a share");
        Expect(first.lethality >= 0 && first.lethality <= SOS_Q16_ONE, "lethality is a chance");
        Expect(first.immunityTicks >= 0u && first.immunityTicks <= 2 * SOS_TICKS_PER_YEAR,
               "immunity lasts months to years");
        Expect(first.route >= SOS_PATHOGEN_ROUTE_CONTACT && first.route < SOS_PATHOGEN_ROUTE_COUNT,
               "every pathogen has a route");
    }
}

static uint64_t Fingerprint(const SosPathogen *p)
{
    uint64_t hash = SosHash64((uint64_t)(uint32_t)p->transmissibility);
    hash = SosHashCombine(hash, p->incubationTicks);
    hash = SosHashCombine(hash, p->infectiousTicks);
    hash = SosHashCombine(hash, (uint64_t)(uint32_t)p->severity);
    hash = SosHashCombine(hash, (uint64_t)(uint32_t)p->lethality);
    hash = SosHashCombine(hash, p->immunityTicks);
    return SosHashCombine(hash, (uint64_t)p->route);
}

/* Fixed points of pathogen generation and mutation. Every platform and
 * compiler must produce exactly these strains. */
static void TestGolden(void)
{
    const uint64_t ids[] = {1u, 0x00000000ABCDEF01u, 0x0123456789ABCDEFu, UINT64_MAX};
    SosRandom random = SosRandomStream(SEED, 13u);
    uint64_t fingerprint = UINT64_C(0x64697365617365);
    for (size_t index = 0u; index < sizeof(ids) / sizeof(ids[0]); ++index)
    {
        SosPathogen pathogen;
        Expect(SosDiseasePathogen(SEED, ids[index], &pathogen), "a golden pathogen exists");
        fingerprint = SosHashCombine(fingerprint, Fingerprint(&pathogen));
        fingerprint = SosHashCombine(fingerprint, SosDiseaseMutate(SEED, ids[index], &random));
    }
    const uint64_t expected = UINT64_C(0x13F22B8C4A0E05F3);
    if (fingerprint != expected)
    {
        fprintf(stderr, "disease fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the pathogens match the reference");
    }
}

static void TestArguments(void)
{
    SosPathogen p = FixedPathogen();
    SosInfection inf = {0};
    SosRandom random = SosRandomStream(SEED, 1u);

    Expect(!SosDiseasePathogen(SEED, 0u, &p) && !SosDiseasePathogen(SEED, 1u, NULL),
           "pathogens reject missing ids and outputs");
    Expect(!SosDiseaseExpose(NULL, &p, 1u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, &random) &&
               !SosDiseaseExpose(&inf, NULL, 1u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS,
                                 &random) &&
               !SosDiseaseExpose(&inf, &p, 1u, 0u, SOS_Q16_ONE, SOS_DISEASE_EXPOSURE_TICKS, NULL),
           "exposure rejects missing arguments");
    Expect(!SosDiseaseContagious(NULL, 0u) && SosDiseaseDamage(NULL, &p, 0u, 1u) == 0u &&
               SosDiseaseDamage(&inf, NULL, 0u, 1u) == 0u,
           "queries reject missing arguments");
    Expect(SosDiseaseMutate(SEED, 7u, NULL) == 0u && SosDiseaseMutate(SEED, 0u, &random) == 0u,
           "mutation rejects missing arguments");

    /* Advancing without a stage or a pathogen changes nothing. */
    const SosInfection idle = inf;
    const SosDiseaseOutcome noPathogen = SosDiseaseAdvance(&inf, NULL, 100u);
    Expect(!noPathogen.becameInfectious && !noPathogen.recovered && !noPathogen.cleared,
           "advancing without a pathogen reports nothing");
    Expect(inf.stage == idle.stage && inf.stageTick == idle.stageTick,
           "advancing without a pathogen changes nothing");
    const SosDiseaseOutcome noInfection = SosDiseaseAdvance(NULL, &p, 100u);
    Expect(!noInfection.becameInfectious && !noInfection.recovered && !noInfection.cleared,
           "advancing without an infection reports nothing");
    SosInfection susceptible = {0};
    const SosDiseaseOutcome never = SosDiseaseAdvance(&susceptible, &p, UINT64_MAX);
    Expect(!never.becameInfectious && !never.recovered && !never.cleared,
           "a susceptible being never transitions");
    Expect(susceptible.stage == SOS_INFECTION_SUSCEPTIBLE, "a susceptible being never changes");
}

int main(void)
{
    TestStageTimeline();
    TestImmunity();
    TestExposureProbability();
    TestDamage();
    TestDamageAcrossBoundaries();
    TestAdvanceEquivalence();
    TestMeanDamage();
    TestMutation();
    TestPathogens();
    TestGolden();
    TestArguments();
    return 0;
}
