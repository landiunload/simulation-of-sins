#ifndef SOS_APP_MODULES_H
#define SOS_APP_MODULES_H

#include "mod/module_host.h"

#include <stdbool.h>
#include <stdint.h>

/* Engine technologies are loaded at run time by name and never linked. A
 * missing or broken module switches off only what it provides; the caller
 * learns that from a NULL service and decides how the game continues. */

#define SOS_MODULES_MAX_NAMES 16u

typedef struct SosModules
{
    LaiueModuleHost *host;
} SosModules;

bool SosModulesOpen(SosModules *modules);
void SosModulesClose(SosModules *modules);

/* Loads the named engine modules next to the executable, each one optional.
 * Every module that did not start is logged with the reason. False only when
 * the request itself is invalid or the host refused it; a partial result is
 * success. */
bool SosModulesLoad(SosModules *modules, const char *const *names, uint32_t nameCount,
                    const LaiueModuleProviderSelectionV1 *selections, uint32_t selectionCount);

/* Stops every loaded module. Services queried earlier become invalid. */
void SosModulesUnload(SosModules *modules);

/* The service table, or NULL when no started module publishes it with at
 * least this version and size. outSize receives the published size. */
const void *SosModulesQuery(const SosModules *modules, const char *service, uint32_t minimumVersion,
                            uint32_t minimumSize, uint32_t *outSize);

#endif
