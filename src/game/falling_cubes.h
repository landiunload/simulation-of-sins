#ifndef SIMULATION_OF_SINS_GAME_FALLING_CUBES_H
#define SIMULATION_OF_SINS_GAME_FALLING_CUBES_H

#include "physics/rigid_body.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;

// Поле не удаляет тела по лимиту. Стандартный спавнер кубов не ограничен;
// сценарий может подключить свой callback и предел числа успешных спавнов.
// Временный массив растёт до 1,5x, persistent-кэши сохраняют свои пороги.

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
    // Форма каждого тела, выровненная по индексу: обычный куб — boxCount == 0,
    // постройка — дочерние коробки относительно COM. Массивы растут вместе.
    VoxelRigidCompoundShape *shapes;
    // Borrowed game spawner. NULL uses the original cube spawner. The callback
    // appends one body on success; AdvanceTick owns its spawnCounter increment.
    bool (*spawnBody)(void *context, const double position[3]);
    void *spawnContext;
    // Zero means unlimited. Counts lifetime spawns, not surviving fragments.
    uint64_t spawnLimit;
    uint64_t nextStableId;
    uint64_t spawnCounter;
    uint64_t randomState;
    uint64_t tickCount;
    double spawnDirection[2];
    double stepAccumulator;
    // Буфер контактов/решателя принадлежит игре; числа bigint имеют своё хранилище.
    void *scratch;
    // Persistent solver initial guesses belong to the simulation state, not
    // a render frame. Reset only when invalidating the collision snapshot.
    VoxelRigidContactCache contactCache;
    // Spatial index is rebuildable derived data, unlike the impulse history.
    VoxelRigidBroadphase broadphase;
    // Borrowed execution/profile interfaces. The field always supplies its own
    // contact cache and optional broadphase when calling the engine.
    VoxelRigidStepOptions stepOptions;
    VoxelRigidStepSettings settings;
    uint32_t capacity;
    uint32_t count;
    uint32_t spawnPhase;
    uint32_t lastCandidatePairCount;
    uint32_t lastContactCount;
    uint32_t scratchBytes;
    // Ошибка шага может оставить частично обновлённые тела: повторять его
    // нельзя. Ошибка фиксируется до загрузки снимка или новой инициализации.
    bool failed;
    // Opt-in: the uniform spawner currently performs better with the grid.
    bool useSpatialIndex;
    bool spawningStopped;
} SimulationCubeField;

bool SimulationCubeFieldInit(SimulationCubeField *field);
bool SimulationCubeFieldInitWithSeed(SimulationCubeField *field, uint64_t seed);
void SimulationCubeFieldRelease(SimulationCubeField *field);

// Сдвигает кубы вместе с началом локальных координат (rebasing).
void SimulationCubeFieldRebase(SimulationCubeField *field, const int64_t blockShift[3]);

// Слоты тел для составных построек. Поле владеет массивом тел и параллельным
// массивом форм; удаление сдвигает индексы последующих слотов. Добавление
// проходит через ReserveBodies и PrepareSolver до публикации. RemoveBody освобождает
// тело и сдвигает массив вниз, сохраняя порядок; warm-start и broadphase
// сбрасываются явно, потому что stableId-якоря и индексы сместились.
bool SimulationCubeFieldRemoveBody(SimulationCubeField *field, uint32_t index);
// Явно забывает warm-start импульсы и broadphase после смены топологии/формы.
void SimulationCubeFieldInvalidateSolver(SimulationCubeField *field);
// Истина, когда слот — обычная коробка (boxCount == 0), а не составное тело.
bool SimulationCubeFieldBodyIsBox(const SimulationCubeField *field, uint32_t index);

// Суммарное число примитивов шага: каждое тело считается минимум за один,
// составное добавляет boxCount - 1. false при переполнении uint32. По этому
// числу движок считает честный бюджет контактов (primitiveCount * 16).
bool SimulationCubeFieldPrimitiveCount(const SimulationCubeField *field, uint32_t *outCount);

// Резервирует транзиентную ёмкость тел/форм и broadphase минимум под required
// тел, не меняя count и не трогая contact cache. Нужна, чтобы подготовить
// слоты до публикации правки: отказ не теряет ни тела, ни warm-start.
bool SimulationCubeFieldReserveBodies(SimulationCubeField *field, uint32_t required);

// План замены solver-буферов для ещё не опубликованной топологии. Новые
// scratch и contact cache выделяются отдельно и не подменяют текущие до
// Commit, поэтому отклонённая правка не теряет warm-start. Commit переключает
// буферы и освобождает старые; Abort освобождает только новые.
typedef struct SimulationCubeFieldSolverPlan
{
    void *scratch;
    uint32_t scratchBytes;
    void *cacheStorage;
    uint32_t cacheCapacity;
    uint32_t cacheBytes;
} SimulationCubeFieldSolverPlan;

bool SimulationCubeFieldPrepareSolver(SimulationCubeField *field, uint32_t requiredBodyCount,
                                      uint32_t requiredPrimitives, SimulationCubeFieldSolverPlan *plan);
void SimulationCubeFieldCommitSolver(SimulationCubeField *field, SimulationCubeFieldSolverPlan *plan);
void SimulationCubeFieldAbortSolver(SimulationCubeFieldSolverPlan *plan);

// Ближайший куб вдоль луча. direction обязан быть единичным: длина луча не
// нормируется, потому что камера уже отдаёт единичный вектор, а лишний sqrt
// тянул бы libm в переносимое ядро. false — в пределах maximumDistance куба
// нет. outIndex и outDistance пишутся только при попадании.
bool SimulationCubeFieldRaycast(const SimulationCubeField *field, const double origin[3],
                                const double direction[3], double maximumDistance,
                                uint32_t *outIndex, double *outDistance);

// Импульс по радиусу: тела в шаре radius вокруг center получают прибавку
// скорости вдоль единичного direction. Чем дальше тело от center, тем меньше
// прибавка; на границе радиуса она ровно нулевая. Прибавка домножается на
// обратную массу, поэтому параметр — импульс, а не готовое изменение скорости.
// Спящие тела пробуждаются. Возвращает число затронутых тел.
uint32_t SimulationCubeFieldApplyRadialImpulse(SimulationCubeField *field,
                                               const double center[3],
                                               const double direction[3],
                                               double radius, double impulse);

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

// Ёмкость persistent-буфера, которой хватает на required элементов. При
// нехватке удваивается, поэтому вставка n элементов требует O(log n)
// перевыделений, а не O(n): без этого буфер инстансов пересоздавался бы на
// каждом кадре, в котором появился хоть один куб. Возврат всегда >= required.
uint32_t SimulationGrownCapacity(uint32_t current, uint32_t required);

// Привязка и поворот куба для инстанса рендера: origin уже смещён на
// повёрнутый полуразмер, потому что меш растёт от нуля в плюс, а тело
// задано центром. Составное тело — не куб: для него возвращается false,
// постройки рисуются ConstructBlockPlacement. false также означает, что
// тело улетело за пределы локальных координат.
bool SimulationCubeFieldPlacement(const SimulationCubeField *field, uint32_t index,
                                  double outOrigin[3], float outRotation[4]);

#endif
