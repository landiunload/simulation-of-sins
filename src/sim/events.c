#include "sim/events.h"

/* The name of each core kind, indexed by SosEventKind. */
static const char *const eventNames[SOS_EVENT_KIND_COUNT] = {
    [SOS_EVENT_NONE] = "none",
    [SOS_EVENT_BIRTH] = "birth",
    [SOS_EVENT_DEATH] = "death",
    [SOS_EVENT_MEAL] = "meal",
    [SOS_EVENT_HUNGER] = "hunger",
    [SOS_EVENT_THIRST] = "thirst",
    [SOS_EVENT_BOND] = "bond formed",
    [SOS_EVENT_SEPARATION] = "separation",
    [SOS_EVENT_CHILD_BORN] = "child born",
    [SOS_EVENT_KIN_DIED] = "kin died",
    [SOS_EVENT_INSULT] = "insult",
    [SOS_EVENT_GIFT] = "gift",
    [SOS_EVENT_HELP] = "help",
    [SOS_EVENT_THREAT] = "threat",
    [SOS_EVENT_ATTACK] = "attack",
    [SOS_EVENT_THEFT] = "theft",
    [SOS_EVENT_SIN] = "sin committed",
    [SOS_EVENT_SIN_WITNESSED] = "sin witnessed",
    [SOS_EVENT_DISCOVERY] = "discovery",
    [SOS_EVENT_MIGRATION] = "migration",
    [SOS_EVENT_GROUP_FOUNDED] = "group founded",
    [SOS_EVENT_GROUP_DISSOLVED] = "group dissolved",
    [SOS_EVENT_GROUP_MERGED] = "group merged",
    [SOS_EVENT_FOOD_FOUND] = "food found",
    [SOS_EVENT_WATER_FOUND] = "water found",
    [SOS_EVENT_DANGER_SEEN] = "danger seen",
    [SOS_EVENT_DISASTER] = "disaster",
    [SOS_EVENT_CONFLICT] = "conflict",
};

const char *SosEventName(uint32_t kind)
{
    if (kind < (uint32_t)SOS_EVENT_KIND_COUNT)
    {
        return eventNames[kind];
    }
    if (kind >= (uint32_t)SOS_EVENT_CUSTOM_FIRST)
    {
        return "custom";
    }
    return "unknown";
}
