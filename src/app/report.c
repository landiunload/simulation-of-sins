#include "app/report.h"

#include "app/log.h"
#include "sim/calendar.h"

#include <inttypes.h>
#include <stddef.h>

void SosReportPopulation(const SosWorld *world, SosBeingsStats *reported)
{
    if (world == NULL || reported == NULL)
    {
        return;
    }
    const SosBeings *beings = SosWorldBeings(world);
    const SosBeingsStats stats = SosBeingsGetStats(beings);
    if (stats.births == reported->births && stats.deaths == reported->deaths)
    {
        return;
    }
    const SosDate date = SosCalendarDate(SosWorldTick(world));
    SosLog(SOS_LOG_INFO, "year %" PRId64 " day %d: %u living, %" PRIu64 " born, %" PRIu64 " died",
           date.year, date.day, SosBeingsLivingCount(beings), stats.births, stats.deaths);
    *reported = stats;
}

void SosReportWorld(const SosWorld *world, const char *event)
{
    if (world == NULL || event == NULL)
    {
        return;
    }
    const SosDate date = SosCalendarDate(SosWorldTick(world));
    SosLog(SOS_LOG_INFO,
           "%s at tick %" PRIu64 " (year %" PRId64
           " day %d %02d:%02d): %u living, state %016" PRIx64,
           event, SosWorldTick(world), date.year, date.day, date.hour, date.minute,
           SosBeingsLivingCount(SosWorldBeings(world)), SosWorldHash(world));
}
