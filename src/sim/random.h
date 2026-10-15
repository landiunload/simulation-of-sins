#ifndef SOS_SIM_RANDOM_H
#define SOS_SIM_RANDOM_H

#include <stdint.h>

/* Deterministic randomness. Every random decision in the world draws from a
 * stream derived from the world seed and a stable stream number (a being, a
 * terrain feature), so the same seed and inputs replay the same history on
 * every machine. Streams are eight bytes: millions of beings can each own one.
 * The generator is SplitMix64. */

typedef struct SosRandom
{
    uint64_t state;
} SosRandom;

/* Mixes 64 bits into 64 well-distributed bits; equal inputs give equal
 * outputs everywhere. */
uint64_t SosHash64(uint64_t value);
uint64_t SosHashCombine(uint64_t seed, uint64_t value);

/* The stream `stream` of the world `seed`. Different streams of one seed and
 * the same stream of different seeds are independent for practical use. */
SosRandom SosRandomStream(uint64_t seed, uint64_t stream);

uint64_t SosRandomNext(SosRandom *random);
/* Uniform in [0, bound); zero when bound is zero. Unbiased. */
uint32_t SosRandomBelow(SosRandom *random, uint32_t bound);
/* Uniform in [minimum, maximum]; minimum when the range is empty. */
int64_t SosRandomRange(SosRandom *random, int64_t minimum, int64_t maximum);
/* True with probability numerator / denominator. */
int SosRandomChance(SosRandom *random, uint32_t numerator, uint32_t denominator);

#endif
