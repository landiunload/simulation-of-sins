#include "sim/events.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "events test failed: %s\n", message);
        exit(1);
    }
}

/* The whole core vocabulary, pinned: every platform and compiler must name
 * each kind exactly this way. */
static void TestVocabulary(void)
{
    static const char *const names[SOS_EVENT_KIND_COUNT] = {
        "none",          "birth",           "death",        "meal",
        "hunger",        "thirst",          "bond formed",  "separation",
        "child born",    "kin died",        "insult",       "gift",
        "help",          "threat",          "attack",       "theft",
        "sin committed", "sin witnessed",   "discovery",    "migration",
        "group founded", "group dissolved", "group merged", "food found",
        "water found",   "danger seen",     "disaster",     "conflict",
    };
    Expect((uint32_t)SOS_EVENT_KIND_COUNT == 28u, "the core vocabulary has a fixed size");
    for (uint32_t kind = 0u; kind < (uint32_t)SOS_EVENT_KIND_COUNT; ++kind)
    {
        const char *name = SosEventName(kind);
        Expect(name != NULL && strcmp(name, names[kind]) == 0, "a core kind has its name");
    }
}

/* Numbers outside the core vocabulary fall into two bands. */
static void TestBands(void)
{
    Expect(strcmp(SosEventName((uint32_t)SOS_EVENT_KIND_COUNT), "unknown") == 0,
           "the band above the core is unknown");
    Expect(strcmp(SosEventName(SOS_EVENT_CUSTOM_FIRST - 1u), "unknown") == 0,
           "the last unknown kind is unknown");
    Expect(strcmp(SosEventName(SOS_EVENT_CUSTOM_FIRST), "custom") == 0,
           "the first custom kind is custom");
    Expect(strcmp(SosEventName(SOS_EVENT_CUSTOM_FIRST + 7u), "custom") == 0,
           "custom kinds stay custom");
    Expect(strcmp(SosEventName(UINT32_MAX), "custom") == 0, "the last kind is custom");
}

/* A name is a pure function of the kind, so a replay reads it the same way. */
static void TestDeterminism(void)
{
    const uint32_t kinds[] = {0u, (uint32_t)SOS_EVENT_SIN, (uint32_t)SOS_EVENT_KIND_COUNT,
                              SOS_EVENT_CUSTOM_FIRST, UINT32_MAX};
    for (size_t index = 0u; index < sizeof(kinds) / sizeof(kinds[0]); ++index)
    {
        const char *first = SosEventName(kinds[index]);
        const char *again = SosEventName(kinds[index]);
        Expect(first != NULL && strcmp(first, again) == 0, "a name is stable");
    }
}

int main(void)
{
    TestVocabulary();
    TestBands();
    TestDeterminism();
    return 0;
}
