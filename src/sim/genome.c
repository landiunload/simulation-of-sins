#include "sim/genome.h"

#include "sim/random.h"

/* Seeds that keep the streams one serial drives apart, so a founder genome and
 * a child's inheritance never draw the same numbers. */
#define SOS_GENOME_FOUNDER_STREAM UINT64_C(0x666F756E646572) /* "founder" */
#define SOS_GENOME_INHERIT_STREAM UINT64_C(0x696E6865726974) /* "inherit" */
#define SOS_GENOME_TRAIT_STREAM UINT64_C(0x7472616974)       /* "trait" */
#define SOS_GENOME_HASH_SEED UINT64_C(0x67656E6F6D65)        /* "genome" */

/* Population count of a 64-bit word, the SWAR way: identical everywhere and
 * no builtin or platform header. */
static uint32_t CountBits(uint64_t value)
{
    value -= (value >> 1u) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2u) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4u)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (uint32_t)((value * UINT64_C(0x0101010101010101)) >> 56u);
}

static bool Allele(const uint64_t haplotype[SOS_GENOME_WORDS], uint32_t position)
{
    return ((haplotype[position >> 6u] >> (position & 63u)) & 1u) != 0u;
}

/* Fills out[0..count) with count distinct values in [0, bound), sorted, drawn
 * from `random`. Because choosing from a range that shrinks by one per pick
 * and then bumping past the values already taken never rejects, every bit of
 * a haplotype can be a locus and the loop always ends. count <= bound. */
static void SelectDistinct(SosRandom *random, uint32_t bound, uint32_t count, uint32_t *out)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t value = SosRandomBelow(random, bound - index);
        for (uint32_t earlier = 0u; earlier < index; ++earlier)
        {
            if (out[earlier] <= value)
            {
                ++value;
            }
        }
        /* Keep the values picked so far sorted, so the bumps above place
         * `value` exactly one past every taken value below it. */
        uint32_t position = index;
        while (position > 0u && out[position - 1u] > value)
        {
            out[position] = out[position - 1u];
            --position;
        }
        out[position] = value;
    }
}

/* One haplotype a parent passes: a mosaic of its two haplotypes, switching at
 * one to three crossover points, then each bit flipped at the mutation rate.
 * The switch points are in [1, bits), so bit zero always keeps the starting
 * haplotype and the first segment is never empty. `out` must not alias either
 * input; SosGenomeInherit passes a local buffer so a caller may alias. */
static void Recombine(const uint64_t maternal[SOS_GENOME_WORDS],
                      const uint64_t paternal[SOS_GENOME_WORDS], SosRandom *random,
                      uint64_t out[SOS_GENOME_WORDS])
{
    for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
    {
        out[word] = 0u;
    }
    const uint32_t crossovers =
        SOS_GENOME_MIN_CROSSOVERS +
        SosRandomBelow(random, SOS_GENOME_MAX_CROSSOVERS - SOS_GENOME_MIN_CROSSOVERS + 1u);
    uint32_t points[SOS_GENOME_MAX_CROSSOVERS];
    SelectDistinct(random, SOS_GENOME_HAPLOTYPE_BITS - 1u, crossovers, points);
    for (uint32_t point = 0u; point < crossovers; ++point)
    {
        points[point] += 1u;
    }
    const uint64_t *current = SosRandomBelow(random, 2u) == 0u ? maternal : paternal;
    const uint64_t *alternate = current == maternal ? paternal : maternal;
    uint32_t nextPoint = 0u;
    for (uint32_t bit = 0u; bit < SOS_GENOME_HAPLOTYPE_BITS; ++bit)
    {
        if (nextPoint < crossovers && points[nextPoint] == bit)
        {
            const uint64_t *swap = current;
            current = alternate;
            alternate = swap;
            ++nextPoint;
        }
        bool set = Allele(current, bit);
        if (SosRandomChance(random, 1u, SOS_GENOME_MUTATION_ONE_IN) != 0)
        {
            set = !set;
        }
        if (set)
        {
            out[bit >> 6u] |= UINT64_C(1) << (bit & 63u);
        }
    }
}

bool SosGenomeFounder(uint64_t seed, uint64_t serial, SosGenome *out)
{
    if (out == NULL)
    {
        return false;
    }
    SosRandom random = SosRandomStream(SosHashCombine(seed, SOS_GENOME_FOUNDER_STREAM), serial);
    for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
    {
        out->maternal[word] = SosRandomNext(&random);
        out->paternal[word] = SosRandomNext(&random);
    }
    return true;
}

bool SosGenomeInherit(const SosGenome *mother, const SosGenome *father, uint64_t seed,
                      uint64_t childSerial, SosGenome *out)
{
    if (mother == NULL || father == NULL || out == NULL)
    {
        return false;
    }
    SosRandom random =
        SosRandomStream(SosHashCombine(seed, SOS_GENOME_INHERIT_STREAM), childSerial);
    /* Built in a local so that `out` may alias either parent. */
    SosGenome child;
    Recombine(mother->maternal, mother->paternal, &random, child.maternal);
    Recombine(father->maternal, father->paternal, &random, child.paternal);
    *out = child;
    return true;
}

/* How many of a trait's alleles are set: the shared kernel of the trait value
 * and its percentile, so the two can never read different loci. */
static uint32_t TraitAlleles(const SosGenome *genome, uint32_t traitId)
{
    SosRandom random = SosRandomStream(traitId, SOS_GENOME_TRAIT_STREAM);
    uint32_t loci[SOS_GENOME_TRAIT_LOCI];
    SelectDistinct(&random, SOS_GENOME_HAPLOTYPE_BITS, SOS_GENOME_TRAIT_LOCI, loci);
    uint32_t set = 0u;
    for (uint32_t locus = 0u; locus < SOS_GENOME_TRAIT_LOCI; ++locus)
    {
        set += (uint32_t)Allele(genome->maternal, loci[locus]);
        set += (uint32_t)Allele(genome->paternal, loci[locus]);
    }
    return set;
}

SosQ16 SosGenomeTrait(const SosGenome *genome, uint32_t traitId)
{
    if (genome == NULL)
    {
        return 0;
    }
    return SosQ16Ratio((int64_t)TraitAlleles(genome, traitId), (int64_t)SOS_GENOME_TRAIT_ALLELES);
}

/* Mid-rank percentiles of the binomial(SOS_GENOME_TRAIT_ALLELES, half) allele
 * count a random founder shows: entry k is (sum of ways below k plus half the
 * ways at k) / 2^32, the midpoint of the CDF jump at k, rounded down in Q16.
 * Mapping a count through it spreads a founder population near-uniformly over
 * [0, ONE]; the table holds the exact values so every machine agrees and no
 * runtime CDF is needed. */
static const SosQ16 traitPercentile[SOS_GENOME_TRAIT_ALLELES + 1u] = {
    0,     0,     0,     0,     0,     2,     10,    43,    149,   443,   1149,
    2626,  5333,  9706,  15953, 23865, 32768, 41670, 49582, 55829, 60202, 62909,
    64386, 65092, 65386, 65492, 65525, 65533, 65535, 65535, 65535, 65535, 65535,
};

SosQ16 SosGenomePercentile(const SosGenome *genome, uint32_t traitId)
{
    if (genome == NULL)
    {
        return 0;
    }
    return traitPercentile[TraitAlleles(genome, traitId)];
}

SosQ16 SosGenomeSimilarity(const SosGenome *a, const SosGenome *b)
{
    if (a == NULL || b == NULL)
    {
        return 0;
    }
    uint32_t equal = 0u;
    for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
    {
        equal += 64u - CountBits(a->maternal[word] ^ b->maternal[word]);
        equal += 64u - CountBits(a->paternal[word] ^ b->paternal[word]);
    }
    return SosQ16Ratio((int64_t)equal, (int64_t)SOS_GENOME_ALLELES);
}

uint64_t SosGenomeHash(const SosGenome *genome)
{
    if (genome == NULL)
    {
        return 0u;
    }
    uint64_t hash = SOS_GENOME_HASH_SEED;
    for (uint32_t word = 0u; word < SOS_GENOME_WORDS; ++word)
    {
        hash = SosHashCombine(hash, genome->maternal[word]);
        hash = SosHashCombine(hash, genome->paternal[word]);
    }
    return hash;
}
