#ifndef SOS_SIM_SINS_H
#define SOS_SIM_SINS_H

#include "sim/calendar.h"
#include "sim/fixed.h"

#include <stdbool.h>
#include <stdint.h>

/* The seven deadly sins and the virtues that oppose them.
 *
 * Every being carries a nature, a tendency toward each sin, inborn from the
 * predisposition the caller passes (later from genome and personality) and
 * set apart by a seeded individual variation, so two beings with the same
 * parents and traits still differ. A situation then offers temptations: its
 * chance follows from the nature and the situation alone, so a being step
 * needs no stored mood or plan, only a call and a draw from the being's own
 * random stream. A temptation needs an occasion: a situation that offers no
 * opportunity tempts nobody. Wrath is the single exception, because an injury
 * already suffered is its own occasion, so a being can be provoked into wrath
 * even where nothing else could move it.
 *
 * What a being did is a plain value type the caller stores per being (in a
 * SosTable, say), so nothing is held by this module and idle beings cost
 * nothing. Committing a sin is remembered as a count and leaves guilt and,
 * when it was seen, notoriety. Committing strengthens the habit and resisting
 * weakens it. Guilt and notoriety are written at a tick and read later as a
 * function of elapsed time through SosDecay, like plant regrowth: no record
 * is touched while time passes. A world hash folds SosSinsNatureHash over the
 * habits and SosSinsHash over the ledger. */

/* The seven sins in a fixed order; each is opposed by the virtue of the same
 * index in the virtue names. */
typedef enum SosSin
{
    SOS_SIN_PRIDE = 0,
    SOS_SIN_GREED,
    SOS_SIN_LUST,
    SOS_SIN_ENVY,
    SOS_SIN_GLUTTONY,
    SOS_SIN_WRATH,
    SOS_SIN_SLOTH,
    SOS_SIN_COUNT,
} SosSin;

/* How far one full-severity sin moves the habit and how far one resisted
 * temptation moves it back. A habit drifts over dozens of acts, never at a
 * single one, so character changes slowly. */
#define SOS_SINS_HABIT_GAIN (SOS_Q16_ONE / 64)
#define SOS_SINS_HABIT_LOSS (SOS_Q16_ONE / 128)
/* Guilt one full-severity sin leaves, and how much one resisted temptation
 * eases it: remorse is real but a hard-won restraint makes up ground. */
#define SOS_SINS_GUILT_PER_SIN (SOS_Q16_ONE / 2)
#define SOS_SINS_RESIST_RELIEF (SOS_Q16_ONE / 16)
/* Notoriety one seen full-severity sin leaves: a witness's word, not the act. */
#define SOS_SINS_NOTORIETY_PER_SIN (SOS_Q16_ONE / 2)
/* Half-lives: remorse fades within a week, a bad name lingers for a month. */
#define SOS_SINS_GUILT_HALF_LIFE (7 * SOS_TICKS_PER_DAY)
#define SOS_SINS_NOTORIETY_HALF_LIFE (30 * SOS_TICKS_PER_DAY)
/* Individual variation added to the predisposition, at most a quarter either
 * way: enough for character, never enough to invert it. */
#define SOS_SINS_VARIATION (SOS_Q16_ONE / 4)
/* How much guilt and notoriety weigh in the moral balance. */
#define SOS_SINS_GUILT_WEIGHT (SOS_Q16_ONE / 2)
#define SOS_SINS_NOTORIETY_WEIGHT (SOS_Q16_ONE / 2)

/* A being's inborn tendencies, each in [0, SOS_Q16_ONE]. */
typedef struct SosSinNature
{
    SosQ16 tendency[SOS_SIN_COUNT];
} SosSinNature;

/* What a moment offers a being, all SosQ16. opportunity is how easy the sin
 * is here and the occasion every temptation needs: without it no sin is
 * possible except wrath, which provocation alone can drive (see
 * SosSinsTemptation). need is an unmet want; provocation an injury or insult
 * just suffered; rivalAdvantage how much better off a rival is; watched how
 * public the moment is (shame lowers every chance); mood is the current humour
 * in [-SOS_Q16_ONE, SOS_Q16_ONE], higher being content. Values outside their
 * range are clamped, so a caller cannot inflate a chance. */
typedef struct SosSinSituation
{
    SosQ16 opportunity;
    SosQ16 need;
    SosQ16 provocation;
    SosQ16 rivalAdvantage;
    SosQ16 watched;
    SosQ16 mood;
} SosSinSituation;

/* A being's ledger of sin and restraint, zero-filled to begin with. `guilt`
 * and `notoriety` are in [0, SOS_Q16_ONE] as of `stamp`; read them through
 * SosSinsGuilt, SosSinsNotoriety or SosSinsBalance, which decay them to the
 * queried tick. */
typedef struct SosSinRecord
{
    uint32_t committed[SOS_SIN_COUNT];
    uint32_t resisted[SOS_SIN_COUNT];
    SosQ16 guilt;
    SosQ16 notoriety;
    uint64_t stamp;
} SosSinRecord;

/* Fills `out` with the inborn tendencies from `predisposition` plus a seeded
 * variation taken from the `serial` stream of `seed`. False when `out` or
 * `predisposition` is NULL; the output is then untouched. */
bool SosSinsNature(uint64_t seed, uint64_t serial, const SosQ16 predisposition[SOS_SIN_COUNT],
                   SosSinNature *out);

/* The chance to give in to `sin` in `situation`, in [0, SOS_Q16_ONE]:
 * tendency times the situation's pressure, and the pressure is scaled by the
 * opportunity, so a situation with no opportunity tempts nobody. Wrath is the
 * one exception: its provocation is not gated by the opportunity, so an
 * injury can make a being wrathful where there is no occasion at all. Zero
 * when any argument is NULL or `sin` is not a sin. */
SosQ16 SosSinsTemptation(const SosSinNature *nature, SosSin sin, const SosSinSituation *situation);

/* Records a committed `sin` of `severity` in [0, SOS_Q16_ONE] at `tick`:
 * counts it, raises the habit slightly, adds guilt and, when `witnessed`, adds
 * notoriety. False when `nature` or `record` is NULL or `sin` is not a sin,
 * leaving both untouched. */
bool SosSinsCommit(SosSinNature *nature, SosSinRecord *record, SosSin sin, SosQ16 severity,
                   uint64_t tick, bool witnessed);

/* Records a resisted temptation of `sin` at `tick`: counts it, lowers the
 * habit slightly and eases guilt. False under the same conditions as
 * SosSinsCommit. */
bool SosSinsResist(SosSinNature *nature, SosSinRecord *record, SosSin sin, uint64_t tick);

/* The guilt and notoriety of `record` at `tick`, in [0, SOS_Q16_ONE]; zero
 * for a NULL record. */
SosQ16 SosSinsGuilt(const SosSinRecord *record, uint64_t tick);
SosQ16 SosSinsNotoriety(const SosSinRecord *record, uint64_t tick);

/* The moral balance of `record` at `tick`, in [-SOS_Q16_ONE, SOS_Q16_ONE]:
 * positive is virtue, negative is sin. It compares resisted with committed
 * acts and lets the current guilt and notoriety pull it down. Zero for a NULL
 * record. */
SosQ16 SosSinsBalance(const SosSinRecord *record, uint64_t tick);

/* Folds the state of `record` at `tick` into one value, order-independently,
 * for a world hash; zero for a NULL record. Guilt and notoriety are folded as
 * they are at `tick`, so equal states give equal hashes, and the moment the
 * record was last written is folded too, so two ledgers whose decayed
 * feelings coincide are still told apart. */
uint64_t SosSinsHash(const SosSinRecord *record, uint64_t tick);

/* Folds the tendencies of `nature` into one value, order-independently, for a
 * world hash: the habit changes with every sin committed and resisted, so a
 * world hash folds this beside SosSinsHash. Zero for a NULL nature. */
uint64_t SosSinsNatureHash(const SosSinNature *nature);

/* The name of a sin and of its opposing virtue, English and lower case, e.g.
 * "pride" and "humility"; NULL when `sin` is not a sin. */
const char *SosSinsName(SosSin sin);
const char *SosSinsVirtueName(SosSin sin);

#endif
