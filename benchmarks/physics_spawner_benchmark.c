#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "game/falling_cubes.h"
#include "game/ground_provider.h"
#include "task/task_pool.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define BENCHMARK_FPRINTF fprintf_s
#else
#include <time.h>
#define BENCHMARK_FPRINTF fprintf
#endif

#define BENCHMARK_DEFAULT_TICKS 2048u
#define BENCHMARK_MAX_TICKS 16384u
#define BENCHMARK_REPORT_TICKS 128u
#define BENCHMARK_DEFAULT_THREADS 4u

// Four configurations are measured in one paired run so that broadphase and
// executor can be compared under identical machine conditions:
// {grid, tree} x {serial, executor}. All four must stay bitwise equal.
#define BENCHMARK_CONFIG_COUNT 4u

typedef struct BenchmarkOptions
{
    uint32_t ticks;
    uint32_t threads;
    VoxelRigidSolverOrder solverOrder;
    bool profile;
} BenchmarkOptions;

typedef struct BenchmarkConfig
{
    bool useSpatialIndex;
    bool useExecutor;
} BenchmarkConfig;

static const BenchmarkConfig BENCHMARK_CONFIGS[BENCHMARK_CONFIG_COUNT] = {
    {.useSpatialIndex = false, .useExecutor = false},
    {.useSpatialIndex = false, .useExecutor = true},
    {.useSpatialIndex = true, .useExecutor = false},
    {.useSpatialIndex = true, .useExecutor = true},
};

typedef struct BenchmarkSimulation
{
    SimulationGroundProvider ground;
    World *world;
    SimulationCubeField field;
    double spawn[3];
    double windowSeconds;
    double totalSeconds;
    VoxelRigidStepProfile profile;
    double stageSeconds[VOXEL_RIGID_PROFILE_STAGE_COUNT];
    uint32_t maxSolverBatchCount;
    uint32_t maxSolverBatchSize;
    uint32_t maxSolverOverflowContacts;
    bool timerFailed;
} BenchmarkSimulation;

#if defined(_WIN32)
static LARGE_INTEGER timerFrequency;
#endif

static bool TimerInitialize(void)
{
#if defined(_WIN32)
    return QueryPerformanceFrequency(&timerFrequency) != 0 && timerFrequency.QuadPart > 0;
#else
    struct timespec value = {0};
    return clock_gettime(CLOCK_MONOTONIC, &value) == 0;
#endif
}

static bool TimerRead(double *outSeconds)
{
#if defined(_WIN32)
    LARGE_INTEGER value;
    if (QueryPerformanceCounter(&value) == 0)
    {
        return false;
    }
    *outSeconds = (double)value.QuadPart / (double)timerFrequency.QuadPart;
#else
    struct timespec value = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return false;
    }
    *outSeconds = (double)value.tv_sec + (double)value.tv_nsec * 1.0e-9;
#endif
    return true;
}

static double ProfileClock(void *context)
{
    BenchmarkSimulation *simulation = context;
    double seconds = 0.0;
    if (!TimerRead(&seconds))
    {
        simulation->timerFailed = true;
    }
    return seconds;
}

static bool InitializeSimulation(BenchmarkSimulation *simulation, bool useSpatialIndex,
                                  const BenchmarkOptions *options,
                                  const LaiueTaskExecutor *executor)
{
    SimulationGroundProviderInit(&simulation->ground);
    WorldBaseProvider provider = {0};
    SimulationGroundProviderBind(&simulation->ground, &provider);
    simulation->world = WorldCreate(&provider);
    if (simulation->world == NULL || !SimulationCubeFieldInit(&simulation->field))
    {
        return false;
    }
    simulation->field.useSpatialIndex = useSpatialIndex;
    simulation->field.stepOptions.solverOrder = options->solverOrder;
    simulation->field.stepOptions.executor = executor;
    if (options->profile)
    {
        simulation->profile.structSize = sizeof(simulation->profile);
        simulation->field.stepOptions.profile = &simulation->profile;
        simulation->field.stepOptions.clockSeconds = ProfileClock;
        simulation->field.stepOptions.clockContext = simulation;
    }
    simulation->spawn[0] = 0.5;
    simulation->spawn[1] = 0.5;
    simulation->spawn[2] = (double)SimulationGroundLocalLevel(&simulation->ground) + 1.0 + 18.0;
    return true;
}

static void ReleaseSimulation(BenchmarkSimulation *simulation)
{
    SimulationCubeFieldRelease(&simulation->field);
    if (simulation->world != NULL)
    {
        WorldDestroy(simulation->world);
        simulation->world = NULL;
    }
}

static uint64_t DoubleBits(double value)
{
    union
    {
        double floating;
        uint64_t bits;
    } representation = {.floating = value};
    return representation.bits;
}

static bool SameDoubles(const double *left, const double *right, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (DoubleBits(left[index]) != DoubleBits(right[index]))
        {
            return false;
        }
    }
    return true;
}

static bool SameCoordinate(const InfiniteCoord *left, const InfiniteCoord *right)
{
    if (left->sign != right->sign || left->limbCount != right->limbCount)
    {
        return false;
    }
    for (uint32_t limb = 0u; limb < left->limbCount; ++limb)
    {
        if (left->limbs[limb] != right->limbs[limb])
        {
            return false;
        }
    }
    return true;
}

static bool SameBody(const VoxelRigidBody *left, const VoxelRigidBody *right)
{
    if (left->stableId != right->stableId || left->active != right->active ||
        left->sleeping != right->sleeping || left->sleepCounter != right->sleepCounter ||
        !SameDoubles(left->orientation, right->orientation, 4u) ||
        !SameDoubles(left->halfExtent, right->halfExtent, 3u) ||
        !SameDoubles(left->inverseInertia, right->inverseInertia, 3u) ||
        DoubleBits(left->inverseMass) != DoubleBits(right->inverseMass) ||
        DoubleBits(left->restitution) != DoubleBits(right->restitution) ||
        DoubleBits(left->friction) != DoubleBits(right->friction))
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!SameCoordinate(&left->position[axis], &right->position[axis]) ||
            !SameCoordinate(&left->linearVelocity[axis], &right->linearVelocity[axis]) ||
            !SameCoordinate(&left->angularVelocity[axis], &right->angularVelocity[axis]))
        {
            return false;
        }
    }
    return true;
}

static bool SameSettings(const VoxelRigidStepSettings *left,
                         const VoxelRigidStepSettings *right)
{
    return SameDoubles(left->gravity, right->gravity, 3u) &&
           left->solverIterations == right->solverIterations &&
           DoubleBits(left->penetrationCorrection) == DoubleBits(right->penetrationCorrection) &&
           DoubleBits(left->penetrationSlop) == DoubleBits(right->penetrationSlop) &&
           DoubleBits(left->sleepLinearSpeed) == DoubleBits(right->sleepLinearSpeed) &&
           DoubleBits(left->sleepAngularSpeed) == DoubleBits(right->sleepAngularSpeed) &&
           left->sleepFrames == right->sleepFrames;
}

// Compare semantic state, not pointers, structure padding or rebuildable index
// topology. Every bigint limb and floating-point bit (including -0) is checked.
// Opaque impulse-cache storage is not interpreted: its public contact counters
// and its effect on the complete body state are checked after every tick.
static bool SameSimulation(const SimulationCubeField *left, const SimulationCubeField *right)
{
    if (left->tickCount != right->tickCount || left->count != right->count ||
        left->capacity != right->capacity || left->spawnCounter != right->spawnCounter ||
        left->spawnPhase != right->spawnPhase || left->randomState != right->randomState ||
        left->nextStableId != right->nextStableId || left->failed != right->failed ||
        left->lastContactCount != right->lastContactCount ||
        left->contactCache.contactCount != right->contactCache.contactCount ||
        left->contactCache.matchedContactCount != right->contactCache.matchedContactCount ||
        !SameDoubles(left->spawnDirection, right->spawnDirection, 2u) ||
        DoubleBits(left->stepAccumulator) != DoubleBits(right->stepAccumulator) ||
        left->stepOptions.solverOrder != right->stepOptions.solverOrder ||
        !SameSettings(&left->settings, &right->settings))
    {
        fputs("spawner or solver state differs\n", stderr);
        return false;
    }
    for (uint32_t index = 0u; index < left->count; ++index)
    {
        if (!SameBody(&left->bodies[index], &right->bodies[index]))
        {
            BENCHMARK_FPRINTF(stderr, "body mismatch: slot=%" PRIu32 " stable_id=%" PRIu64 "\n",
                              index, left->bodies[index].stableId);
            return false;
        }
    }
    return true;
}

static bool TimedAdvance(BenchmarkSimulation *simulation)
{
    // Clear outside timing: a tick with no bodies can legitimately skip StepEx.
    VoxelRigidStepProfile emptyProfile = {0};
    emptyProfile.structSize = sizeof(emptyProfile);
    simulation->profile = emptyProfile;
    double begin = 0.0;
    double end = 0.0;
    if (!TimerRead(&begin))
    {
        return false;
    }
    bool advanced = SimulationCubeFieldAdvanceTick(&simulation->field, simulation->world,
                                                   simulation->spawn);
    if (!TimerRead(&end) || end < begin || !advanced || simulation->timerFailed)
    {
        return false;
    }
    double elapsed = end - begin;
    simulation->windowSeconds += elapsed;
    simulation->totalSeconds += elapsed;
    // Collect diagnostic values only after the measured AdvanceTick interval.
    for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
    {
        double seconds = simulation->profile.seconds[stage];
        if (!isfinite(seconds) || seconds < 0.0)
        {
            return false;
        }
        simulation->stageSeconds[stage] += seconds;
    }
    if (simulation->profile.solverBatchCount > simulation->maxSolverBatchCount)
    {
        simulation->maxSolverBatchCount = simulation->profile.solverBatchCount;
    }
    if (simulation->profile.solverMaxBatchSize > simulation->maxSolverBatchSize)
    {
        simulation->maxSolverBatchSize = simulation->profile.solverMaxBatchSize;
    }
    if (simulation->profile.solverOverflowContacts > simulation->maxSolverOverflowContacts)
    {
        simulation->maxSolverOverflowContacts = simulation->profile.solverOverflowContacts;
    }
    return true;
}

static bool ParsePositive(const char *text, uint32_t maximum, uint32_t *outValue)
{
    uint32_t value = 0u;
    if (*text == '\0')
    {
        return false;
    }
    for (; *text != '\0'; ++text)
    {
        if (*text < '0' || *text > '9')
        {
            return false;
        }
        uint32_t digit = (uint32_t)(*text - '0');
        if (digit > maximum || value > (maximum - digit) / 10u)
        {
            return false;
        }
        value = value * 10u + digit;
    }
    if (value == 0u)
    {
        return false;
    }
    *outValue = value;
    return true;
}

static bool ParseOptions(int argc, char **argv, BenchmarkOptions *options)
{
    options->ticks = BENCHMARK_DEFAULT_TICKS;
    options->threads = BENCHMARK_DEFAULT_THREADS;
    options->solverOrder = VOXEL_RIGID_SOLVER_CANONICAL;
    bool ticksSeen = false;
    bool solverSeen = false;
    bool threadsSeen = false;
    for (int index = 1; index < argc; ++index)
    {
        const char *argument = argv[index];
        if (strncmp(argument, "--ticks=", 8u) == 0)
        {
            if (ticksSeen || !ParsePositive(argument + 8, BENCHMARK_MAX_TICKS, &options->ticks))
            {
                return false;
            }
            ticksSeen = true;
        }
        else if (strncmp(argument, "--threads=", 10u) == 0)
        {
            if (threadsSeen || !ParsePositive(argument + 10, 64u, &options->threads))
            {
                return false;
            }
            threadsSeen = true;
        }
        else if (strncmp(argument, "--solver=", 9u) == 0)
        {
            if (solverSeen)
            {
                return false;
            }
            if (strcmp(argument + 9, "canonical") == 0)
            {
                options->solverOrder = VOXEL_RIGID_SOLVER_CANONICAL;
            }
            else if (strcmp(argument + 9, "colored") == 0)
            {
                options->solverOrder = VOXEL_RIGID_SOLVER_COLORED;
            }
            else
            {
                return false;
            }
            solverSeen = true;
        }
        else if (strcmp(argument, "--profile") == 0 && !options->profile)
        {
            options->profile = true;
        }
        else
        {
            return false;
        }
    }
    return true;
}

static void PrintProfile(const BenchmarkSimulation *simulation, const char *label, uint32_t ticks)
{
    static const char *const stageNames[VOXEL_RIGID_PROFILE_STAGE_COUNT] = {
        "order", "forces", "bounds", "broadphase", "wake", "world_contacts", "body_contacts",
        "prepare", "warm_start", "schedule", "solve", "integrate", "sleep", "store"};
    for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
    {
        printf("profile,%s,%s,%.6f\n", label, stageNames[stage],
               simulation->stageSeconds[stage] * 1000.0 / (double)ticks);
    }
    printf("schedule %s max_batches=%" PRIu32 " max_batch_runs=%" PRIu32
           " max_overflow_contacts=%" PRIu32 "\n", label, simulation->maxSolverBatchCount,
           simulation->maxSolverBatchSize, simulation->maxSolverOverflowContacts);
}

int main(int argc, char **argv)
{
    BenchmarkOptions options = {0};
    if (!ParseOptions(argc, argv, &options))
    {
        fputs("usage: simulation_of_sins_physics_benchmark [--ticks=1..16384] "
              "[--threads=1..64] [--solver=canonical|colored] [--profile]\n", stderr);
        return 2;
    }
    if (!TimerInitialize())
    {
        fputs("monotonic timer unavailable\n", stderr);
        return 1;
    }
    BenchmarkSimulation simulations[BENCHMARK_CONFIG_COUNT] = {{0}};
    char labels[BENCHMARK_CONFIG_COUNT][24];
    for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
    {
        (void)snprintf(labels[config], sizeof(labels[config]), "%s_%s",
                       BENCHMARK_CONFIGS[config].useSpatialIndex ? "tree" : "grid",
                       BENCHMARK_CONFIGS[config].useExecutor ? "parallel" : "serial");
    }
    LaiueTaskPool *pool = NULL;
    LaiueTaskExecutor executor = {0};
    executor.structSize = sizeof(executor);
    int result = 1;
    if (options.threads >= 2u)
    {
        pool = LaiueTaskPoolCreate(options.threads);
        if (pool == NULL || !LaiueTaskPoolGetExecutor(pool, &executor))
        {
            fputs("task pool initialization failed\n", stderr);
            goto cleanup;
        }
    }
    for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
    {
        const LaiueTaskExecutor *configExecutor =
            BENCHMARK_CONFIGS[config].useExecutor && pool != NULL ? &executor : NULL;
        if (!InitializeSimulation(&simulations[config], BENCHMARK_CONFIGS[config].useSpatialIndex,
                                  &options, configExecutor) ||
            !SameSimulation(&simulations[0].field, &simulations[config].field))
        {
            fputs("simulation initialization failed\n", stderr);
            goto cleanup;
        }
    }
    printf("paired spawner: seed=0x%016" PRIx64 " ticks=%" PRIu32
           " tick_seconds=%.9f solver_iterations=%" PRIu32
           " solver=%s threads=%" PRIu32 " logical_processors=%" PRIu32 " profile=%u\n",
           simulations[0].field.randomState, options.ticks, SIMULATION_CUBE_STEP_SECONDS,
           simulations[0].field.settings.solverIterations,
           options.solverOrder == VOXEL_RIGID_SOLVER_COLORED ? "colored" : "canonical",
           options.threads, LaiueTaskLogicalProcessorCount(), options.profile ? 1u : 0u);
    printf("tick,bodies,awake,%s_ms_per_tick,%s_ms_per_tick,%s_ms_per_tick,%s_ms_per_tick,"
           "grid_candidates,tree_candidates,contacts\n",
           labels[0], labels[1], labels[2], labels[3]);
    uint32_t windowTicks = 0u;
    for (uint32_t tick = 1u; tick <= options.ticks; ++tick)
    {
        // Rotating the first runner balances systematic cache/clock drift
        // across all four configurations.
        uint32_t start = tick % BENCHMARK_CONFIG_COUNT;
        for (uint32_t offset = 0u; offset < BENCHMARK_CONFIG_COUNT; ++offset)
        {
            uint32_t config = (start + offset) % BENCHMARK_CONFIG_COUNT;
            if (!TimedAdvance(&simulations[config]))
            {
                BENCHMARK_FPRINTF(stderr, "physics step or timer failed at tick=%" PRIu32
                                  " config=%s failed=%u\n", tick, labels[config],
                                  simulations[config].field.failed ? 1u : 0u);
                goto cleanup;
            }
        }
        // Validation is deliberately outside every timed interval.
        for (uint32_t config = 1u; config < BENCHMARK_CONFIG_COUNT; ++config)
        {
            if (!SameSimulation(&simulations[0].field, &simulations[config].field))
            {
                BENCHMARK_FPRINTF(stderr, "exact replay mismatch at tick=%" PRIu32
                                  " config=%s\n", tick, labels[config]);
                goto cleanup;
            }
        }
        ++windowTicks;
        if (windowTicks == BENCHMARK_REPORT_TICKS || tick == options.ticks)
        {
            printf("%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%.6f,%.6f,%.6f,%.6f,"
                   "%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                   tick, simulations[0].field.count,
                   SimulationCubeFieldAwakeCount(&simulations[0].field),
                   simulations[0].windowSeconds * 1000.0 / (double)windowTicks,
                   simulations[1].windowSeconds * 1000.0 / (double)windowTicks,
                   simulations[2].windowSeconds * 1000.0 / (double)windowTicks,
                   simulations[3].windowSeconds * 1000.0 / (double)windowTicks,
                   simulations[0].field.lastCandidatePairCount,
                   simulations[2].field.lastCandidatePairCount,
                   simulations[0].field.lastContactCount);
            fflush(stdout);
            for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
            {
                simulations[config].windowSeconds = 0.0;
            }
            windowTicks = 0u;
        }
    }
    printf("PASS exact_state_equal_ticks=%" PRIu32 " bodies=%" PRIu32, options.ticks,
           simulations[0].field.count);
    for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
    {
        printf(" %s_total_ms=%.3f %s_avg_ms=%.6f", labels[config],
               simulations[config].totalSeconds * 1000.0, labels[config],
               simulations[config].totalSeconds * 1000.0 / (double)options.ticks);
    }
    printf("\n");
    if (options.profile)
    {
        puts("profile,mode,stage,ms_per_tick");
        for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
        {
            PrintProfile(&simulations[config], labels[config], options.ticks);
        }
    }
    result = 0;

cleanup:
    for (uint32_t config = 0u; config < BENCHMARK_CONFIG_COUNT; ++config)
    {
        ReleaseSimulation(&simulations[config]);
    }
    LaiueTaskPoolDestroy(pool);
    return result;
}
