#ifndef SOS_SIM_EVENTS_H
#define SOS_SIM_EVENTS_H

#include <stdint.h>

/* What happens, in one vocabulary.
 *
 * Every layer that records or reacts to a happening - the chronicle that keeps
 * history, a being's memory, a rumor that spreads, a mood that answers - needs
 * the same name for the same event, so a kind is defined once here instead of
 * once per module. A kind is an open-ended number: SOS_EVENT_NONE is no event,
 * the values below SOS_EVENT_KIND_COUNT are the core vocabulary this header
 * gives meaning to, and a later layer defines its own kinds from
 * SOS_EVENT_CUSTOM_FIRST upward without this file learning about them. Nothing
 * stores the set of kinds, so the vocabulary grows forever at no cost.
 *
 * The band between SOS_EVENT_KIND_COUNT and SOS_EVENT_CUSTOM_FIRST is reserved:
 * it is not a core kind and SosEventName reads it as "unknown". Keeping it free
 * lets the core add kinds without ever moving a custom kind's number, so a kind
 * a later layer already stored keeps its meaning. */

typedef enum SosEventKind
{
    /* Not a happening: what a query matches any kind against, never stored. */
    SOS_EVENT_NONE = 0,
    SOS_EVENT_BIRTH,
    SOS_EVENT_DEATH,
    SOS_EVENT_MEAL,
    SOS_EVENT_HUNGER,
    SOS_EVENT_THIRST,
    SOS_EVENT_BOND,
    SOS_EVENT_SEPARATION,
    SOS_EVENT_CHILD_BORN,
    SOS_EVENT_KIN_DIED,
    SOS_EVENT_INSULT,
    SOS_EVENT_GIFT,
    SOS_EVENT_HELP,
    SOS_EVENT_THREAT,
    SOS_EVENT_ATTACK,
    SOS_EVENT_THEFT,
    SOS_EVENT_SIN,
    SOS_EVENT_SIN_WITNESSED,
    SOS_EVENT_DISCOVERY,
    SOS_EVENT_MIGRATION,
    SOS_EVENT_GROUP_FOUNDED,
    SOS_EVENT_GROUP_DISSOLVED,
    SOS_EVENT_GROUP_MERGED,
    SOS_EVENT_FOOD_FOUND,
    SOS_EVENT_WATER_FOUND,
    SOS_EVENT_DANGER_SEEN,
    SOS_EVENT_DISASTER,
    SOS_EVENT_CONFLICT,
    /* The number of core kinds; itself not a kind. */
    SOS_EVENT_KIND_COUNT,
    /* First number a later layer may define a kind for itself. It sits far above
     * SOS_EVENT_KIND_COUNT so the core vocabulary has room to grow without ever
     * moving a kind a later layer already stored. */
    SOS_EVENT_CUSTOM_FIRST = 65536,
} SosEventKind;

/* The lowercase name of `kind`: the core kind's name ("none", "birth",
 * "sin witnessed", ...), "custom" for any kind from SOS_EVENT_CUSTOM_FIRST up,
 * and "unknown" for a number in the reserved band below it. Never NULL, and the
 * returned string is constant and stable across calls. */
const char *SosEventName(uint32_t kind);

#endif
