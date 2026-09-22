# Составные постройки: рабочие заметки

Задача: заменить примитивную axis-aligned физику `src/game/construct.c`
настоящими составными телами публичного `physics/rigid_body.h`, не заводя
второй независимой симуляции. Обычные кубы и постройки обязаны решаться
вместе на общем fixed tick `1/128`.

## Зона записи

- `src/game/construct.c`, `src/game/construct.h`
- `src/game/falling_cubes.c`, `src/game/falling_cubes.h`
- `docs/compound_game_work.md`

Авторитетные изменения `src/app/application.c` и `tests/game_test.c` делает
root. Движок (`src/physics/rigid_body.[ch]`) правит отдельный агент; игровой
код использует только публичный контракт:

```c
typedef struct VoxelRigidCompoundBox { double center[3]; double halfExtent[3]; } VoxelRigidCompoundBox;
typedef struct VoxelRigidCompoundShape { const VoxelRigidCompoundBox *boxes; uint32_t boxCount; double inverseInertia[9]; } VoxelRigidCompoundShape;
bool VoxelRigidCompoundMassProperties(const VoxelRigidCompoundBox *boxes, uint32_t boxCount, double mass, double center[3], double halfExtent[3], double inverseInertia[9]);
bool VoxelRigidBodyStepCompoundEx(VoxelRigidBody *bodies, uint32_t bodyCount, const VoxelCollisionSource *collision, const VoxelRigidStepSettings *settings, void *scratch, uint32_t scratchBytes, const VoxelRigidStepOptions *options, const VoxelRigidCompoundShape *shapes);
```

`boxCount == 0` — прежний быстрый путь обычной коробки.

## Общий мир тел

`SimulationCubeField` владеет **единым** массивом `VoxelRigidBody` и
параллельным массивом `VoxelRigidCompoundShape`. Обычные кубы — тела с
`boxCount == 0`; постройки — тела с дочерними коробками. Один вызов
`VoxelRigidBodyStepCompoundEx` решает всех.

Добавленные в поле helpers (ownership: поле владеет слотами, вызывающий —
содержимым тела):

- сырая операция «добавить слот» намеренно не публикуется: topology code
  сначала вызывает `ReserveBodies` и `PrepareSolver`, затем commit-ит готовый
  план без промежуточного шага с устаревшим scratch/cache;
- `bool SimulationCubeFieldRemoveBody(SimulationCubeField *, uint32_t index)` —
  освобождает тело и сдвигает массив, сохраняя порядок; явно сбрасывает
  warm-start кэш и broadphase. Индексы построек выше сдвигаются вниз.
- `void SimulationCubeFieldInvalidateSolver(SimulationCubeField *)` — сброс
  persistent impulse cache и broadphase после смены топологии/формы.
- `bool SimulationCubeFieldBodyIsBox(const SimulationCubeField *, uint32_t)` —
  истина для слота обычной коробки.
- `bool SimulationCubeFieldPrimitiveCount(const SimulationCubeField *,
  uint32_t *)` — сумма примитивов (`boxCount` составного, иначе 1).
- `bool SimulationCubeFieldReserveBodies(SimulationCubeField *,
  uint32_t required)` — растит транзиентные `bodies`/`shapes` и broadphase под
  ещё не опубликованные тела, не трогая `count`, scratch и contact cache.
- `SimulationCubeFieldPrepareSolver/CommitSolver/AbortSolver` — транзакционная
  замена scratch и contact cache: новые буферы выделяются отдельно, старые
  подменяются только на Commit, поэтому отклонённая правка не теряет
  warm-start.

`SimulationCubeFieldPlacement` теперь возвращает `false` для составного тела:
это привязка именно куба, постройки рисуются `ConstructBlockPlacement`.

`SIMULATION_CUBE_MAX_COUNT` и искусственный предел спавна убраны: спавн
останавливается только на пределе движка или при ошибке памяти.

## ConstructSystem

- `ConstructSystemInit(ConstructSystem *, World *, SimulationCubeField *)` —
  поле заимствуется и живёт дольше системы.
- `ConstructSystemReset` освобождает тела построек через поле (без двойного
  free), `ConstructSystemRelease` дополнительно отвязывает поле.
- `ConstructBody` хранит только мету: `active`, `id`, `bodyIndex`, `blocks`,
  `minimum/maximum`, `localCOM` и фиксированный `shapeBoxes`. Никаких
  независимых `double position/velocity`: поза читается из поля.
- `ConstructSpawn`/`ConstructPlaceBlock`/`ConstructBreakBlock`/`ConstructRaycast`/
  `ConstructActiveBodyCount` сохранены. `ConstructStep` удалён: шаг делает
  `SimulationCubeFieldUpdate`. `ConstructRebase` не двигает тела второй раз —
  общий rebase делает `SimulationCubeFieldRebase`.
- `ConstructBlockPlacement(system, bodyIndex, blockIndex, outOrigin[3],
  outRotation[4])` — привязка блока `[0,1]^3` в мировых координатах с учётом
  поворота тела.

## Масса, COM, инерция, скорость

- `mass = blockCount * CONSTRUCT_BLOCK_MASS`; COM/envelope/полный
  `inverseInertia` считает движок по дочерним коробкам.
- При правке топологии геометрия пересчитывается, world-положения блоков
  сохраняются: COM сдвигается на `R * (localCOM_new - localCOM_old)`, а
  линейная скорость в новой COM берётся как скорость материала в этой точке
  (`v_old + omega x (com_new - com_old)`), угловая сохраняется.
- Распад: осколок получает скорость поверхности прежнего тела в своей COM,
  ориентацию и угловую скорость родителя.
- Правки транзакционные по схеме prepare-all / commit-no-fail. Каждое новое
  тело собирается в отдельном heap-плане `ConstructRigidPlan` (свои blocks,
  boxes, `VoxelRigidBody`); поле, мета ConstructBody, contact cache и
  `nextStableId` не меняются, пока все планы, ёмкости и solver-буферы не
  готовы. Commit только переставляет подготовленное и освобождает старое.
  Любой отказ (слоты, id, координаты, OOM) возвращает `false` и не помечает
  `field.failed`; блоки и прежнее тело остаются на месте.
- Позиция и скорости не проходят через насыщающий double: старая
  `InfiniteCoord` клонируется публичным `CopyAddInt64(..., 0)`, затем
  добавляется ограниченный fixed-point сдвиг COM и `omega x r` (BigInt
  умножение со сдвигом). Уникальные id всех осколков проверяются заранее на
  ноль и переполнение `UINT64_MAX`.
- `HasFaceNeighbor`/`LabelComponents` считают соседей в `int64`, поэтому
  `INT32_MIN/MAX` не переполняют signed и не дают ложную связность.
- `ConstructSpawn` требует связный по граням набор и отвергает материал-0,
  дубликаты и разреженные формы до любой мутации.

## Бюджет контактов

- `scratch` готовится по `VoxelRigidBodyStepCompoundScratchBytes(bodyCount,
  primitiveCount)`; cache с `bodyCapacity >= primitiveCount` рекомендуется.
  Меньшая ёмкость даёт явный false, если фактических контактов больше её
  бюджета. Для сцены
  из одних коробок берётся прежний `VoxelRigidBodyStepScratchBytes` и прежние
  power-of-two пороги, поэтому replay-хеши не меняются.
- Поздний спавн куба, чей `GrowTo` пересоздал legacy scratch, добирается
  `EnsureSolverCapacity` (no-op для чистых кубов).

## Raycast

Луч переводится в систему тела обратным поворотом, пересекается с дочерними
коробками. Нормаль — грань в **локальной сетке** тела (`int8`), как нужно
редактору. Distance — мировая (поворот сохраняет длину). Кубический
`SimulationCubeFieldRaycast` пропускает составные тела (иначе envelope давал
бы попадания в дырке) и проверяет finite origin/direction.

## Проверка

- `build/compound-game-agent`, только `--parallel 1`.
- Сохранены: спавн до 1.5x, persistent power-of-two пороги кэша, raycast и
  радиальный импульс.

## Результат

Собрано и проверено в `build/compound-game-agent` (Windows MSVC, только
`--parallel 1`). Оба клиента (`SimulationOfSins`, `SimulationOfSinsHeadless`)
и `simulation_of_sins_game_test` линкуются.

Hardening-проверка (топология, raycast, падение, сохранение позы/скорости,
атомарность заполненного пула, 16-блочная плита с бюджетом контактов,
отклонённая правка при переполнении id, replay 384 tick + rebase):
Debug и Release — зелёные. Оба конфига проверены отдельно.

`TestConstructFrameReplay` задаёт `executor.structSize = sizeof(executor)`
до `LaiueTaskPoolGetExecutor`, как требует публичный контракт. Он запускает
одну копию через task pool, другую serial и проверяет тождественное состояние
для canonical и colored solver.

Найденный и исправленный ранее баг: `ConstructSystemInit` обнулял систему
через локальный `ConstructSystem empty = {0}` (~2 МиБ) и ронял стек
(`0xC00000FD`). Заменено на `memset`; `ConstructBody`-временные убраны, чтобы
no-CRT кадр оставался меньше 4 КиБ стека.

Ограничения:

- `ConstructSystem.world` хранится для совместимости Init, но коллизию с
  миром целиком ведёт solver через `VoxelCollisionSource` поля.
- `ConstructSystem` immovable, пока привязана к полю: адреса `shapeBoxes`
  уходят в `VoxelRigidCompoundShape`.
- `ConstructSystem` должен быть нулево инициализирован перед первым Init и
  оставаться по одному адресу, пока привязан к полю. Перед повторным Init
  вызывается `ConstructSystemRelease`.
- Проверены Windows MSVC/clang-cl Debug/Release, Linux GCC Debug ASan/UBSan
  и ARM64 link-closure; нативный runtime ARM64/macOS/mobile остаётся отдельной
  платформенной проверкой.
