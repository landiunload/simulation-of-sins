# Составные постройки: динамические тела и блоки — рабочие заметки

Задача: убрать игровые потолки `CONSTRUCT_MAX_BLOCKS` (256) и
`CONSTRUCT_MAX_BODIES` (1024). Остаётся ровно один искусственный предел —
`SIMULATION_CONSTRUCT_SPAWN_LIMIT` (1000) у дефолтного сценарного спавнера.
Сама `ConstructSystem` растёт в куче до доступной памяти/представления.

## Зона записи

- `src/game/construct.c`, `src/game/construct.h`
- `tests/game_test.c`
- `docs/compound_dynamic_work.md` (этот файл)

`src/app/application.c`, `tests/construct_spawner_test.c` и движок правит
root. Сборку выполняет root; здесь изменён только игровой код и тест.

## Структура данных

```c
typedef struct ConstructBody
{
    bool active;
    uint64_t id;
    uint32_t bodyIndex;
    uint32_t blockCount;
    int32_t minimum[3];
    int32_t maximum[3];
    double localCOM[3];
    ConstructBlock *blocks;                // отдельное владение
    VoxelRigidCompoundBox *shapeBoxes;     // отдельное владение
} ConstructBody;

typedef struct ConstructSystem
{
    World *world;
    SimulationCubeField *field;
    ConstructBody *bodies;                 // malloc/realloc
    uint32_t capacity;                     // длина массива слотов, изначально 0
} ConstructSystem;
```

- `CONSTRUCT_MAX_BLOCKS`/`CONSTRUCT_MAX_BODIES` удалены полностью.
- `bodies`, `blocks` и `shapeBoxes` — разные выделения. Переезд метаданных
  (`realloc bodies`) не двигает `shapeBoxes`, которые поле держит в
  `VoxelRigidCompoundShape::boxes`. Поэтому внешние циклы читают
  `system->bodies[index]` для `index < system->capacity`.
- Слоты переиспользуются по наименьшему свободному индексу, поэтому порядок
  тел детерминирован и не зависит от истории роста. Метаданные растут ~1.5x
  (`capacity + capacity/2`, старт 64), новые слоты обнуляются.

## Транзакционность и владение

- `ConstructRigidPlan` владеет `blocks`/`boxes`. Commit **передаёт владение**
  телу: присваивает массивы, обнуляет их в плане и освобождает прежние
  массивы тела только после перенаправления `shape->boxes`. `PlanFree` после
  Commit ничего не делает; при отказе освобождает всё.
- Резерв метаданных делается заранее (`ConstructReserveBodies`), затем
  указатель на тело берётся заново по id (`body` может переехать). Массивы
  `blocks` при этом не двигаются, поэтому индексы и метки компонент остаются
  валидными.
- `ConstructSpawn`/`ConstructPlaceBlock`/`ConstructBreakBlock` по-прежнему
  prepare-all / commit-no-fail: `field.failed`, `nextStableId`, числа тел,
  позы и PRNG-состояние спавнера не меняются при любом отказе (память, id,
  координаты). Осколки не «теряются молча»: если не удалось подготовить
  ёмкости/id, правка отменяется целиком.
- `ConstructSystemReset` снимает тела через поле, освобождает массивы тел,
  затем освобождает `blocks`/`shapeBoxes` и обнуляет слоты. `world`/`field`
  сохраняются: после Reset ту же систему можно снова привязать к полю.
  Повторный Reset и Reset после `SimulationCubeFieldRelease` — no-op.
  `ConstructSystemRelease` дополнительно отвязывает поле.

## Хеш-карта воксельных координат

Квадратичные проходы по блокам заменены на открытую хеш-таблицу
координата → индекс блока (`ConstructVoxelMap`, линейное пробирование,
load factor <= 0.5):

- `ConstructSpawn`: одна карта на дубликаты, материал и связность по 6
  граням.
- `ConstructPlaceBlock`: карта занятых клеток для проверки «клетка свободна
  и касается гранью».
- `ConstructBreakBlock`: карта всех блоков, кроме удаляемого; BFS по 6
  соседям.

Метки компонент — `uint32_t`, очередь — `uint32_t`, память — куча
(`malloc`), а не фиксированный стек и не `uint8`/`uint16` индексы. Порядок
обхода сохранён: сид — наименьший непомеченный индекс, соседи — в
фиксированном порядке шести граней. Поэтому нумерация компонент, порядок
блоков в осколках и их id детерминированы и совпадают с прежним поведением.

Все размеры и произведения (`count * sizeof`, `active + extra`,
`nextStableId + extra`, ёмкость таблицы) проверяются на переполнение
`size_t`/`uint32`/`uint64`.

## Что осталось без изменений

- Точное слияние коробок `VoxelRigidCompoundMergeBoxes` и fallback на
  единичные дети для слишком длинных составных форм.
- Масса `blockCount`, COM/envelope/тензор от движка, перенос `InfiniteCoord`
  и `omega x r` при правках и распаде.
- Публичные сигнатуры `ConstructSpawn`/`Place`/`Break`/`Raycast`/
  `BlockPlacement`/`Rebase`/`ActiveBodyCount`.

## Проверка (tests/game_test.c)

- Все обходы `CONSTRUCT_MAX_BODIES` заменены на `system->capacity`;
  `FindConstructSlot` возвращает `system->capacity` как «не найдено».
- Прежняя проверка исчерпания фиксированного пула заменена:
  - `TestConstructGrowthBeyondOneThousand` — 1100 тел, `capacity > 1024`,
    наименьший освободившийся слот переиспользуется следующим спавном;
  - `TestConstructLargeShapeOwnership` — тело из 4096 блоков, слияние в одну
    коробку, правка блока передаёт владение новым массивам, поле видит
    актуальный `shapeBoxes`;
  - `TestConstructLargeSplitOwnership` — 4096 блоков, распад по одному
    соединительному блоку на 2048 + 2047, у каждого тела свои `blocks` и
    `shapeBoxes`, поле ссылается на форму своего тела.
- Сцена не прогоняется физикой: тесты больших форм проверяют владение,
  рост и распад, а не шаг решателя.
- Никаких заявлений о симуляции нехватки памяти (OOM) нет.

## Что нужно от root

1. `src/app/application.c` уже читает `constructs->capacity` и
   `constructs->bodies` — менять циклы не нужно.
2. `tests/construct_spawner_test.c` уже использует `system->capacity`.
   Проверить, что больше нигде нет `CONSTRUCT_MAX_BODIES`.
3. `src/game/construct_spawner.c/h` — без изменений: лимит 1000 остаётся
   свойством спавнера, а не системы.
4. Движок: убрать `256`-child cap (если он ещё есть) и убедиться, что
   `uint32`-число детей и бюджет scratch — ресурс шага, а не игровой потолок.
   Число тел ограничено только `VOXEL_RIGID_MAX_BODIES = UINT32_MAX / 2`.
5. CMake не менялся: новые файлы не заводились.

## Ограничения

- Потолок по представлению остаётся: он определяется `size_t`/`uint32` и
  доступной памятью; для каждого шага — бюджет scratch движка.
- Сборка и прогон тестов здесь не выполнялись: `src/game/construct.c` и
  `tests/game_test.c` прошли только `clang -fsyntax-only`; полную матрицу
  собирает root.
