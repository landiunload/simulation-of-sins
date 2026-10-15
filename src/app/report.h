#ifndef SOS_APP_REPORT_H
#define SOS_APP_REPORT_H

#include "sim/beings.h"
#include "sim/world.h"

/* Logs births and deaths since the last report with the game date; silent
 * when nothing changed. */
void SosReportPopulation(const SosWorld *world, SosBeingsStats *reported);

/* Logs where the world stands: date, population and the state hash. */
void SosReportWorld(const SosWorld *world, const char *event);

#endif
