#include "sim/fixed.h"
#include "sim/genome.h"
#include "sim/random.h"
#include "sim/space.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
/* A trait is a count of SOS_GENOME_TRAIT_ALLELES alleles, so one allele is
 * this many Q16 steps. */
#define TRAIT_STEP (SOS_Q16_ONE / (int32_t)SOS_GENOME_TRAIT_ALLELES)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "genome test failed: %s\n", message);
        exit(1);
    }
}

static uint32_t SetBits(uint64_t value)
{
    value -= (value >> 1u) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2u) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4u)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (uint32_t)((value * UINT64_C(0x0101010101010101)) >> 56u);
}

static uint32_t AlleleCount(SosQ16 trait)
{
    return (uint32_t)(trait / TRAIT_STEP);
}

/* The bit positions a trait id selects, recovered by probing a genome with one
 * maternal allele set: there the trait's allele count is exactly one. Both
 * haplotypes read these loci, so a test can build a genome of any count.
 * Returns how many of the SOS_GENOME_TRAIT_LOCI were found. */
static uint32_t TraitLoci(uint32_t traitId, uint32_t *loci)
{
    uint32_t found = 0u;
    for (uint32_t bit = 0u; bit < SOS_GENOME_HAPLOTYPE_BITS; ++bit)
    {
        SosGenome probe = {{0u, 0u, 0u, 0u}, {0u, 0u, 0u, 0u}};
        probe.maternal[bit >> 6u] = UINT64_C(1) << (bit & 63u);
        if (AlleleCount(SosGenomeTrait(&probe, traitId)) == 1u)
        {
            loci[found] = bit;
            ++found;
        }
    }
    return found;
}

/* A genome whose `traitId` allele count is exactly `count`, built from the
 * trait's own loci so the count can be pinned without knowing the hash. */
static void GenomeWithCount(uint32_t traitId, uint32_t count, SosGenome *out)
{
    uint32_t loci[SOS_GENOME_TRAIT_LOCI];
    Expect(TraitLoci(traitId, loci) == SOS_GENOME_TRAIT_LOCI, "a trait reads its loci");
    *out = (SosGenome){{0u, 0u, 0u, 0u}, {0u, 0u, 0u, 0u}};
    for (uint32_t allele = 0u; allele < count; ++allele)
    {
        const uint32_t bit = loci[allele % SOS_GENOME_TRAIT_LOCI];
        uint64_t *haplotype = allele < SOS_GENOME_TRAIT_LOCI ? out->maternal : out->paternal;
        haplotype[bit >> 6u] |= UINT64_C(1) << (bit & 63u);
    }
}

static int Same(const SosGenome *a, const SosGenome *b)
{
    for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
    {
        if (a->maternal[word] != b->maternal[word] || a->paternal[word] != b->paternal[word])
        {
            return 0;
        }
    }
    return 1;
}

static void TestFounder(void)
{
    SosGenome founder;
    Expect(!SosGenomeFounder(SEED, 1u, NULL), "a founder needs an output");
    Expect(SosGenomeFounder(SEED, 1u, &founder), "a founder is made");
    SosGenome again;
    Expect(SosGenomeFounder(SEED, 1u, &again) && Same(&founder, &again), "a founder replays");
    SosGenome other;
    Expect(SosGenomeFounder(SEED, 2u, &other) && !Same(&founder, &other),
           "different serials differ");
    Expect(SosGenomeFounder(0u, UINT64_MAX, &other), "extreme seeds and serials are accepted");
    Expect(SosGenomeHash(&founder) != 0u && SosGenomeHash(NULL) == 0u,
           "hashing folds a genome and rejects null");
    Expect(SosGenomeHash(&founder) == SosGenomeHash(&again), "equal genomes hash equally");
}

static void TestInherit(void)
{
    SosGenome mother;
    SosGenome father;
    Expect(SosGenomeFounder(SEED, 10u, &mother) && SosGenomeFounder(SEED, 11u, &father),
           "parents are made");
    SosGenome child;
    Expect(!SosGenomeInherit(NULL, &father, SEED, 100u, &child) &&
               !SosGenomeInherit(&mother, NULL, SEED, 100u, &child) &&
               !SosGenomeInherit(&mother, &father, SEED, 100u, NULL),
           "inherit checks its arguments");
    Expect(SosGenomeInherit(&mother, &father, SEED, 100u, &child), "a child is made");
    Expect(!Same(&mother, &child), "a child is not a parent");
    SosGenome again;
    Expect(SosGenomeInherit(&mother, &father, SEED, 100u, &again) && Same(&child, &again),
           "a child replays");
    SosGenome twin;
    Expect(SosGenomeInherit(&mother, &father, SEED, 101u, &twin) && !Same(&child, &twin),
           "siblings differ");
    Expect(SosGenomeInherit(&mother, &mother, SEED, 102u, &again),
           "one genome may be both parents");
    /* The output may be one of the parents. */
    SosGenome aliased = mother;
    Expect(SosGenomeInherit(&aliased, &father, SEED, 103u, &aliased), "output may alias a parent");
    SosGenome reference;
    Expect(SosGenomeInherit(&mother, &father, SEED, 103u, &reference), "the same child is remade");
    Expect(Same(&aliased, &reference), "aliasing does not change the child");
}

static void TestTraits(void)
{
    SosGenome genome;
    Expect(SosGenomeFounder(SEED, 7u, &genome), "a genome is made");
    Expect(SosGenomeTrait(NULL, SOS_GENOME_TRAIT_HEIGHT) == 0, "no genome means no trait");
    const SosQ16 height = SosGenomeTrait(&genome, SOS_GENOME_TRAIT_HEIGHT);
    Expect(height >= 0 && height <= SOS_Q16_ONE, "a trait is a share of one");
    Expect(SosGenomeTrait(&genome, SOS_GENOME_TRAIT_HEIGHT) == height, "a trait repeats");
    int differs = 0;
    for (uint32_t id = 0u; id < SOS_GENOME_TRAIT_COUNT; ++id)
    {
        if (SosGenomeTrait(&genome, id) != height)
        {
            differs = 1;
        }
    }
    Expect(differs, "trait ids read different loci");
    Expect(SosGenomeTrait(&genome, UINT32_MAX) >= 0, "an unknown id is still a trait");
}

/* The percentile is a non-decreasing function of the allele count, so the 33
 * counts sweep the range of [0, ONE]. */
static void TestPercentileMonotone(void)
{
    const uint32_t traitId = SOS_GENOME_TRAIT_HEIGHT;
    Expect(SosGenomePercentile(NULL, traitId) == 0, "no genome means no percentile");
    SosQ16 previous = -1;
    for (uint32_t count = 0u; count <= SOS_GENOME_TRAIT_ALLELES; ++count)
    {
        SosGenome genome;
        GenomeWithCount(traitId, count, &genome);
        Expect(AlleleCount(SosGenomeTrait(&genome, traitId)) == count, "the built count is exact");
        const SosQ16 percentile = SosGenomePercentile(&genome, traitId);
        Expect(percentile >= 0 && percentile <= SOS_Q16_ONE, "a percentile is within one");
        Expect(percentile >= previous, "percentiles are monotone in the allele count");
        Expect(SosGenomePercentile(&genome, traitId) == percentile, "a percentile repeats");
        previous = percentile;
    }
    Expect(previous == SOS_Q16_ONE - 1, "the top count reaches the top of the range");
}

/* The raw trait is binomial and leaves the outer quarters nearly empty; the
 * percentile should instead spread founders over the whole range. */
static void TestPercentileUniform(void)
{
    const uint32_t families = 4000u;
    uint32_t quarters[4] = {0u, 0u, 0u, 0u};
    for (uint32_t index = 0u; index < families; ++index)
    {
        SosGenome genome;
        Expect(SosGenomeFounder(SEED, 7000u + index, &genome), "a founder is made");
        const SosQ16 percentile = SosGenomePercentile(&genome, SOS_GENOME_TRAIT_HEIGHT);
        const uint32_t quarter = (uint32_t)((int64_t)percentile * 4 / SOS_Q16_ONE);
        ++quarters[quarter];
    }
    for (uint32_t quarter = 0u; quarter < 4u; ++quarter)
    {
        Expect(quarters[quarter] > families / 10u, "every quarter holds founders");
        Expect(quarters[quarter] < families * 55u / 100u, "no quarter holds most founders");
    }
}

/* The percentile is a monotone map of the count, so it inherits the count's
 * parent-child correlation. Values are scaled down to keep the integer
 * covariance products inside 64 bits. */
static void TestPercentileHeritability(void)
{
    const uint32_t families = 3000u;
    const uint32_t traitId = SOS_GENOME_TRAIT_HEIGHT;
    int64_t sumX = 0;
    int64_t sumY = 0;
    int64_t sumXX = 0;
    int64_t sumYY = 0;
    int64_t sumXY = 0;
    for (uint32_t index = 0u; index < families; ++index)
    {
        SosGenome mother;
        SosGenome father;
        SosGenome child;
        Expect(SosGenomeFounder(SEED, 8000u + 2u * index, &mother) &&
                   SosGenomeFounder(SEED, 8001u + 2u * index, &father) &&
                   SosGenomeInherit(&mother, &father, SEED, 500000u + index, &child),
               "a family is made");
        const int64_t x =
            (SosGenomePercentile(&mother, traitId) + SosGenomePercentile(&father, traitId)) >> 8;
        const int64_t y = SosGenomePercentile(&child, traitId) >> 8;
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumYY += y * y;
        sumXY += x * y;
    }
    const int64_t n = (int64_t)families;
    const int64_t varX = n * sumXX - sumX * sumX;
    const int64_t varY = n * sumYY - sumY * sumY;
    const int64_t cov = n * sumXY - sumX * sumY;
    Expect(varX > 0 && varY > 0 && cov > 0, "percentiles vary and covary");
    const uint64_t spread = SosIntegerSqrt((uint64_t)varX * (uint64_t)varY);
    Expect((uint64_t)cov * 2u > spread, "a child percentile lands near its parents' mean");
}

static void TestDistribution(void)
{
    const uint32_t families = 4000u;
    int64_t sum = 0;
    uint32_t minimum = SOS_GENOME_TRAIT_ALLELES;
    uint32_t maximum = 0u;
    uint32_t middle = 0u;
    for (uint32_t index = 0u; index < families; ++index)
    {
        SosGenome genome;
        Expect(SosGenomeFounder(SEED, 1000u + index, &genome), "a founder is made");
        const uint32_t count = AlleleCount(SosGenomeTrait(&genome, SOS_GENOME_TRAIT_HEIGHT));
        sum += count;
        if (count < minimum)
        {
            minimum = count;
        }
        if (count > maximum)
        {
            maximum = count;
        }
        if (count >= 13u && count <= 19u)
        {
            ++middle;
        }
    }
    /* Founders flip independent alleles, so a trait is binomial around half. */
    const int64_t mean = sum / (int64_t)families;
    Expect(mean >= 15 && mean <= 17, "the mean trait sits at half");
    Expect(minimum <= 10u && maximum >= 22u, "the trait spans the bell");
    Expect(middle * 10u > families * 6u, "most founders cluster near the mean");
}

/* The child of two parents tends to sit between them: its expected allele at
 * every trait locus is the parents' mean. */
static void TestHeritability(void)
{
    const uint32_t families = 3000u;
    const uint32_t traits[3] = {SOS_GENOME_TRAIT_HEIGHT, SOS_GENOME_TRAIT_STRENGTH,
                                SOS_GENOME_TRAIT_INTELLIGENCE};
    for (uint32_t trait = 0u; trait < 3u; ++trait)
    {
        int64_t sumX = 0;
        int64_t sumY = 0;
        int64_t sumXX = 0;
        int64_t sumYY = 0;
        int64_t sumXY = 0;
        for (uint32_t index = 0u; index < families; ++index)
        {
            SosGenome mother;
            SosGenome father;
            SosGenome child;
            Expect(SosGenomeFounder(SEED, 5000u + 2u * index, &mother) &&
                       SosGenomeFounder(SEED, 5001u + 2u * index, &father) &&
                       SosGenomeInherit(&mother, &father, SEED, 200000u + index, &child),
                   "a family is made");
            const int64_t x = (int64_t)AlleleCount(SosGenomeTrait(&mother, traits[trait])) +
                              AlleleCount(SosGenomeTrait(&father, traits[trait]));
            const int64_t y = AlleleCount(SosGenomeTrait(&child, traits[trait]));
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumYY += y * y;
            sumXY += x * y;
        }
        const int64_t n = (int64_t)families;
        const int64_t varX = n * sumXX - sumX * sumX;
        const int64_t varY = n * sumYY - sumY * sumY;
        const int64_t cov = n * sumXY - sumX * sumY;
        Expect(varX > 0 && varY > 0 && cov > 0, "traits vary and covary");
        /* 2 * cov > sqrt(varX * varY) is a correlation above one half, checked
         * with the integer square root to keep every product in 64 bits. */
        const uint64_t spread = SosIntegerSqrt((uint64_t)varX * (uint64_t)varY);
        Expect((uint64_t)cov * 2u > spread, "a child lands near its parents' mean");
    }
}

/* A parent homozygous everywhere passes one allele per bit, so every flipped
 * bit is a mutation and the count is the rate. */
static void TestMutationRate(void)
{
    const SosGenome mother = {{0u, 0u, 0u, 0u}, {0u, 0u, 0u, 0u}};
    const SosGenome father = {{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX},
                              {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
    const uint32_t children = 4000u;
    uint64_t flips = 0u;
    for (uint32_t index = 0u; index < children; ++index)
    {
        SosGenome child;
        Expect(SosGenomeInherit(&mother, &father, SEED, 300000u + index, &child),
               "a child is made");
        for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
        {
            flips += SetBits(child.maternal[word]);
            flips += 64u - SetBits(child.paternal[word]);
        }
    }
    /* About one mutation per 256-bit haplotype, so about two per child. */
    const uint64_t expected = (uint64_t)children * 2u;
    Expect(flips > expected * 7u / 10u && flips < expected * 14u / 10u,
           "mutations happen about once per haplotype");
}

/* A crossbred parent passes a mosaic of its two haplotypes. */
static void TestCrossover(void)
{
    const SosGenome parent = {{0u, 0u, 0u, 0u}, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
    uint32_t mixed = 0u;
    for (uint32_t index = 0u; index < 200u; ++index)
    {
        SosGenome child;
        Expect(SosGenomeInherit(&parent, &parent, SEED, 400000u + index, &child),
               "a child is made");
        uint32_t ones = 0u;
        for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
        {
            ones += SetBits(child.maternal[word]);
        }
        if (ones > 16u && ones < 240u)
        {
            ++mixed;
        }
    }
    Expect(mixed > 100u, "crossovers mix the two haplotypes");
}

static void TestSimilarity(void)
{
    SosGenome first;
    SosGenome second;
    Expect(SosGenomeFounder(SEED, 20u, &first) && SosGenomeFounder(SEED, 21u, &second),
           "genomes are made");
    Expect(SosGenomeSimilarity(NULL, &second) == 0 && SosGenomeSimilarity(&first, NULL) == 0,
           "similarity checks its arguments");
    Expect(SosGenomeSimilarity(&first, &first) == SOS_Q16_ONE, "a genome equals itself");
    Expect(SosGenomeSimilarity(&first, &second) == SosGenomeSimilarity(&second, &first),
           "similarity is symmetric");

    const int64_t half = SOS_Q16_ONE / 2;
    const uint32_t pairs = 2000u;
    int64_t sum = 0;
    for (uint32_t index = 0u; index < pairs; ++index)
    {
        SosGenome a;
        SosGenome b;
        Expect(SosGenomeFounder(SEED, 6000u + 2u * index, &a) &&
                   SosGenomeFounder(SEED, 6001u + 2u * index, &b),
               "unrelated genomes are made");
        const SosQ16 similarity = SosGenomeSimilarity(&a, &b);
        Expect(similarity > half - 8192 && similarity < half + 8192,
               "unrelated genomes share about half their alleles");
        sum += similarity;
    }
    const int64_t mean = sum / (int64_t)pairs;
    Expect(mean > half - 512 && mean < half + 512, "unrelated genomes average half");
}

/* Fixed points of heredity. Every platform and compiler must produce exactly
 * these genomes and values. */
static void TestGolden(void)
{
    SosGenome founder;
    Expect(SosGenomeFounder(SEED, 1u, &founder), "a founder is made");
    const uint64_t founderHash = SosGenomeHash(&founder);
    const SosQ16 height = SosGenomeTrait(&founder, SOS_GENOME_TRAIT_HEIGHT);
    const SosQ16 heightPercentile = SosGenomePercentile(&founder, SOS_GENOME_TRAIT_HEIGHT);
    SosGenome mother;
    SosGenome father;
    Expect(SosGenomeFounder(SEED, 10u, &mother) && SosGenomeFounder(SEED, 11u, &father),
           "golden parents are made");
    SosGenome child;
    Expect(SosGenomeInherit(&mother, &father, SEED, 100u, &child), "the golden child is made");
    const uint64_t childHash = SosGenomeHash(&child);
    const uint64_t expectedFounder = UINT64_C(0xFE06BC74072EB88D);
    const uint64_t expectedChild = UINT64_C(0x60233DCF4F1DCB90);
    const SosQ16 expectedHeight = 30720; /* 15 of 32 alleles, the golden founder's height */
    /* 15 alleles map to the binomial CDF's midpoint there. */
    const SosQ16 expectedHeightPercentile = 23865;
    if (founderHash != expectedFounder || childHash != expectedChild || height != expectedHeight ||
        heightPercentile != expectedHeightPercentile)
    {
        fprintf(stderr,
                "golden founder 0x%016" PRIX64 ", child 0x%016" PRIX64 ", height %" PRId32
                ", percentile %" PRId32 "\n",
                founderHash, childHash, height, heightPercentile);
        Expect(0, "the genome matches the reference");
    }
}

int main(void)
{
    TestFounder();
    TestInherit();
    TestTraits();
    TestPercentileMonotone();
    TestPercentileUniform();
    TestPercentileHeritability();
    TestDistribution();
    TestHeritability();
    TestMutationRate();
    TestCrossover();
    TestSimilarity();
    TestGolden();
    return 0;
}
