#include "sim/names.h"

#include "sim/random.h"

#include <stddef.h>
#include <string.h>

/* Streams of one seed. Fixed, distinct numbers keep a language's sounds, its
 * words, each kind of name and its daughters independent of one another. */
#define LANGUAGE_STREAM UINT64_C(0x6e616d6573) /* "names" */
#define WORD_STREAM UINT64_C(0x776f7264)       /* "word" */
#define DRIFT_STREAM UINT64_C(0x6472696674)    /* "drift" */
#define PERSONAL_STREAM UINT64_C(0x70657273)   /* "pers" */
#define FAMILY_STREAM UINT64_C(0x66616d73)     /* "fams" */
#define GROUP_STREAM UINT64_C(0x67727073)      /* "grps" */
#define PLACE_STREAM UINT64_C(0x706c6373)      /* "plcs" */

/* How many sounds a fresh language takes. Broad enough that two seeds rarely
 * overlap much, narrow enough that every word stays pronounceable. */
#define MIN_CONSONANTS 5u
#define MAX_CONSONANTS 13u
#define MIN_VOWELS 3u
#define MAX_VOWELS 6u
#define MIN_SHAPES 2u
#define MAX_SHAPES 5u

/* Enough for the widest table, so a mask can be built without allocating. */
#define MAX_PHONEMES SOS_NAMES_CONSONANT_COUNT

/* The longest syllable a shape can spell, "CLVC" of digraphs. */
#define MAX_SYLLABLE_CHARS 8u

/* Letters in the longest single phoneme, the digraphs. */
#define PHONEME_MAX_CHARS 2u

/* How many small shifts a daughter may differ by, and how many kinds of
 * shift exist. Keeping the number small is what makes daughters resemble
 * their parent. */
#define MAX_DRIFT_CHANGES 3u
#define DRIFT_KINDS 6u

typedef struct Phoneme
{
    /* One or two ASCII letters, a single sound; up to two so that digraphs
     * like "th" count as one consonant. */
    char text[3];
} Phoneme;

/* Order is the identity of a consonant: bit i of a mask names entry i. */
static const Phoneme consonantTable[SOS_NAMES_CONSONANT_COUNT] = {
    {"b"}, {"c"},  {"d"},  {"f"},  {"g"},  {"h"},  {"j"},  {"k"},  {"l"},
    {"m"}, {"n"},  {"p"},  {"r"},  {"s"},  {"t"},  {"v"},  {"w"},  {"x"},
    {"z"}, {"th"}, {"sh"}, {"ch"}, {"ph"}, {"wh"}, {"ng"}, {"ck"},
};

static const Phoneme vowelTable[SOS_NAMES_VOWEL_COUNT] = {
    {"a"},  {"e"},  {"i"},  {"o"},  {"u"},  {"y"},  {"ai"},
    {"ea"}, {"ee"}, {"oo"}, {"ou"}, {"ie"}, {"ay"}, {"oy"},
};

typedef struct SyllableShape
{
    /* A pattern of slots: 'C' any consonant, 'L' a liquid or glide that
     * follows one, 'V' a vowel. Up to four slots. */
    char pattern[5];
} SyllableShape;

static const SyllableShape shapeTable[SOS_NAMES_SHAPE_COUNT] = {
    {"V"}, {"CV"}, {"VC"}, {"CVC"}, {"CLV"}, {"CLVC"},
};

/* The liquids and glides l, r, w: the only sounds that may follow a consonant
 * inside a syllable, so clusters stay pronounceable. */
#define ONSET_TAIL_MASK ((UINT32_C(1) << 8) | (UINT32_C(1) << 12) | (UINT32_C(1) << 16))

/* Bits set in `mask`. */
static uint32_t BitCount(uint32_t mask)
{
    uint32_t count = 0u;
    while (mask != 0u)
    {
        mask &= mask - 1u;
        ++count;
    }
    return count;
}

/* The table index of the `index`-th set bit, which the caller guarantees is
 * in range. */
static uint32_t NthBit(uint32_t mask, uint32_t index)
{
    for (uint32_t bit = 0u; bit < 32u; ++bit)
    {
        if ((mask & (UINT32_C(1) << bit)) != 0u)
        {
            if (index == 0u)
            {
                return bit;
            }
            --index;
        }
    }
    return 0u; /* unreachable: callers pass a valid index */
}

/* One phoneme of `mask`, or NULL when the mask is empty. */
static const char *PickPhoneme(SosRandom *random, uint32_t mask, const Phoneme *table)
{
    const uint32_t count = BitCount(mask);
    if (count == 0u)
    {
        return NULL;
    }
    return table[NthBit(mask, SosRandomBelow(random, count))].text;
}

/* A mask of `minimum`..`maximum` distinct bits below `universe`, drawn
 * uniformly: a partial Fisher-Yates shuffle picks the wanted bits without
 * ever repeating one. */
static uint32_t RandomMask(SosRandom *random, uint32_t universe, uint32_t minimum, uint32_t maximum)
{
    uint8_t indices[MAX_PHONEMES];
    for (uint32_t slot = 0u; slot < universe; ++slot)
    {
        indices[slot] = (uint8_t)slot;
    }
    const uint32_t span = maximum - minimum + 1u;
    const uint32_t count = minimum + SosRandomBelow(random, span);
    uint32_t mask = 0u;
    for (uint32_t slot = 0u; slot < count; ++slot)
    {
        const uint32_t pick = slot + SosRandomBelow(random, universe - slot);
        const uint8_t swap = indices[slot];
        indices[slot] = indices[pick];
        indices[pick] = swap;
        mask |= UINT32_C(1) << indices[slot];
    }
    return mask;
}

static bool IsAsciiLower(unsigned char value)
{
    return value >= (unsigned char)'a' && value <= (unsigned char)'z';
}

/* Appends one phoneme's letters, or nothing when they would overrun the
 * ending; a caller never builds an ending that long. */
static void AppendEnding(char *out, size_t *length, const char *text)
{
    for (uint32_t index = 0u; index < PHONEME_MAX_CHARS; ++index)
    {
        if (text[index] == '\0' || *length >= SOS_NAMES_ENDING_CHARS)
        {
            return;
        }
        out[*length] = text[index];
        ++*length;
    }
}

/* A short ending for one sex, built from the language's own sounds and always
 * opening on a vowel, so it joins the stem smoothly. A woman's name trails
 * off in a vowel, a man's closes on a consonant, so the two names of a pair
 * belong to one language yet sound distinct. */
static void BuildEnding(SosRandom *random, const SosLanguage *lang, SosSex sex, char *out)
{
    size_t length = 0u;
    AppendEnding(out, &length, PickPhoneme(random, lang->vowelMask, vowelTable));
    if (sex == SOS_SEX_FEMALE)
    {
        if (SosRandomBelow(random, 2u) == 0u)
        {
            AppendEnding(out, &length, PickPhoneme(random, lang->vowelMask, vowelTable));
        }
    }
    else
    {
        AppendEnding(out, &length, PickPhoneme(random, lang->consonantMask, consonantTable));
    }
    out[length] = '\0';
}

/* A language is usable only when every sound set is present, the syllable
 * count is in range and both endings are short lowercase words. This catches
 * a caller's hand-built or corrupted struct before it can be read out of
 * bounds. */
static bool EndingValid(const char *ending)
{
    size_t length = 0u;
    while (length <= SOS_NAMES_ENDING_CHARS && ending[length] != '\0')
    {
        if (!IsAsciiLower((unsigned char)ending[length]))
        {
            return false;
        }
        ++length;
    }
    return length > 0u && length <= SOS_NAMES_ENDING_CHARS;
}

static bool LanguageValid(const SosLanguage *lang)
{
    return lang->consonantMask != 0u && lang->vowelMask != 0u && lang->shapeMask != 0u &&
           lang->syllables >= 1u && lang->syllables <= SOS_NAMES_MAX_SYLLABLES &&
           EndingValid(lang->femaleEnding) && EndingValid(lang->maleEnding);
}

void SosLanguageFromSeed(uint64_t seed, SosLanguage *out)
{
    if (out == NULL)
    {
        return;
    }
    SosRandom random = SosRandomStream(seed, LANGUAGE_STREAM);
    SosLanguage lang = {0};
    lang.seed = seed;
    lang.consonantMask =
        RandomMask(&random, SOS_NAMES_CONSONANT_COUNT, MIN_CONSONANTS, MAX_CONSONANTS);
    lang.vowelMask = RandomMask(&random, SOS_NAMES_VOWEL_COUNT, MIN_VOWELS, MAX_VOWELS);
    lang.shapeMask = RandomMask(&random, SOS_NAMES_SHAPE_COUNT, MIN_SHAPES, MAX_SHAPES);
    lang.syllables = 1u + SosRandomBelow(&random, SOS_NAMES_MAX_SYLLABLES);
    BuildEnding(&random, &lang, SOS_SEX_FEMALE, lang.femaleEnding);
    BuildEnding(&random, &lang, SOS_SEX_MALE, lang.maleEnding);
    *out = lang;
}

/* `preferred` syllables, usually, sometimes one less or more. */
static uint32_t ChooseSyllables(SosRandom *random, uint32_t preferred)
{
    uint32_t count = preferred;
    if (count < 1u)
    {
        count = 1u;
    }
    if (count > SOS_NAMES_MAX_SYLLABLES)
    {
        count = SOS_NAMES_MAX_SYLLABLES;
    }
    const uint32_t roll = SosRandomBelow(random, 4u);
    if (roll == 0u && count > 1u)
    {
        --count;
    }
    else if (roll == 3u && count < SOS_NAMES_MAX_SYLLABLES)
    {
        ++count;
    }
    return count;
}

/* A word under construction. Syllables are added whole, so a name that does
 * not fit is cut on a syllable boundary and never mid-syllable. */
typedef struct WordBuilder
{
    char *out;
    size_t capacity;
    size_t length;
} WordBuilder;

/* Appends `text` as one unit, or leaves the builder unchanged when it would
 * not fit with room for the NUL. */
static bool BuilderAppend(WordBuilder *builder, const char *text)
{
    const size_t textLength = strlen(text);
    if (textLength + 1u > builder->capacity - builder->length)
    {
        return false;
    }
    memcpy(builder->out + builder->length, text, textLength);
    builder->length += textLength;
    return true;
}

static bool AppendSyllable(WordBuilder *builder, const SosLanguage *lang, SosRandom *random)
{
    const uint32_t shapeCount = BitCount(lang->shapeMask);
    if (shapeCount == 0u)
    {
        return false;
    }
    const char *pattern =
        shapeTable[NthBit(lang->shapeMask, SosRandomBelow(random, shapeCount))].pattern;
    char syllable[MAX_SYLLABLE_CHARS + 1u];
    size_t length = 0u;
    for (const char *slot = pattern; *slot != '\0'; ++slot)
    {
        uint32_t mask = lang->consonantMask;
        const Phoneme *table = consonantTable;
        if (*slot == 'V')
        {
            mask = lang->vowelMask;
            table = vowelTable;
        }
        else if (*slot == 'L')
        {
            mask = lang->consonantMask & ONSET_TAIL_MASK;
            if (mask == 0u)
            {
                mask = lang->consonantMask;
            }
        }
        const char *text = PickPhoneme(random, mask, table);
        if (text == NULL)
        {
            return false;
        }
        const size_t textLength = strlen(text);
        if (length + textLength > MAX_SYLLABLE_CHARS)
        {
            return false;
        }
        memcpy(syllable + length, text, textLength);
        length += textLength;
    }
    syllable[length] = '\0';
    return BuilderAppend(builder, syllable);
}

static char UpperAscii(char letter)
{
    const unsigned char value = (unsigned char)letter;
    if (IsAsciiLower(value))
    {
        return (char)(value - (unsigned char)'a' + (unsigned char)'A');
    }
    return letter;
}

/* Builds `seed`'s name in `lang`: `preferredSyllables` syllables plus the
 * fixed `suffix` (may be NULL), each added whole. */
static bool ComposeWord(const SosLanguage *lang, uint64_t nameSeed, uint64_t stream,
                        uint32_t preferredSyllables, const char *suffix, bool capitalized,
                        char *out, size_t capacity)
{
    SosRandom random = SosRandomStream(SosHashCombine(lang->seed, nameSeed), stream);
    const uint32_t syllables = ChooseSyllables(&random, preferredSyllables);
    WordBuilder builder = {.out = out, .capacity = capacity, .length = 0u};
    for (uint32_t index = 0u; index < syllables; ++index)
    {
        if (!AppendSyllable(&builder, lang, &random))
        {
            break;
        }
    }
    if (suffix != NULL)
    {
        (void)BuilderAppend(&builder, suffix);
    }
    out[builder.length] = '\0';
    if (capitalized && builder.length > 0u)
    {
        out[0] = UpperAscii(out[0]);
    }
    return builder.length > 0u;
}

bool SosLanguageWord(const SosLanguage *lang, uint64_t wordSeed, bool capitalized, char *out,
                     size_t capacity)
{
    if (lang == NULL || out == NULL || capacity == 0u || !LanguageValid(lang))
    {
        return false;
    }
    return ComposeWord(lang, wordSeed, WORD_STREAM, lang->syllables, NULL, capitalized, out,
                       capacity);
}

bool SosNamesPersonal(const SosLanguage *lang, uint64_t seed, uint64_t serial, SosSex sex,
                      char *out, size_t capacity)
{
    if (lang == NULL || out == NULL || capacity == 0u || serial == 0u || !LanguageValid(lang))
    {
        return false;
    }
    /* A short stem and the sex ending: a personal name is rarely as long as a
     * plain word. */
    const uint32_t stemSyllables = lang->syllables < 2u ? 1u : 2u;
    const char *ending = sex == SOS_SEX_MALE ? lang->maleEnding : lang->femaleEnding;
    return ComposeWord(lang, SosHashCombine(seed, serial), PERSONAL_STREAM, stemSyllables, ending,
                       true, out, capacity);
}

bool SosNamesFamily(const SosLanguage *lang, uint64_t founderSerial, char *out, size_t capacity)
{
    if (lang == NULL || out == NULL || capacity == 0u || founderSerial == 0u ||
        !LanguageValid(lang))
    {
        return false;
    }
    return ComposeWord(lang, founderSerial, FAMILY_STREAM, lang->syllables, NULL, true, out,
                       capacity);
}

bool SosNamesGroup(const SosLanguage *lang, uint64_t groupId, char *out, size_t capacity)
{
    if (lang == NULL || out == NULL || capacity == 0u || groupId == 0u || !LanguageValid(lang))
    {
        return false;
    }
    return ComposeWord(lang, groupId, GROUP_STREAM, lang->syllables, NULL, true, out, capacity);
}

bool SosNamesPlace(const SosLanguage *lang, int64_t cellX, int64_t cellY, char *out,
                   size_t capacity)
{
    if (lang == NULL || out == NULL || capacity == 0u || !LanguageValid(lang))
    {
        return false;
    }
    const uint64_t nameSeed = SosHashCombine((uint64_t)cellX, (uint64_t)cellY);
    return ComposeWord(lang, nameSeed, PLACE_STREAM, lang->syllables, NULL, true, out, capacity);
}

/* Moves one set bit of `mask` to a neighbouring index: a sound shifts a
 * little, as a speaker of the daughter tongue would drift it. The mask stays
 * non-empty: a lone sound may only move, never disappear. */
static void ShiftMask(SosRandom *random, uint32_t *mask, uint32_t universe)
{
    const uint32_t count = BitCount(*mask);
    if (count == 0u)
    {
        return;
    }
    const uint32_t from = NthBit(*mask, SosRandomBelow(random, count));
    uint32_t to;
    if (from == 0u)
    {
        to = 1u;
    }
    else if (from + 1u >= universe || SosRandomBelow(random, 2u) == 0u)
    {
        to = from - 1u;
    }
    else
    {
        to = from + 1u;
    }
    *mask &= ~(UINT32_C(1) << from);
    *mask |= UINT32_C(1) << to;
}

static void AdjustSyllables(SosRandom *random, SosLanguage *lang)
{
    if (lang->syllables <= 1u)
    {
        lang->syllables = 2u;
    }
    else if (lang->syllables >= SOS_NAMES_MAX_SYLLABLES)
    {
        lang->syllables = SOS_NAMES_MAX_SYLLABLES - 1u;
    }
    else if (SosRandomBelow(random, 2u) == 0u)
    {
        ++lang->syllables;
    }
    else
    {
        --lang->syllables;
    }
}

static void ApplyDriftKind(SosRandom *random, SosLanguage *lang, uint32_t kind)
{
    switch (kind)
    {
    case 0u:
        ShiftMask(random, &lang->consonantMask, SOS_NAMES_CONSONANT_COUNT);
        break;
    case 1u:
        ShiftMask(random, &lang->vowelMask, SOS_NAMES_VOWEL_COUNT);
        break;
    case 2u:
        ShiftMask(random, &lang->shapeMask, SOS_NAMES_SHAPE_COUNT);
        break;
    case 3u:
        AdjustSyllables(random, lang);
        break;
    case 4u:
        BuildEnding(random, lang, SOS_SEX_FEMALE, lang->femaleEnding);
        break;
    default:
        BuildEnding(random, lang, SOS_SEX_MALE, lang->maleEnding);
        break;
    }
}

void SosLanguageDrift(const SosLanguage *parent, uint64_t seed, SosLanguage *out)
{
    if (parent == NULL || out == NULL)
    {
        return;
    }
    SosLanguage lang = *parent;
    SosRandom random = SosRandomStream(SosHashCombine(parent->seed, seed), DRIFT_STREAM);
    /* Shuffle the kinds and take the first few, so each kind applies at most
     * once: no sound set is shifted twice and the daughter cannot wander far
     * from the parent. */
    uint32_t order[DRIFT_KINDS];
    for (uint32_t kind = 0u; kind < DRIFT_KINDS; ++kind)
    {
        order[kind] = kind;
    }
    for (uint32_t slot = 0u; slot < DRIFT_KINDS; ++slot)
    {
        const uint32_t pick = slot + SosRandomBelow(&random, DRIFT_KINDS - slot);
        const uint32_t swap = order[slot];
        order[slot] = order[pick];
        order[pick] = swap;
    }
    const uint32_t changes = 1u + SosRandomBelow(&random, MAX_DRIFT_CHANGES);
    for (uint32_t index = 0u; index < changes; ++index)
    {
        ApplyDriftKind(&random, &lang, order[index]);
    }
    *out = lang;
}
