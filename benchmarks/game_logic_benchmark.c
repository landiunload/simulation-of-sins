#include "game/frame_timing.h"
#include "game/rebase_policy.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define BENCHMARK_INPUT_COUNT 4096U
#define BENCHMARK_SAMPLE_COUNT 9U
#define BENCHMARK_DEFAULT_ITERATIONS 5000000ULL
#define BENCHMARK_MAX_ITERATIONS 100000000ULL

typedef struct BenchmarkInput
{
    double position[3];
    double previousSeconds;
    double currentSeconds;
    int64_t block;
} BenchmarkInput;

typedef struct BenchmarkMeasurement
{
    double baselineMilliseconds;
    double workloadMilliseconds;
    uint64_t checksum;
} BenchmarkMeasurement;

static BenchmarkInput benchmarkInputs[BENCHMARK_INPUT_COUNT];
static volatile uint64_t benchmarkSink;

static uint64_t Mix(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static uint64_t NextRandom(uint64_t *state)
{
    *state += UINT64_C(0x9e3779b97f4a7c15);
    return Mix(*state);
}

static double Fraction(uint64_t value)
{
    return (double)(value & UINT64_C(0xfffff)) / 1048576.0;
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

static uint64_t Accumulate(uint64_t checksum, uint64_t value)
{
    return (checksum ^ value) * UINT64_C(0x100000001b3);
}

static int64_t SignedFromBits(uint64_t value)
{
    if (value <= INT64_MAX)
    {
        return (int64_t)value;
    }
    return -1 - (int64_t)(UINT64_MAX - value);
}

static void InitializeInputs(uint64_t seed)
{
    uint64_t state = seed;
    for (uint32_t index = 0; index < BENCHMARK_INPUT_COUNT; ++index)
    {
        BenchmarkInput *input = &benchmarkInputs[index];
        double fraction = Fraction(NextRandom(&state));
        double sign = (index & 1U) == 0U ? 1.0 : -1.0;
        switch (index & 3U)
        {
        case 0U:
            input->position[0] = sign * (64.0 + fraction);
            break;
        case 1U:
            input->position[0] = 513.0 + fraction;
            break;
        case 2U:
            input->position[0] = -513.0 - fraction;
            break;
        default:
            input->position[0] = sign * (4096.0 + fraction);
            break;
        }
        input->position[1] = sign * (1025.0 + Fraction(NextRandom(&state)));
        input->position[2] = sign * (257.0 + Fraction(NextRandom(&state)));
        input->previousSeconds = 1000.0 + Fraction(NextRandom(&state));
        input->currentSeconds = input->previousSeconds + 0.001 + Fraction(NextRandom(&state)) * 0.2;
        input->block = SignedFromBits(NextRandom(&state));
    }
}

static uint64_t RunBaseline(uint64_t iterations)
{
    uint64_t checksum = UINT64_C(0x243f6a8885a308d3);
    for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    {
        uint32_t index = (uint32_t)iteration & (BENCHMARK_INPUT_COUNT - 1U);
        const BenchmarkInput *input = &benchmarkInputs[index];
        checksum = Accumulate(checksum, DoubleBits(input->position[0]));
        checksum = Accumulate(checksum, DoubleBits(input->position[1]));
        checksum = Accumulate(checksum, DoubleBits(input->position[2]));
        checksum = Accumulate(checksum, DoubleBits(input->currentSeconds));
        checksum = Accumulate(checksum, (uint64_t)input->block);
        checksum = Accumulate(checksum, iteration);
        checksum = Accumulate(checksum, (uint64_t)index);
        checksum = Accumulate(checksum, (uint64_t)input->block ^ iteration);
        checksum = Accumulate(checksum, iteration & 1U);
    }
    return checksum;
}

static uint64_t RunWorkload(uint64_t iterations)
{
    uint64_t checksum = UINT64_C(0x13198a2e03707344);
    for (uint64_t iteration = 0; iteration < iterations; ++iteration)
    {
        const BenchmarkInput *input =
            &benchmarkInputs[(uint32_t)iteration & (BENCHMARK_INPUT_COUNT - 1U)];
        double position[3] = {
            input->position[0],
            input->position[1],
            input->position[2],
        };
        SimulationOriginShift shift;
        if (!SimulationOriginShiftPlan(position, &shift))
        {
            return 0;
        }
        SimulationOriginShiftApply(position, &shift);

        int64_t chunk = SimulationBlockToChunkFloor(input->block);
        float delta = SimulationFrameDeltaSeconds(input->previousSeconds, input->currentSeconds);
        checksum = Accumulate(checksum, (uint64_t)chunk);
        checksum = Accumulate(checksum, (uint64_t)shift.block[0]);
        checksum = Accumulate(checksum, (uint64_t)shift.block[1]);
        checksum = Accumulate(checksum, (uint64_t)shift.block[2]);
        checksum = Accumulate(checksum, shift.required ? 1U : 0U);
        checksum = Accumulate(checksum, DoubleBits(position[0]));
        checksum = Accumulate(checksum, DoubleBits(position[1]));
        checksum = Accumulate(checksum, DoubleBits(position[2]));
        checksum = Accumulate(checksum, DoubleBits((double)delta));
    }
    return checksum;
}

static double MeasureMilliseconds(uint64_t (*work)(uint64_t), uint64_t iterations,
                                  uint64_t *checksum)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER begin;
    LARGE_INTEGER end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    *checksum = work(iterations);
    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart - begin.QuadPart) * 1000.0 / (double)frequency.QuadPart;
}

static BenchmarkMeasurement MeasureSample(uint64_t iterations, bool workloadFirst)
{
    BenchmarkMeasurement measurement = {0};
    uint64_t baselineChecksum = 0;
    if (workloadFirst)
    {
        measurement.workloadMilliseconds =
            MeasureMilliseconds(RunWorkload, iterations, &measurement.checksum);
        measurement.baselineMilliseconds =
            MeasureMilliseconds(RunBaseline, iterations, &baselineChecksum);
    }
    else
    {
        measurement.baselineMilliseconds =
            MeasureMilliseconds(RunBaseline, iterations, &baselineChecksum);
        measurement.workloadMilliseconds =
            MeasureMilliseconds(RunWorkload, iterations, &measurement.checksum);
    }
    benchmarkSink = measurement.checksum ^ baselineChecksum;
    return measurement;
}

static void Sort(double values[BENCHMARK_SAMPLE_COUNT])
{
    for (uint32_t index = 1; index < BENCHMARK_SAMPLE_COUNT; ++index)
    {
        double value = values[index];
        uint32_t insertion = index;
        while (insertion > 0U && values[insertion - 1U] > value)
        {
            values[insertion] = values[insertion - 1U];
            --insertion;
        }
        values[insertion] = value;
    }
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static bool ParseUnsigned(const char *text, uint64_t *outValue)
{
    if (text == NULL || outValue == NULL || *text < '0' || *text > '9')
    {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0')
    {
        return false;
    }
    *outValue = (uint64_t)value;
    return true;
}

int main(int argc, char **argv)
{
    uint64_t iterations = BENCHMARK_DEFAULT_ITERATIONS;
    uint64_t seed = UINT64_C(0x5eed1234);
    if (argc > 3 ||
        (argc > 1 && (!ParseUnsigned(argv[1], &iterations) || iterations == 0 ||
                      iterations > BENCHMARK_MAX_ITERATIONS)) ||
        (argc > 2 && !ParseUnsigned(argv[2], &seed)))
    {
        fputs("usage: simulation_of_sins_game_benchmark "
              "[iterations: 1..100000000] [uint64 seed]\n",
              stderr);
        return 2;
    }
    InitializeInputs(seed);

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    DWORD_PTR previousThreadMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0)
    {
        DWORD_PTR selectedCpu = processMask & (~processMask + 1U);
        previousThreadMask = SetThreadAffinityMask(GetCurrentThread(), selectedCpu);
    }

    (void)MeasureSample(iterations, false);
    double baseline[BENCHMARK_SAMPLE_COUNT];
    double workload[BENCHMARK_SAMPLE_COUNT];
    double net[BENCHMARK_SAMPLE_COUNT];
    double netDeviation[BENCHMARK_SAMPLE_COUNT];
    uint64_t checksum = 0;
    for (uint32_t sample = 0; sample < BENCHMARK_SAMPLE_COUNT; ++sample)
    {
        BenchmarkMeasurement measurement = MeasureSample(iterations, (sample & 1U) != 0U);
        baseline[sample] = measurement.baselineMilliseconds;
        workload[sample] = measurement.workloadMilliseconds;
        net[sample] = measurement.workloadMilliseconds - measurement.baselineMilliseconds;
        if (sample == 0U)
        {
            checksum = measurement.checksum;
        }
        else if (checksum != measurement.checksum)
        {
            fputs("benchmark checksum changed between samples\n", stderr);
            return 1;
        }
    }
    if (previousThreadMask != 0)
    {
        SetThreadAffinityMask(GetCurrentThread(), previousThreadMask);
    }

    Sort(baseline);
    Sort(workload);
    Sort(net);
    const double netMedian = net[BENCHMARK_SAMPLE_COUNT / 2U];
    for (uint32_t sample = 0; sample < BENCHMARK_SAMPLE_COUNT; ++sample)
    {
        netDeviation[sample] = Absolute(net[sample] - netMedian);
    }
    Sort(netDeviation);
    printf("iterations=%" PRIu64 " samples=%u baseline_median_ms=%.3f "
           "workload_median_ms=%.3f net_median_ms=%.3f net_mad_ms=%.3f "
           "checksum=0x%016" PRIx64 "\n",
           iterations, BENCHMARK_SAMPLE_COUNT, baseline[BENCHMARK_SAMPLE_COUNT / 2U],
           workload[BENCHMARK_SAMPLE_COUNT / 2U], netMedian,
           netDeviation[BENCHMARK_SAMPLE_COUNT / 2U], checksum);
    return 0;
}
