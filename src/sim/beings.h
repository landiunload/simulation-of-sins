#ifndef SOS_SIM_BEINGS_H
#define SOS_SIM_BEINGS_H

#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* People.
 *
 * Every being has a serial number that is never reused, so genealogy and
 * recorded history can name it forever, and a slot in column storage that
 * the simulation walks in a fixed order. A step advances all beings by any
 * number of ticks: fine steps while someone watches, coarse ones while time
 * is fast-forwarded. All arithmetic is integer, so a step gives the same
 * result on every machine. */

/* Needs and health share one fixed-point scale: 0 is satisfied (for health,
 * dead) and SOS_NEED_MAX is unbearable (for health, perfect). */
#define SOS_NEED_MAX (UINT32_C(1) << 30)

typedef enum SosSex
{
    SOS_SEX_FEMALE = 0,
    SOS_SEX_MALE = 1,
} SosSex;

typedef enum SosActivity
{
    SOS_ACTIVITY_IDLE = 0,
    SOS_ACTIVITY_WANDER,
    SOS_ACTIVITY_EAT,
    SOS_ACTIVITY_DRINK,
    SOS_ACTIVITY_SLEEP,
} SosActivity;

/* Knowledge is a set of discoveries. Their meaning and the tree that links
 * them belong to the technology layer; the founders start with these. */
#define SOS_KNOWLEDGE_SPEECH (UINT64_C(1) << 0)
#define SOS_KNOWLEDGE_FORAGING (UINT64_C(1) << 1)

typedef struct SosBeings SosBeings;

typedef struct SosBeingSpawn
{
    SosSex sex;
    /* May lie before tick zero: the founders arrive as adults. */
    int64_t birthTick;
    SosPosition position;
    uint64_t knowledge;
    uint64_t mother;
    uint64_t father;
} SosBeingSpawn;

/* A read-only copy of one being for presentation, tests and tools. */
typedef struct SosBeingView
{
    uint64_t serial;
    SosSex sex;
    int64_t birthTick;
    SosPosition position;
    uint32_t health;
    uint32_t hunger;
    uint32_t thirst;
    uint32_t fatigue;
    uint64_t knowledge;
    uint64_t mother;
    uint64_t father;
    bool pregnant;
    SosActivity activity;
} SosBeingView;

typedef struct SosBeingsStats
{
    uint64_t births;
    uint64_t deaths;
} SosBeingsStats;

SosBeings *SosBeingsCreate(uint64_t worldSeed);
void SosBeingsDestroy(SosBeings *beings);

/* The serial of the new being, or 0 when the request is invalid or memory
 * ran out. The being stands on the terrain surface below its position. */
uint64_t SosBeingsSpawn(SosBeings *beings, const SosTerrain *terrain, uint64_t tick,
                        const SosBeingSpawn *spawn);

/* Advances every living being from `tick` by `ticks` ticks. */
void SosBeingsStep(SosBeings *beings, const SosTerrain *terrain, uint64_t tick, uint64_t ticks);

uint32_t SosBeingsLivingCount(const SosBeings *beings);
/* Slots are visited in [0, SlotCount); empty slots return false. */
uint32_t SosBeingsSlotCount(const SosBeings *beings);
bool SosBeingsView(const SosBeings *beings, uint32_t slot, SosBeingView *outView);
SosBeingsStats SosBeingsGetStats(const SosBeings *beings);

/* Folds the complete state into one value: equal worlds give equal hashes, so
 * replays and machines can be compared. */
uint64_t SosBeingsHash(const SosBeings *beings);

#endif
