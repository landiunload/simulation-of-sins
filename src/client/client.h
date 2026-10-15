#ifndef SOS_CLIENT_CLIENT_H
#define SOS_CLIENT_CLIENT_H

#include <stdint.h>

typedef enum SosGraphicsChoice
{
    /* The best provider this platform has, falling back to the next one. */
    SOS_GRAPHICS_ANY = 0,
    SOS_GRAPHICS_D3D12,
    SOS_GRAPHICS_VULKAN,
} SosGraphicsChoice;

typedef struct SosClientOptions
{
    SosGraphicsChoice graphics;
    /* Non-zero presents this many frames and exits: a check that the whole
     * presentation path works on this machine. */
    uint32_t smokeFrames;
    double timeScale;
} SosClientOptions;

/* Exit codes beyond 0 tell why the client could not run. */
#define SOS_CLIENT_EXIT_INVALID_OPTIONS 2
#define SOS_CLIENT_EXIT_NO_ENGINE 3
#define SOS_CLIENT_EXIT_NO_PRESENTATION 4
#define SOS_CLIENT_EXIT_FRAME_FAILED 5

int SosClientRun(const SosClientOptions *options);

#endif
