#include "app/core_smoke.h"

#include "game/foundation_world.h"
#include "game/rebase_policy.h"

#include "world/world.h"

#include <stdint.h>

bool SimulationCoreSmokeTest(void)
{
    World *world = WorldCreate(NULL);
    if (world == NULL)
    {
        return false;
    }

    uint64_t initialRevision = WorldGetRevision(world);
    bool succeeded = SimulationFoundationWorldPopulate(world) &&
                     SimulationFoundationWorldValidate(world) &&
                     WorldGetRevision(world) > initialRevision;
    WorldDestroy(world);

    const double position[3] = {513.25, -513.75, 3.0};
    SimulationOriginShift shift;
    return succeeded && SimulationOriginShiftPlan(position, &shift) && shift.required &&
           shift.block[0] == 512 && shift.block[1] == -512 && shift.block[2] == 0;
}
