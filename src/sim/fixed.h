#ifndef SOS_SIM_FIXED_H
#define SOS_SIM_FIXED_H

#include <stdint.h>

/* Fractions in fixed point.
 *
 * Shares, chances and the strength of traits, feelings and ties are Q16
 * values: SOS_Q16_ONE stands for 1.0. Integer arithmetic gives every machine
 * the same world; floats belong to presentation. Division is used instead of
 * shifting because C fixes how division rounds negative numbers (toward zero)
 * but leaves shifting them to the compiler. */

#define SOS_Q16_BITS 16u
#define SOS_Q16_ONE (INT32_C(1) << SOS_Q16_BITS)

typedef int32_t SosQ16;

/* a * b. The product must fit in a Q16 value. */
static inline SosQ16 SosQ16Mul(SosQ16 a, SosQ16 b)
{
    return (SosQ16)((int64_t)a * b / SOS_Q16_ONE);
}

/* from + (to - from) * t: `from` at t = 0, `to` at SOS_Q16_ONE. */
static inline SosQ16 SosQ16Lerp(SosQ16 from, SosQ16 to, SosQ16 t)
{
    return (SosQ16)(from + ((int64_t)to - from) * t / SOS_Q16_ONE);
}

static inline SosQ16 SosQ16Clamp(SosQ16 value, SosQ16 minimum, SosQ16 maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

/* numerator / denominator, clamped to the Q16 range; zero when the
 * denominator is zero. Exact for |numerator| below 2^47. */
SosQ16 SosQ16Ratio(int64_t numerator, int64_t denominator);

/* `value` after `elapsed` ticks of halving every `halfLife` ticks: what is
 * left of a feeling, a memory or a claim nobody renewed. It moves toward zero
 * and never crosses it, and it is a function of the elapsed time alone, so a
 * value decayed in one long step equals one decayed in many short steps up
 * to rounding, and nothing has to be touched while time passes. A zero
 * half-life drops the value at once. */
int64_t SosDecay(int64_t value, uint64_t elapsed, uint64_t halfLife);

/* What is left at `now` of a value last set at `since`: the lazy read of
 * every fading record. A `now` before `since` leaves the value unchanged, so a
 * caller with a stale tick never makes anything grow. */
static inline int64_t SosDecaySince(int64_t value, uint64_t since, uint64_t now, uint64_t halfLife)
{
    return now > since ? SosDecay(value, now - since, halfLife) : value;
}

/* a + b clamped to [minimum, maximum], without overflow for any inputs. */
static inline SosQ16 SosQ16AddClamped(SosQ16 a, SosQ16 b, SosQ16 minimum, SosQ16 maximum)
{
    const int64_t sum = (int64_t)a + b;
    return sum < minimum ? minimum : (sum > maximum ? maximum : (SosQ16)sum);
}

#endif
