#ifndef SIMULATION_OF_SINS_GAME_CONSTRUCT_H
#define SIMULATION_OF_SINS_GAME_CONSTRUCT_H

#include "game/falling_cubes.h"
#include "physics/rigid_body.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

// Физические постройки: тело — это набор блоков, а не одна коробка.
//
// Постройки живут в общем мире тел SimulationCubeField, рядом с обычными
// кубами. Слот поля — настоящее составное тело публичного solver: дочерние
// коробки жёстко связаны с одним VoxelRigidBody и получают вращение, момент
// и инерцию от общей COM. Поэтому постройку можно опрокинуть или разбить, а
// кубы и постройки сталкиваются друг с другом в одном fixed step.
//
// Блоки заданы в целочисленной сетке тела. Начало сетки — клетка первого
// блока; блок с local = (0,0,0) занимает клетку [origin, origin + 1] в момент
// спавна. Дальше поза тела (COM, поворот) читается только из поля, а не
// хранится в ConstructBody.
//
// Если удаление блока разрывает постройку на несвязные части, каждая часть
// становится отдельным телом. Пока блоки связаны по граням — движутся вместе.
//
// Числа тел и блоков не имеют игрового потолка: метаданные и массивы блоков
// растут в куче до доступной памяти/представления. Искусственный предел
// спавна (SIMULATION_CONSTRUCT_SPAWN_LIMIT) задаёт только сам спавнер сцены,
// а не систему построек.

typedef struct ConstructBlock
{
    // Клетка в сетке постройки. Может быть отрицательной: начало сетки —
    // это первый поставленный блок, а расширяться постройка вправе в обе
    // стороны.
    int32_t local[3];
    uint8_t material;
} ConstructBlock;

typedef struct ConstructBody
{
    bool active;
    // Уникальный идентификатор тела, он же stableId его слота в поле.
    uint64_t id;
    // Слот тела в SimulationCubeField (bodies и shapes). Поза читается
    // оттуда, здесь хранится только индекс.
    uint32_t bodyIndex;
    uint32_t blockCount;
    // AABB занятых блоков в сетке, в клетках. Нужен рендеру и проверкам.
    int32_t minimum[3];
    int32_t maximum[3];
    // COM тела в координатах сетки (для блока с local центр равен local+0.5).
    // Вместе с поворотом и COM из поля задаёт мировое положение блока.
    double localCOM[3];
    // Блоки и дочерние коробки выделяются отдельно и принадлежат телу.
    // Переезд метаданных (realloc ConstructSystem::bodies) их не двигает:
    // поле держит указатель только на shapeBoxes, и он остаётся валидным,
    // пока тело живо. Оба массива имеют ровно blockCount элементов; форма
    // поля владеет фактическим boxCount.
    // Exact non-overlapping partition of the block union; adjacent blocks may
    // share one collision box.
    ConstructBlock *blocks;
    VoxelRigidCompoundBox *shapeBoxes;
} ConstructBody;

typedef struct ConstructSystem
{
    World *world;
    // Поле заимствуется: оно владеет телами и живёт дольше системы.
    SimulationCubeField *field;
    // Метаданные тел. capacity — длина массива slots; активные тела лежат
    // в слотах [0, capacity) в детерминированном порядке наименьшего
    // свободного слота. Массивы blocks/shapeBoxes тел выделяются отдельно,
    // поэтому рост bodies не рвёт указатели, отданные полю.
    ConstructBody *bodies;
    uint32_t capacity;
} ConstructSystem;

typedef struct ConstructRaycastHit
{
    uint64_t bodyId;
    uint32_t blockIndex;
    double distance;
    // Грань входа в локальной сетке тела. Редактор ставит соседний блок по
    // этой нормали, поэтому она не переводится в мировые оси.
    int8_t normal[3];
} ConstructRaycastHit;

// Систему создаёт вызывающий нулево инициализированной и освобождает. World
// и field обязаны жить дольше системы. Перед повторным Init нужно вызвать
// ConstructSystemRelease; во время привязки поле держит адреса shapeBoxes,
// поэтому сами массивы формы нельзя освобождать раньше, чем тело уйдёт из
// поля, но метаданные bodies могут переезжать.
void ConstructSystemInit(ConstructSystem *system, World *world, SimulationCubeField *field);
// Отвязывает тела построек от поля и освобождает все метаданные, блоки и
// формы. Сохраняет world/field: после Reset систему можно привязать к тому же
// полю снова. Повторный вызов и вызов после SimulationCubeFieldRelease
// безопасны.
void ConstructSystemReset(ConstructSystem *system);
void ConstructSystemRelease(ConstructSystem *system);

// Создаёт тело из набора блоков. Блоки копируются; форма не сдвигается
// относительно мира, а COM и инерция считаются движком. Блоки обязаны быть
// уникальными, материал — не воздух, иначе false. outBodyId может быть NULL.
bool ConstructSpawn(ConstructSystem *system, const ConstructBlock *blocks, uint32_t count,
                    const double origin[3], uint64_t *outBodyId);

// Ставит блок на тело. Клетка обязана быть свободной и касаться уже
// существующего блока гранью. false — занято, не касается или не хватает
// памяти.
bool ConstructPlaceBlock(ConstructSystem *system, uint64_t bodyId, const int32_t local[3],
                         uint8_t material);

// Ломает блок. Возвращает false, если блока нет или не удалось подготовить
// осколки: тогда ничего не меняется и блоки не теряются. При разрыве
// связности тело распадается: часть с прежним id остаётся (компонента первого
// блока), отколовшиеся получают новые id по возрастанию. Их число — в
// outBodyCount, сами id — в outBodyIds, если буфер задан.
// outRemovedMaterial может быть NULL.
bool ConstructBreakBlock(ConstructSystem *system, uint64_t bodyId, const int32_t local[3],
                         uint64_t *outBodyIds, uint32_t bodyIdCapacity, uint32_t *outBodyCount,
                         uint8_t *outRemovedMaterial);

// Луч по постройкам: ближайший блок среди всех тел. Луч переводится в систему
// каждого тела, поэтому попадание честное и для повёрнутой постройки.
// normal — грань входа в локальной сетке тела.
bool ConstructRaycast(const ConstructSystem *system, const double origin[3],
                      const double direction[3], double maximumDistance,
                      ConstructRaycastHit *outHit);

// Привязка и поворот блока [0,1]^3 в мировых координатах. origin смещён на
// повёрнутый полуразмер, потому что меш растёт от нуля в плюс. bodyIndex —
// индекс в ConstructSystem::bodies, а не слот поля.
bool ConstructBlockPlacement(const ConstructSystem *system, uint32_t bodyIndex, uint32_t blockIndex,
                             double outOrigin[3], float outRotation[4]);

// Ничего не двигает: общий rebase тел делает SimulationCubeFieldRebase.
// Оставлена для совместимости вызова и всегда возвращает true.
bool ConstructRebase(ConstructSystem *system, const int64_t blockShift[3]);

uint32_t ConstructActiveBodyCount(const ConstructSystem *system);

#endif
