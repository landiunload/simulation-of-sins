#ifndef SOS_SIM_GENOME_H
#define SOS_SIM_GENOME_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Heredity.
 *
 * A being's nature is a diploid genome: two 256-bit haplotypes, one from the
 * mother and one from the father, each held as four 64-bit words. No gene is
 * stored per trait. A trait id hashes to a fixed set of 16 loci and its value
 * is the share of set alleles there, so the set of possible traits is
 * unbounded while a genome stays 64 bytes of plain data. A child draws a
 * haplotype recombined from each parent's two, so its traits land near its
 * parents' mean, every trait is bell-shaped across a population, and a world
 * of millions of beings needs only one genome per being. */

/* A haplotype is this many 64-bit words; the two together are a genome. */
#define SOS_GENOME_WORDS 4u
#define SOS_GENOME_HAPLOTYPE_BITS (SOS_GENOME_WORDS * 64u)
/* Alleles of a genome: both haplotypes. */
#define SOS_GENOME_ALLELES (2u * SOS_GENOME_HAPLOTYPE_BITS)
/* Loci a trait id selects, and the alleles they contribute. */
#define SOS_GENOME_TRAIT_LOCI 16u
#define SOS_GENOME_TRAIT_ALLELES (2u * SOS_GENOME_TRAIT_LOCI)
/* Crossover points a parent may put in one passed haplotype. */
#define SOS_GENOME_MIN_CROSSOVERS 1u
#define SOS_GENOME_MAX_CROSSOVERS 3u
/* Each bit of a passed haplotype flips with this chance. A haplotype is
 * SOS_GENOME_HAPLOTYPE_BITS bits, so this is about one mutation per haplotype
 * per generation: variation without destroying what selection kept. */
#define SOS_GENOME_MUTATION_ONE_IN SOS_GENOME_HAPLOTYPE_BITS

typedef struct SosGenome
{
    /* The haplotype from the mother, then the one from the father. */
    uint64_t maternal[SOS_GENOME_WORDS];
    uint64_t paternal[SOS_GENOME_WORDS];
} SosGenome;

/* Well-known traits. Any other id works too: the id only seeds which loci it
 * reads, so new traits need no new code and no change to a stored genome. */
typedef enum SosGenomeTraitId
{
    SOS_GENOME_TRAIT_HEIGHT = 0,
    SOS_GENOME_TRAIT_STRENGTH,
    SOS_GENOME_TRAIT_ENDURANCE,
    SOS_GENOME_TRAIT_INTELLIGENCE,
    SOS_GENOME_TRAIT_FERTILITY,
    SOS_GENOME_TRAIT_LONGEVITY,
    SOS_GENOME_TRAIT_TEMPERAMENT,
    SOS_GENOME_TRAIT_BEAUTY,
    SOS_GENOME_TRAIT_IMMUNITY,
    SOS_GENOME_TRAIT_CURIOSITY,
    /* The five personality factors consumers such as the mind and the
     * personality modules name instead of hard-coding numbers. */
    SOS_GENOME_TRAIT_OPENNESS,
    SOS_GENOME_TRAIT_CONSCIENTIOUSNESS,
    SOS_GENOME_TRAIT_EXTRAVERSION,
    SOS_GENOME_TRAIT_AGREEABLENESS,
    SOS_GENOME_TRAIT_NEUROTICISM,
    /* Predisposition to each of the seven deadly sins, read by the sins
     * module when it weighs a temptation. */
    SOS_GENOME_TRAIT_PRIDE,
    SOS_GENOME_TRAIT_GREED,
    SOS_GENOME_TRAIT_LUST,
    SOS_GENOME_TRAIT_ENVY,
    SOS_GENOME_TRAIT_GLUTTONY,
    SOS_GENOME_TRAIT_WRATH,
    SOS_GENOME_TRAIT_SLOTH,
    /* How briskly a being moves, so the body and movement modules share one
     * name for it. */
    SOS_GENOME_TRAIT_WALKING_SPEED,
    SOS_GENOME_TRAIT_COUNT,
} SosGenomeTraitId;

/* A random founder genome, a pure function of `seed` and `serial` so every
 * machine starts the same beings with the same nature. False when `out` is
 * NULL, which leaves it unchanged. */
bool SosGenomeFounder(uint64_t seed, uint64_t serial, SosGenome *out);

/* The genome of the child of `mother` and `father`: each passes one haplotype
 * recombined from its two with one to three crossover points, then mutated at
 * SOS_GENOME_MUTATION_ONE_IN. A pure function of the parents, `seed` and
 * `childSerial`. The two parents may be the same genome and `out` may alias
 * either of them. False when a parent or `out` is NULL, leaving `out`
 * unchanged. */
bool SosGenomeInherit(const SosGenome *mother, const SosGenome *father, uint64_t seed,
                      uint64_t childSerial, SosGenome *out);

/* The value of `traitId` in [0, SOS_Q16_ONE]: the share of set alleles among
 * the trait's SOS_GENOME_TRAIT_ALLELES alleles. A NULL genome gives zero. */
SosQ16 SosGenomeTrait(const SosGenome *genome, uint32_t traitId);

/* The population percentile of `traitId` in [0, SOS_Q16_ONE]: the trait's
 * allele count mapped through the exact binomial(SOS_GENOME_TRAIT_ALLELES,
 * half) CDF, so a founder
 * population spreads evenly instead of piling up at the bell's center, which
 * consumers of a trait as a 0..1 strength expect. Monotone in the allele
 * count. A NULL genome gives zero. */
SosQ16 SosGenomePercentile(const SosGenome *genome, uint32_t traitId);

/* The share of alleles that are equal, in [0, SOS_Q16_ONE]: one against the
 * same genome and about half between unrelated founders. The maternal
 * haplotypes are compared with each other and the paternal ones with each
 * other. A NULL genome gives zero. */
SosQ16 SosGenomeSimilarity(const SosGenome *a, const SosGenome *b);

/* Folds a genome into one value for replay and cross-machine checks. A NULL
 * genome gives zero. */
uint64_t SosGenomeHash(const SosGenome *genome);

#endif
