#include "sim/memories.h"

#include "sim/calendar.h"
#include "sim/random.h"

#include <stddef.h>

/* Half-lives of a fading memory, in ticks: the time for its salience to drop
 * by half. A dull event is gone within days, while one that moved the being
 * deeply is still vivid after two months. The half-life grows linearly with
 * the strength of the emotion, so feeling buys time; a zero valence takes the
 * calm value. */
#define SOS_MEMORIES_HALF_LIFE_CALM (2 * SOS_TICKS_PER_DAY)
#define SOS_MEMORIES_HALF_LIFE_VIVID (64 * SOS_TICKS_PER_DAY)
/* |valence| of the most extreme emotion; valence is a signed 16-bit value and
 * -32768 has magnitude 32768. */
#define SOS_MEMORIES_VALENCE_MAX 32768u

/* The magnitude of an emotion; |INT16_MIN| does not fit an int16. */
static uint32_t ValenceStrength(int16_t valence)
{
    return valence < 0 ? (uint32_t)(-(int32_t)valence) : (uint32_t)valence;
}

/* Half-life in ticks of a memory with this emotion. */
static uint64_t HalfLifeFor(int16_t valence)
{
    const uint64_t span = SOS_MEMORIES_HALF_LIFE_VIVID - SOS_MEMORIES_HALF_LIFE_CALM;
    return SOS_MEMORIES_HALF_LIFE_CALM + span * ValenceStrength(valence) / SOS_MEMORIES_VALENCE_MAX;
}

/* The salience of a record at `tick`; a record from the future has not faded. */
static SosQ16 SalienceAt(const SosMemory *memory, uint64_t tick)
{
    const uint64_t elapsed = tick > memory->tick ? tick - memory->tick : 0u;
    return (SosQ16)SosDecay(memory->salience, elapsed, HalfLifeFor(memory->valence));
}

void SosMemoriesInit(SosMemories *memories)
{
    if (memories != NULL)
    {
        *memories = (SosMemories){0};
    }
}

bool SosMemoriesRemember(SosMemories *memories, const SosMemory *memory, uint64_t tick)
{
    if (memories == NULL || memory == NULL || memory->kind == SOS_EVENT_NONE ||
        memory->salience < 0 || memory->salience > SOS_MEMORIES_MAX_SALIENCE || memory->tick > tick)
    {
        return false;
    }

    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        SosMemory *const existing = &memories->records[index];
        if (existing->kind == memory->kind && existing->subject == memory->subject)
        {
            /* The same event again. Bring both records to the later of their
             * ticks before adding, so the decay between them is not thrown
             * away and a long-forgotten memory cannot come back at full
             * strength. The record keeps the newer event's place and valence,
             * and its tick never moves backwards when events arrive out of
             * order. */
            const uint64_t laterTick =
                existing->tick > memory->tick ? existing->tick : memory->tick;
            const int64_t combined =
                (int64_t)SalienceAt(existing, laterTick) + (int64_t)SalienceAt(memory, laterTick);
            existing->salience =
                (SosQ16)(combined > SOS_MEMORIES_MAX_SALIENCE ? SOS_MEMORIES_MAX_SALIENCE
                                                              : combined);
            if (memory->tick >= existing->tick)
            {
                existing->place = memory->place;
                existing->valence = memory->valence;
                existing->tick = memory->tick;
            }
            return true;
        }
    }

    if (memories->count < SOS_MEMORIES_CAPACITY)
    {
        memories->records[memories->count] = *memory;
        memories->count += 1u;
        return true;
    }

    /* Full: find the least salient record at `tick`, the earliest on a tie. */
    uint32_t weakest = 0u;
    SosQ16 weakestSalience = SalienceAt(&memories->records[0], tick);
    for (uint32_t index = 1u; index < memories->count; ++index)
    {
        const SosQ16 salience = SalienceAt(&memories->records[index], tick);
        if (salience < weakestSalience)
        {
            weakest = index;
            weakestSalience = salience;
        }
    }
    if (SalienceAt(memory, tick) <= weakestSalience)
    {
        return false;
    }
    memories->records[weakest] = *memory;
    return true;
}

bool SosMemoriesRecall(const SosMemories *memories, uint32_t kind, uint64_t subject, uint64_t tick,
                       SosMemory *outMemory)
{
    if (memories == NULL || outMemory == NULL)
    {
        return false;
    }

    bool found = false;
    SosQ16 best = 0;
    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        const SosMemory *const record = &memories->records[index];
        if ((kind != SOS_EVENT_NONE && record->kind != kind) ||
            (subject != 0u && record->subject != subject))
        {
            continue;
        }
        const SosQ16 salience = SalienceAt(record, tick);
        /* A memory that faded to nothing is not recalled; SosMemoriesForget
         * is what drops it. */
        if (salience <= 0 || (found && salience <= best))
        {
            continue;
        }
        found = true;
        best = salience;
        *outMemory = *record;
        outMemory->salience = salience;
    }
    return found;
}

int32_t SosMemoriesFeelingAbout(const SosMemories *memories, uint64_t subject, uint64_t tick)
{
    if (memories == NULL || subject == 0u)
    {
        return 0;
    }

    int64_t weighted = 0;
    int64_t total = 0;
    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        const SosMemory *const record = &memories->records[index];
        if (record->subject != subject)
        {
            continue;
        }
        const SosQ16 salience = SalienceAt(record, tick);
        if (salience <= 0)
        {
            continue;
        }
        weighted += (int64_t)record->valence * salience;
        total += salience;
    }
    return total == 0 ? 0 : (int32_t)(weighted / total);
}

bool SosMemoriesPlaceOf(const SosMemories *memories, uint32_t kind, uint64_t tick,
                        SosPosition *outPlace)
{
    if (memories == NULL || outPlace == NULL)
    {
        return false;
    }

    bool found = false;
    SosQ16 best = 0;
    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        const SosMemory *const record = &memories->records[index];
        if (kind != SOS_EVENT_NONE && record->kind != kind)
        {
            continue;
        }
        const SosQ16 salience = SalienceAt(record, tick);
        if (salience <= 0 || (found && salience <= best))
        {
            continue;
        }
        found = true;
        best = salience;
        *outPlace = record->place;
    }
    return found;
}

uint32_t SosMemoriesForget(SosMemories *memories, uint64_t tick)
{
    if (memories == NULL)
    {
        return 0u;
    }

    uint32_t kept = 0u;
    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        if (SalienceAt(&memories->records[index], tick) <= 0)
        {
            continue;
        }
        memories->records[kept] = memories->records[index];
        kept += 1u;
    }
    const uint32_t dropped = memories->count - kept;
    memories->count = kept;
    return dropped;
}

uint64_t SosMemoriesHash(const SosMemories *memories)
{
    if (memories == NULL)
    {
        return 0u;
    }

    /* A sum does not depend on the order of the records, so two machines that
     * reached the same memories by different routes hash the same. */
    uint64_t sum = 0u;
    for (uint32_t index = 0u; index < memories->count; ++index)
    {
        const SosMemory *const record = &memories->records[index];
        uint64_t fold = SosHash64(record->kind);
        fold = SosHashCombine(fold, record->subject);
        fold = SosHashCombine(fold, (uint64_t)record->place.x);
        fold = SosHashCombine(fold, (uint64_t)record->place.y);
        fold = SosHashCombine(fold, (uint64_t)record->place.z);
        fold = SosHashCombine(fold, record->tick);
        fold = SosHashCombine(fold, (uint64_t)(int64_t)record->salience);
        fold = SosHashCombine(fold, (uint64_t)(int64_t)record->valence);
        sum += fold;
    }
    return SosHashCombine(memories->count, sum);
}
