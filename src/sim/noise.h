#ifndef SOS_SIM_NOISE_H
#define SOS_SIM_NOISE_H

#include "sim/fixed.h"

#include <stdint.h>

/* Integer value noise.
 *
 * Terrain height, caves and the climate all read the same kind of smooth
 * pseudo-random field: a pure function of a seed and a position computed from
 * integers alone, so every machine derives an identical unbounded world and
 * nothing has to be stored. The field is value noise on a lattice whose cells
 * are 2^shift world units wide. Each lattice corner holds a pseudo-random
 * value in [-SOS_Q16_ONE, SOS_Q16_ONE) derived from the seed and the corner,
 * and the corners are blended with a smoothstep, so the field and its slope
 * stay continuous. `shift` is therefore the wavelength: a small shift gives
 * fine detail, a large one broad regions. Two dimensions are a z = 0 slice of
 * the three-dimensional field. */

/* Value noise at (x, y) in world units, as a Q16 value in
 * [-SOS_Q16_ONE, SOS_Q16_ONE) (SOS_Q16_ONE is 1.0). `seed` picks the field
 * within a world; `shift` is the lattice wavelength in world units. Equal
 * inputs give equal outputs on every machine. Returns 0 when `shift` exceeds
 * 63, where the lattice masks would no longer fit a 64-bit word. */
int64_t SosNoise2(uint64_t seed, int64_t x, int64_t y, unsigned shift);

/* The three-dimensional form of SosNoise2: value noise at (x, y, z) as a Q16
 * value in [-SOS_Q16_ONE, SOS_Q16_ONE), with the same contract. */
int64_t SosNoise3(uint64_t seed, int64_t x, int64_t y, int64_t z, unsigned shift);

#endif
