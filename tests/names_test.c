#include "sim/names.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "names test failed: %s\n", message);
        exit(1);
    }
}

static uint32_t PopCount(uint32_t mask)
{
    uint32_t count = 0u;
    while (mask != 0u)
    {
        mask &= mask - 1u;
        ++count;
    }
    return count;
}

static bool LettersOnly(const char *text)
{
    for (const char *letter = text; *letter != '\0'; ++letter)
    {
        if (!((*letter >= 'a' && *letter <= 'z') || (*letter >= 'A' && *letter <= 'Z')))
        {
            return false;
        }
    }
    return true;
}

static bool IsVowelLetter(char letter)
{
    return letter == 'a' || letter == 'e' || letter == 'i' || letter == 'o' || letter == 'u' ||
           letter == 'y';
}

/* A language is usable: every sound set is present, the length is in range
 * and the endings are short lowercase words with the right final sound. */
static void ExpectLanguageValid(const SosLanguage *lang)
{
    Expect(lang->consonantMask != 0u && PopCount(lang->consonantMask) >= 5u, "language consonants");
    Expect(lang->vowelMask != 0u && PopCount(lang->vowelMask) >= 3u, "language vowels");
    Expect(lang->shapeMask != 0u, "language shapes");
    Expect(lang->syllables >= 1u && lang->syllables <= SOS_NAMES_MAX_SYLLABLES, "language length");
    Expect(LettersOnly(lang->femaleEnding) && LettersOnly(lang->maleEnding), "endings are letters");
    const size_t femaleLength = strlen(lang->femaleEnding);
    const size_t maleLength = strlen(lang->maleEnding);
    Expect(femaleLength > 0u && maleLength > 0u && femaleLength <= SOS_NAMES_ENDING_CHARS &&
               maleLength <= SOS_NAMES_ENDING_CHARS,
           "endings are short and non-empty");
    Expect(IsVowelLetter(lang->femaleEnding[femaleLength - 1u]), "a female ending ends in a vowel");
    Expect(!IsVowelLetter(lang->maleEnding[maleLength - 1u]), "a male ending ends in a consonant");
}

static void TestLanguageFromSeed(void)
{
    SosLanguage first;
    SosLanguage again;
    SosLanguageFromSeed(SEED, &first);
    SosLanguageFromSeed(SEED, &again);
    Expect(first.seed == again.seed && first.consonantMask == again.consonantMask &&
               first.vowelMask == again.vowelMask && first.shapeMask == again.shapeMask &&
               first.syllables == again.syllables &&
               strcmp(first.femaleEnding, again.femaleEnding) == 0 &&
               strcmp(first.maleEnding, again.maleEnding) == 0,
           "the same seed gives the same language");
    SosLanguageFromSeed(SEED + 1u, &again);
    Expect(again.consonantMask != first.consonantMask || again.vowelMask != first.vowelMask ||
               again.shapeMask != first.shapeMask || again.syllables != first.syllables ||
               strcmp(again.femaleEnding, first.femaleEnding) != 0 ||
               strcmp(again.maleEnding, first.maleEnding) != 0,
           "different seeds differ");
    /* NULL is harmless. */
    SosLanguageFromSeed(SEED, NULL);

    for (uint64_t seed = 0u; seed < 200u; ++seed)
    {
        SosLanguage lang;
        SosLanguageFromSeed(seed, &lang);
        Expect(lang.seed == seed, "a language remembers its seed");
        ExpectLanguageValid(&lang);
    }
}

static void TestWords(void)
{
    SosLanguage lang;
    SosLanguageFromSeed(SEED, &lang);
    char buffer[SOS_NAMES_MAX_NAME_CHARS + 1u];
    char again[SOS_NAMES_MAX_NAME_CHARS + 1u];

    for (uint64_t wordSeed = 0u; wordSeed < 500u; ++wordSeed)
    {
        for (int capitalized = 0; capitalized < 2; ++capitalized)
        {
            const bool upper = capitalized != 0;
            Expect(SosLanguageWord(&lang, wordSeed, upper, buffer, sizeof(buffer)),
                   "a word is built");
            const size_t length = strlen(buffer);
            Expect(length > 0u && length <= SOS_NAMES_MAX_NAME_CHARS,
                   "a word has a plausible length");
            Expect(LettersOnly(buffer), "a word is letters only");
            if (upper)
            {
                Expect(buffer[0] >= 'A' && buffer[0] <= 'Z',
                       "a capitalized word starts upper case");
                for (size_t index = 1u; index < length; ++index)
                {
                    Expect(buffer[index] >= 'a' && buffer[index] <= 'z',
                           "only the first letter is upper case");
                }
            }
            Expect(SosLanguageWord(&lang, wordSeed, upper, again, sizeof(again)) &&
                       strcmp(buffer, again) == 0,
                   "a word replays");
        }
    }
    Expect(SosLanguageWord(&lang, 1u, false, buffer, sizeof(buffer)) &&
               SosLanguageWord(&lang, 2u, false, again, sizeof(again)) &&
               strcmp(buffer, again) != 0,
           "different word seeds give different words");
}

static void TestTruncation(void)
{
    SosLanguage lang;
    SosLanguageFromSeed(SEED, &lang);
    char full[SOS_NAMES_MAX_NAME_CHARS + 1u];
    Expect(SosLanguageWord(&lang, 3u, true, full, sizeof(full)), "a full word fits");
    const size_t fullLength = strlen(full);
    const size_t bufferSize = SOS_NAMES_MAX_NAME_CHARS + 1u + 8u;

    for (size_t capacity = 1u; capacity <= fullLength + 2u; ++capacity)
    {
        char buffer[SOS_NAMES_MAX_NAME_CHARS + 1u + 8u];
        memset(buffer, '#', bufferSize);
        const bool written = SosLanguageWord(&lang, 3u, true, buffer, capacity);
        size_t length = 0u;
        while (length < capacity && buffer[length] != '\0')
        {
            ++length;
        }
        Expect(length < capacity, "a truncated word is NUL terminated within capacity");
        Expect(length <= fullLength && memcmp(buffer, full, length) == 0,
               "a truncated word is a prefix of the full word");
        Expect(written == (length > 0u), "the result says whether a syllable fit");
        for (size_t index = capacity; index < bufferSize; ++index)
        {
            Expect(buffer[index] == '#', "nothing is written past capacity");
        }
    }

    char untouched = '!';
    Expect(!SosLanguageWord(&lang, 3u, true, &untouched, 0u) && untouched == '!',
           "zero capacity writes nothing");
    Expect(!SosLanguageWord(&lang, 3u, true, NULL, 16u), "a NULL destination fails");
    Expect(!SosLanguageWord(NULL, 3u, true, full, sizeof(full)), "a NULL language fails");

    SosLanguage broken = {0};
    Expect(!SosLanguageWord(&broken, 3u, true, full, sizeof(full)), "an invalid language fails");
}

static bool EndsWith(const char *text, const char *ending)
{
    const size_t textLength = strlen(text);
    const size_t endingLength = strlen(ending);
    return endingLength <= textLength && strcmp(text + textLength - endingLength, ending) == 0;
}

static void TestPersonalNames(void)
{
    SosLanguage lang;
    SosLanguageFromSeed(SEED, &lang);
    char female[SOS_NAMES_MAX_NAME_CHARS + 1u];
    char male[SOS_NAMES_MAX_NAME_CHARS + 1u];
    char again[SOS_NAMES_MAX_NAME_CHARS + 1u];

    for (uint64_t serial = 1u; serial < 100u; ++serial)
    {
        Expect(SosNamesPersonal(&lang, SEED, serial, SOS_SEX_FEMALE, female, sizeof(female)) &&
                   LettersOnly(female) && EndsWith(female, lang.femaleEnding),
               "a female name ends in the language's female ending");
        Expect(SosNamesPersonal(&lang, SEED, serial, SOS_SEX_MALE, male, sizeof(male)) &&
                   LettersOnly(male) && EndsWith(male, lang.maleEnding),
               "a male name ends in the language's male ending");
        Expect(strcmp(female, male) != 0, "the two sexes are named differently");
        Expect(SosNamesPersonal(&lang, SEED, serial, SOS_SEX_FEMALE, again, sizeof(again)) &&
                   strcmp(female, again) == 0,
               "a personal name replays");
    }
    Expect(SosNamesPersonal(&lang, SEED, 1u, SOS_SEX_FEMALE, female, sizeof(female)) &&
               SosNamesPersonal(&lang, SEED, 2u, SOS_SEX_FEMALE, again, sizeof(again)) &&
               strcmp(female, again) != 0,
           "different serials give different names");
    Expect(!SosNamesPersonal(&lang, SEED, 0u, SOS_SEX_FEMALE, female, sizeof(female)),
           "nobody has no name");
    Expect(!SosNamesPersonal(&lang, SEED, 1u, SOS_SEX_FEMALE, female, 0u), "zero capacity fails");
}

static void TestFamilyGroupPlace(void)
{
    SosLanguage lang;
    SosLanguageFromSeed(SEED, &lang);
    char founder[SOS_NAMES_MAX_NAME_CHARS + 1u];
    char again[SOS_NAMES_MAX_NAME_CHARS + 1u];

    for (uint64_t serial = 1u; serial < 50u; ++serial)
    {
        Expect(SosNamesFamily(&lang, serial, founder, sizeof(founder)) && LettersOnly(founder),
               "a family name is letters");
        Expect(SosNamesFamily(&lang, serial, again, sizeof(again)) && strcmp(founder, again) == 0,
               "a family name depends only on the founder");
        Expect(SosNamesGroup(&lang, serial, again, sizeof(again)) && LettersOnly(again),
               "a group name is letters");
        Expect(SosNamesGroup(&lang, serial, again, sizeof(again)) &&
                   SosNamesFamily(&lang, serial, founder, sizeof(founder)) &&
                   strcmp(again, founder) != 0,
               "a group and a family of the same id differ");
    }
    Expect(SosNamesFamily(&lang, 1u, founder, sizeof(founder)) &&
               SosNamesFamily(&lang, 2u, again, sizeof(again)) && strcmp(founder, again) != 0,
           "different founders give different family names");
    Expect(!SosNamesFamily(&lang, 0u, founder, sizeof(founder)), "no line, no family name");
    Expect(!SosNamesGroup(&lang, 0u, founder, sizeof(founder)), "no group, no name");

    Expect(SosNamesPlace(&lang, 3, -4, founder, sizeof(founder)) && LettersOnly(founder),
           "a place name is letters");
    Expect(SosNamesPlace(&lang, 3, -4, again, sizeof(again)) && strcmp(founder, again) == 0,
           "a place name replays");
    Expect(SosNamesPlace(&lang, 4, -4, again, sizeof(again)) && strcmp(founder, again) != 0,
           "different cells give different place names");
    Expect(SosNamesPlace(&lang, INT64_MIN, INT64_MAX, again, sizeof(again)) && LettersOnly(again),
           "extreme cells are named safely");
}

static void TestDrift(void)
{
    SosLanguage parent;
    SosLanguageFromSeed(SEED, &parent);
    for (uint64_t seed = 0u; seed < 300u; ++seed)
    {
        SosLanguage child;
        SosLanguageDrift(&parent, seed, &child);
        ExpectLanguageValid(&child);
        Expect(child.seed == parent.seed, "a daughter keeps the seed of the family");
        Expect(PopCount(parent.consonantMask & child.consonantMask) + 1u >=
                   PopCount(parent.consonantMask),
               "a daughter keeps nearly all consonants");
        Expect(PopCount(parent.vowelMask & child.vowelMask) + 1u >= PopCount(parent.vowelMask),
               "a daughter keeps nearly all vowels");
        Expect(PopCount(parent.shapeMask & child.shapeMask) + 1u >= PopCount(parent.shapeMask),
               "a daughter keeps nearly all shapes");
        SosLanguage again;
        SosLanguageDrift(&parent, seed, &again);
        Expect(again.consonantMask == child.consonantMask && again.vowelMask == child.vowelMask &&
                   again.shapeMask == child.shapeMask && again.syllables == child.syllables &&
                   strcmp(again.femaleEnding, child.femaleEnding) == 0 &&
                   strcmp(again.maleEnding, child.maleEnding) == 0,
               "a daughter replays");
    }
    /* Aliasing the parent is allowed. */
    SosLanguage child;
    SosLanguageDrift(&parent, 9u, &child);
    SosLanguage aliased = parent;
    SosLanguageDrift(&aliased, 9u, &aliased);
    Expect(aliased.consonantMask == child.consonantMask &&
               strcmp(aliased.maleEnding, child.maleEnding) == 0,
           "a daughter may overwrite its parent");
    SosLanguageDrift(NULL, 1u, &child);
    SosLanguageDrift(&parent, 1u, NULL);
}

/* Fixed points: every platform and compiler must build exactly these names. */
static void TestGolden(void)
{
    SosLanguage lang;
    SosLanguageFromSeed(SEED, &lang);
    Expect(lang.consonantMask == UINT32_C(0x024EF918) && lang.vowelMask == UINT32_C(0x00000D13) &&
               lang.shapeMask == UINT32_C(0x0000000F) && lang.syllables == 1u,
           "the golden language's sounds");
    Expect(strcmp(lang.femaleEnding, "u") == 0 && strcmp(lang.maleEnding, "iet") == 0,
           "the golden language's endings");

    char buffer[SOS_NAMES_MAX_NAME_CHARS + 1u];
    Expect(SosLanguageWord(&lang, 1u, false, buffer, sizeof(buffer)) && strcmp(buffer, "teu") == 0,
           "the golden word");
    Expect(SosLanguageWord(&lang, 1u, true, buffer, sizeof(buffer)) && strcmp(buffer, "Teu") == 0,
           "the golden capitalized word");
    Expect(SosNamesPersonal(&lang, SEED, 7u, SOS_SEX_FEMALE, buffer, sizeof(buffer)) &&
               strcmp(buffer, "Phuthu") == 0,
           "the golden female name");
    Expect(SosNamesPersonal(&lang, SEED, 7u, SOS_SEX_MALE, buffer, sizeof(buffer)) &&
               strcmp(buffer, "Phuthiet") == 0,
           "the golden male name");
    Expect(SosNamesFamily(&lang, 7u, buffer, sizeof(buffer)) && strcmp(buffer, "Outh") == 0,
           "the golden family name");
    Expect(SosNamesGroup(&lang, 7u, buffer, sizeof(buffer)) && strcmp(buffer, "Ie") == 0,
           "the golden group name");
    Expect(SosNamesPlace(&lang, 3, -4, buffer, sizeof(buffer)) && strcmp(buffer, "Ef") == 0,
           "the golden place name");
}

int main(void)
{
    TestLanguageFromSeed();
    TestWords();
    TestTruncation();
    TestPersonalNames();
    TestFamilyGroupPlace();
    TestDrift();
    TestGolden();
    return 0;
}
