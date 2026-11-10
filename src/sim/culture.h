#ifndef SOS_SIM_CULTURE_H
#define SOS_SIM_CULTURE_H

#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* Culture: what a group believes and how its people behave.
 *
 * A culture is 256 binary memes packed into four 64-bit words: customs,
 * taboos, beliefs, styles and techniques. Nothing is stored per meme; what a
 * slot means is a function of the world seed and its index, so the same bits
 * read the same on every machine while the set of possible meanings stays
 * unbounded. Only the 256 bits travel, split and merge.
 *
 * A group's culture is a world function of the seed and the group id, like the
 * plant cover: the founders of a group share it without storing it. A person
 * receives its own copy by transmission from its parents and its group, with
 * rare innovation, and only that copy is kept. An attitude on any topic is
 * derived from the memes on demand, so a handful of bits yields endless
 * opinions without storing one.
 *
 * Every function is pure: the same inputs give the same outputs everywhere. */

#define SOS_CULTURE_MEMES 256u
#define SOS_CULTURE_WORDS 4u

/* Share of a child's memes inherited from its parents; the rest are oblique,
 * taken from the group. Within the vertical share the mother and father count
 * equally. Three quarters keeps descent visible without freezing a culture. */
#define SOS_CULTURE_VERTICAL_SHARE (3 * SOS_Q16_ONE / 4)
/* Chance that one meme mutates at conception, before conformity scales it
 * down: a conformist meme (conformity near one) almost never changes. */
#define SOS_CULTURE_INNOVATION_RATE (SOS_Q16_ONE / 64)
/* Two cultures count as the same when at most this share of their memes
 * differ. */
#define SOS_CULTURE_SAME_THRESHOLD (SOS_Q16_ONE / 16)
/* Chance that a meme counts toward one topic's attitude; the rest are ignored,
 * so a topic weighs a small subset of the memes and attitudes vary. */
#define SOS_CULTURE_TOPIC_SELECTIVITY (SOS_Q16_ONE / 16)
/* Chance that a founder holds a meme. Half makes founders of different groups
 * as unrelated as coin flips. */
#define SOS_CULTURE_FOUNDER_DENSITY (SOS_Q16_ONE / 2)

typedef struct SosCulture
{
    uint64_t memes[SOS_CULTURE_WORDS];
} SosCulture;

typedef enum SosMemeCategory
{
    SOS_MEME_CUSTOM = 0,
    SOS_MEME_TABOO,
    SOS_MEME_BELIEF,
    SOS_MEME_STYLE,
    SOS_MEME_TECHNIQUE,
    SOS_MEME_CATEGORY_COUNT,
} SosMemeCategory;

/* What one meme slot means for a seed. */
typedef struct SosMemeInfo
{
    SosMemeCategory category;
    /* How hard the meme is to change, from loose (zero) to settled (ONE). */
    SosQ16 conformity;
} SosMemeInfo;

/* The meaning of meme `index` for `seed`. False and nothing written when
 * `index` is at least SOS_CULTURE_MEMES or `outInfo` is NULL. */
bool SosCultureMemeInfo(uint64_t seed, uint32_t index, SosMemeInfo *outInfo);

/* The culture a group starts with: a stable function of `seed` and `groupId`,
 * so the same group always founds the same way. False and nothing written when
 * `outCulture` is NULL. */
bool SosCultureFounder(uint64_t seed, uint64_t groupId, SosCulture *outCulture);

/* A child's culture. Each meme comes from the mother, the father or the group
 * with the shares above; a NULL parent or group is not a source and the rest
 * share its weight, so a missing source never makes the call fail. Rare
 * innovation then may flip a meme, less often the more conformist it is.
 * `serial` is the child's serial and keeps siblings apart. False when
 * `outCulture` is NULL. */
bool SosCultureTransmit(const SosCulture *mother, const SosCulture *father, const SosCulture *group,
                        uint64_t seed, uint64_t serial, SosCulture *outCulture);

/* Share of the 256 memes that differ, in [0, ONE]. Zero when either culture is
 * NULL, a safe "no information" answer rather than a real distance. */
SosQ16 SosCultureDistance(const SosCulture *a, const SosCulture *b);

/* Whether at most SOS_CULTURE_SAME_THRESHOLD of the memes differ. False when
 * either culture is NULL. */
bool SosCultureSame(const SosCulture *a, const SosCulture *b);

/* Merges two groups' cultures: each meme is taken from `a` with probability
 * `share` (clamped to [0, ONE]) and from `b` otherwise, `seed` choosing the
 * mix. A NULL `a` keeps all of `b` and vice versa; both NULL give an empty
 * culture. False when `outCulture` is NULL. */
bool SosCultureBlend(const SosCulture *a, const SosCulture *b, SosQ16 share, uint64_t seed,
                     SosCulture *outCulture);

/* A culture after `generations` of drift: each meme flips per generation with
 * the conformity-scaled innovation chance, so a settled culture lasts and a
 * loose one wanders. Zero generations copies the culture. False when `culture`
 * or `outCulture` is NULL. */
bool SosCultureDrift(const SosCulture *culture, uint64_t seed, uint32_t generations,
                     SosCulture *outCulture);

/* How the culture feels about `topicId`, in [0, ONE], where the topic weighs a
 * subset of the memes and half means balanced. Any topic id works, so opinions
 * are unbounded without being stored. Zero when `culture` is NULL. */
SosQ16 SosCultureAttitude(const SosCulture *culture, uint32_t topicId);

/* Folds the culture into one value for a world hash. Zero only when `culture`
 * is NULL. */
uint64_t SosCultureHash(const SosCulture *culture);

#endif
