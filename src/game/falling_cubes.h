#ifndef SIMULATION_OF_SINS_GAME_FALLING_CUBES_H
#define SIMULATION_OF_SINS_GAME_FALLING_CUBES_H

#include "physics/rigid_body.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;

// Кубы не исчезают и не ограничены числом: массив растёт удвоением,
// появление прекращается только если память кончилась. Симуляция
// перестаёт успевать за реальным временем примерно на тысяче тел — это
// известная цена, а не предел.

// Как и просили — каждые десять миллисекунд.
#define SIMULATION_CUBE_SPAWN_INTERVAL_SECONDS 0.01

// Шаг решателя задан движком: он равен 2^-VOXEL_RIGID_STEP_SHIFT секунды,
// потому что умножение на степень двойки не теряет ни бита.
#define SIMULATION_CUBE_STEP_SECONDS (1.0 / 128.0)

#define SIMULATION_CUBE_EXTENT 0.9
#define SIMULATION_CUBE_HALF_EXTENT 0.45

typedef struct SimulationCubeField
{
    VoxelRigidBody *bodies;
    uint32_t capacity;
    VoxelRigidStepSettings settings;
    uint32_t count;
    uint64_t nextStableId;
    uint64_t spawnCounter;
    uint64_t randomState;
    double spawnAccumulator;
    double stepAccumulator;
    uint32_t lastCandidatePairCount;
    uint32_t lastContactCount;
    // Буфер шага принадлежит игре: physics ничего не выделяет сам.
    void *scratch;
    uint32_t scratchBytes;
} SimulationCubeField;

bool SimulationCubeFieldInit(SimulationCubeField *field);
void SimulationCubeFieldRelease(SimulationCubeField *field);

// Сдвигает кубы вместе с началом локальных координат (rebasing).
void SimulationCubeFieldRebase(SimulationCubeField *field, const int64_t blockShift[3]);

// Кадр целиком: появление по расписанию и физика фиксированным шагом.
void SimulationCubeFieldUpdate(SimulationCubeField *field, World *world,
                               const double spawnPosition[3], double deltaSeconds);

uint32_t SimulationCubeFieldCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldAwakeCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldLastCandidatePairCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldLastContactCount(const SimulationCubeField *field);

// Привязка и поворот куба для инстанса рендера: origin уже смещён на
// повёрнутый полуразмер, потому что меш растёт от нуля в плюс, а тело
// задано центром. false означает, что куб улетел за пределы локальных
// координат — рисовать его нечем.
bool SimulationCubeFieldPlacement(const SimulationCubeField *field, uint32_t index,
                                  double outOrigin[3], float outRotation[4]);

#endif
