#ifndef SOS_SIM_NAMES_H
#define SOS_SIM_NAMES_H

#include "sim/biology.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Procedural languages and names.
 *
 * A language is a compact description of how its people sound: which
 * consonants and vowels they use (some of them two-letter digraphs), which
 * syllable shapes they allow, how long their words tend to be and the
 * endings their women's and men's names take. From that description every
 * word follows deterministically from a seed, so the unbounded world never
 * stores a name: a being, a family line, a group or a place is named by
 * hashing its identity into the language. Names are ASCII letters only;
 * presentation may render them in another script later.
 *
 * A daughter language is the mother tongue with a few small sound changes,
 * so related languages still sound alike while drifting apart over time.
 *
 * Sound is a subset of a fixed phoneme table: bit i of a mask selects entry i
 * of the matching private table. The tables are private to the module; the
 * masks are public so tools and tests can compare two languages by their
 * sounds. */

#define SOS_NAMES_CONSONANT_COUNT 26u
#define SOS_NAMES_VOWEL_COUNT 14u
#define SOS_NAMES_SHAPE_COUNT 6u

/* Words run from 1 to this many syllables. */
#define SOS_NAMES_MAX_SYLLABLES 4u
/* Longest name written here, in characters, excluding the NUL. */
#define SOS_NAMES_MAX_NAME_CHARS 32u
/* Longest sex-specific ending, in characters. */
#define SOS_NAMES_ENDING_CHARS 4u

typedef struct SosLanguage
{
    /* The seed the language grew from; every name derives from it. */
    uint64_t seed;
    /* Subsets of the consonant and vowel tables: bit i selects entry i. */
    uint32_t consonantMask;
    uint32_t vowelMask;
    /* Allowed syllable shapes: bit i selects entry i of the shape table. */
    uint32_t shapeMask;
    /* Preferred number of syllables, 1..SOS_NAMES_MAX_SYLLABLES. */
    uint32_t syllables;
    /* Sex-specific name endings; non-empty and drawn from the language, the
     * female one ending in a vowel and the male one in a consonant. */
    char femaleEnding[SOS_NAMES_ENDING_CHARS + 1u];
    char maleEnding[SOS_NAMES_ENDING_CHARS + 1u];
} SosLanguage;

/* Builds the language of `seed`. Every seed is valid. No-op when `out` is
 * NULL. */
void SosLanguageFromSeed(uint64_t seed, SosLanguage *out);

/* A daughter of `parent`, changed by the sound shifts of `seed`. It keeps the
 * parent's sounds except for a few local changes, so a family of languages
 * grows recognisably apart. The result is always a valid language: no sound
 * set becomes empty. `out` may alias `parent`. No-op when `parent` or `out`
 * is NULL. */
void SosLanguageDrift(const SosLanguage *parent, uint64_t seed, SosLanguage *out);

/* A pronounceable word of 1..SOS_NAMES_MAX_SYLLABLES syllables. The first
 * letter is upper case when `capitalized`. Writes a NUL-terminated string
 * that never exceeds `capacity`; when the word does not fit whole it stops at
 * a syllable boundary. Returns false, with an empty string when there is
 * room, when `lang` or `out` is NULL, `capacity` is zero, the language is
 * invalid, or not even one syllable fits. */
bool SosLanguageWord(const SosLanguage *lang, uint64_t wordSeed, bool capitalized, char *out,
                     size_t capacity);

/* A personal name of `serial` in `lang`, drawn from the world `seed` and
 * ending in the sex-specific ending. False for NULL, zero capacity, an
 * invalid language or `serial` 0 (nobody). */
bool SosNamesPersonal(const SosLanguage *lang, uint64_t seed, uint64_t serial, SosSex sex,
                      char *out, size_t capacity);

/* The family name every descendant of `founderSerial` shares: it depends on
 * the founder alone, never on the descendant. False for NULL, zero capacity,
 * an invalid language or founder 0 (no line). */
bool SosNamesFamily(const SosLanguage *lang, uint64_t founderSerial, char *out, size_t capacity);

/* A name for the group `groupId`. False for NULL, zero capacity, an invalid
 * language or id 0 (no group). */
bool SosNamesGroup(const SosLanguage *lang, uint64_t groupId, char *out, size_t capacity);

/* A name for the place at the cell (cellX, cellY). Every cell is valid,
 * including negative and extreme ones. False for NULL, zero capacity or an
 * invalid language. */
bool SosNamesPlace(const SosLanguage *lang, int64_t cellX, int64_t cellY, char *out,
                   size_t capacity);

#endif
