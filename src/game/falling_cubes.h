#ifndef SIMULATION_OF_SINS_GAME_FALLING_CUBES_H
#define SIMULATION_OF_SINS_GAME_FALLING_CUBES_H

#include "physics/rigid_body.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;

// Кубы не исчезают и не ограничены числом: массив растёт удвоением,
// появление прекращается только если память или диапазон API закончились.

// Номинально каждые 10 ms; появление квантуется до ближайшего будущего tick.
#define SIMULATION_CUBE_SPAWN_INTERVAL_SECONDS 0.01

// Шаг решателя задан движком: он равен 2^-VOXEL_RIGID_STEP_SHIFT секунды,
// потому что умножение на степень двойки не теряет ни бита.
#define SIMULATION_CUBE_TICKS_PER_SECOND (1u << VOXEL_RIGID_STEP_SHIFT)
#define SIMULATION_CUBE_STEP_SECONDS (1.0 / (double)SIMULATION_CUBE_TICKS_PER_SECOND)
#define SIMULATION_CUBE_SPAWNS_PER_SECOND 100u

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
    uint64_t tickCount;
    uint32_t spawnPhase;
    double spawnDirection[2];
    double stepAccumulator;
    // Ошибка шага может оставить частично обновлённые тела: повторять его
    // нельзя. Ошибка фиксируется до загрузки снимка или новой инициализации.
    bool failed;
    uint32_t lastCandidatePairCount;
    uint32_t lastContactCount;
    // Буфер шага принадлежит игре: physics ничего не выделяет сам.
    void *scratch;
    uint32_t scratchBytes;
    // Persistent solver initial guesses belong to the simulation state, not
    // a render frame. Reset only when invalidating the collision snapshot.
    VoxelRigidContactCache contactCache;
    // Spatial index is rebuildable derived data, unlike the impulse history.
    VoxelRigidBroadphase broadphase;
    // Opt-in: the uniform spawner currently performs better with the grid.
    bool useSpatialIndex;
    // Borrowed execution/profile interfaces. The field always supplies its own
    // contact cache and optional broadphase when calling the engine.
    VoxelRigidStepOptions stepOptions;
} SimulationCubeField;

bool SimulationCubeFieldInit(SimulationCubeField *field);
bool SimulationCubeFieldInitWithSeed(SimulationCubeField *field, uint64_t seed);
void SimulationCubeFieldRelease(SimulationCubeField *field);

// Сдвигает кубы вместе с началом локальных координат (rebasing).
void SimulationCubeFieldRebase(SimulationCubeField *field, const int64_t blockShift[3]);

// Авторитетный tick: одно и то же начальное состояние, seed и входные
// данные на каждом tick дают один и тот же порядок спавна и физических шагов.
// В replay вызывается напрямую, без времени кадра. false запрещает retry:
// при ошибке решателя состояние может быть обновлено частично.
bool SimulationCubeFieldAdvanceTick(SimulationCubeField *field, World *world,
                                    const double spawnPosition[3]);

// Адаптер времени кадра: выполняет не более 16 tick за вызов, но сохраняет
// долг. deltaSeconds == 0 позволяет догнать долг без добавления времени.
// Для меняющихся входов нужен AdvanceTick с входами, записанными по tick.
void SimulationCubeFieldUpdate(SimulationCubeField *field, World *world,
                               const double spawnPosition[3], double deltaSeconds);

uint32_t SimulationCubeFieldCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldAwakeCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldLastCandidatePairCount(const SimulationCubeField *field);
uint32_t SimulationCubeFieldLastContactCount(const SimulationCubeField *field);

// Ёмкость растущего буфера, которой хватает на required элементов. При
// нехватке удваивается, поэтому вставка n элементов требует O(log n)
// перевыделений, а не O(n): без этого буфер инстансов пересоздавался бы на
// каждом кадре, в котором появился хоть один куб. Возврат всегда >= required.
uint32_t SimulationGrownCapacity(uint32_t current, uint32_t required);

// Привязка и поворот куба для инстанса рендера: origin уже смещён на
// повёрнутый полуразмер, потому что меш растёт от нуля в плюс, а тело
// задано центром. false означает, что куб улетел за пределы локальных
// координат — рисовать его нечем.
bool SimulationCubeFieldPlacement(const SimulationCubeField *field, uint32_t index,
                                  double outOrigin[3], float outRotation[4]);

#endif
