# Сложные постройки: рабочие заметки по интеграционному тесту

Задача: доказать, что произвольные случайные связные постройки до 4096 блоков
работают через публичный `ConstructSystem` — без лимита 256 детей, без замены
формы выпуклой оболочкой и без выключения физики. Root реализует код и CMake и
собирает; здесь только тест и handoff.

## Зона записи

- `tests/construct_complex_test.c` (новый)
- `docs/construct_complex_test_work.md` (этот файл)

Другие файлы не трогались. CMake-интеграцию и сборку делает root; тест
рассчитан на линковку с `simulation_of_sins_core`.

## Публичный контракт, на который опирается тест

Тест написан под уже согласованную динамическую форму системы (её получает
root от параллельного агента):

```c
typedef struct ConstructBody
{
    bool active;
    uint64_t id;
    uint32_t bodyIndex;   // слот в SimulationCubeField
    uint32_t blockCount;
    int32_t minimum[3];
    int32_t maximum[3];
    double localCOM[3];
    ConstructBlock *blocks;              // указатель, без CONSTRUCT_MAX_BLOCKS
    VoxelRigidCompoundBox *shapeBoxes;   // столько же, сколько готовых коробок
} ConstructBody;

typedef struct ConstructSystem
{
    World *world;
    SimulationCubeField *field;
    ConstructBody *bodies;   // динамический массив
    uint32_t capacity;       // число слотов тел
} ConstructSystem;
```

Опираемся только на публичное:

- `ConstructSystemInit/Reset/Release`, `ConstructSpawn`, `ConstructPlaceBlock`,
  `ConstructBreakBlock`, `ConstructRaycast`, `ConstructBlockPlacement`,
  `ConstructActiveBodyCount`;
- `SimulationCubeFieldInit/Release/AdvanceTick/Rebase`,
  `SimulationCubeFieldReserveBodies` вызывается самим `ConstructSpawn`;
- `VoxelRigidBody`, `VoxelRigidCompoundShape/Box`, `InfiniteCoord`.

`CONSTRUCT_MAX_*`, `CONSTRUCT_MAX_BLOCKS/BODIES` нигде не упоминаются. Ёмкость
тел берётся из `system->capacity`, блоки — из `body->blocks`, форма — из
`field->shapes[bodyIndex].boxes` (тот же адрес, что `body->shapeBoxes`).

## Что проверяет executable

`main(argc, argv)`, опционально `--stress`, `--ticks=N`, `--bodies=N`,
`--blocks=N`. Неизвестный аргумент — код 2; код 0/1.

Default (короткий, для `ctest`, 64 тика):

1. `TestGeneratedGeometry`
   - случайный рост на конечной решётке `32^3` (seed-детерминированный),
     ровно 257/1024/4096 блоков; форма связна по граням;
   - комб-формы для 1024 и 4096 блоков: связны, но **не** склеиваются в одну
     коробку, поэтому `shape->boxCount > 256`. Это и есть проверка «больше 256
     детей», а не 4096 сплошных кубов, схлопнутых в один;
   - для каждой постройки: `blockCount` редактируемых блоков, множество клеток
     совпадает с входом и уникально, `minimum/maximum`, масса
     (`1/inverseMass == blockCount`) и COM как среднее центров клеток;
   - точная укладка готовых дочерних коробок: границы выровнены по целой
     сетке после возврата `localCOM`, каждая клетка накрыта ровно одной
     коробкой, суммарный объём равен числу блоков, коробки лежат внутри AABB;
   - `ConstructBlockPlacement` для выборки блоков даёт конечные позы;
   - правка на масштабе: `ConstructPlaceBlock` + `ConstructBreakBlock` на теле
     из 257 блоков возвращают исходный `blockCount` и не дают распада.
2. `TestCavityRaycasts`
   - кольцо со сквозным отверстием: луч сквозь полость **не** находит блок,
     луч в стенку находит ровно верхнюю грань;
   - полый куб: изнутри полости луч упирается во внутренние стенки (обе оси),
     снаружи самая близкая стенка останавливает луч.
3. `TestComplexCollisionAndReplay` (по умолчанию 64 тика)
   - два независимых мира и две системы с одинаковым seed;
   - две одинаковые сложные постройки по 257 блоков: первая стоит на полу,
     вторая стартует с зазором `0.2` — без плотного начального пересечения;
     рядом лежит комб-форма на 800 блоков (>256 детей), которая тоже реально
     симулируется, прогоняя child-BVH шага;
   - настоящая физика без выключения тел: к моменту касания зазор AABB падает
     ниже `0.1`, контакты наблюдаются, вершины не проваливаются сквозь пол;
   - каждый тик сравниваются канонические состояния: `tickCount`, `count`,
     `nextStableId`, `randomState`, счётчики контактов, `stableId/active/
     sleeping/sleepCounter`, `orientation`, `halfExtent`, `inverseInertia`,
     масса/трение/упругость, а позиции и скорости — по полным лимбам
     `InfiniteCoord` (не по указателям и не через `memcmp` структуры);
   - `ConstructBody` и `shapeBoxes` сравниваются поимённо, без хвостового
     padding;
   - огромный целочисленный rebase `+-2^30` блоков и возврат: позиция в
     масштабе `2^32` даёт значение больше `2^53`, которое double побитово уже
     не хранит; после возврата состояние и хеш совпадают с нетронутым миром.

`--stress` (вне дефолтного CI; default 256 тиков, 8 тел по 512 блоков,
настраивается через `--ticks/--bodies/--blocks`):

- 8+ seeded сложных тел (каждое третье — комб, остальные — случайные), полная
  физика до пола и столкновений, без deactivation и фейкового сна;
- печатает wall time (генерация/спавн/физика), пиковое число контактов,
  минимум z вершины и хеш состояния;
- wall time — только информация; критерием прохождения не является. Тест не
  утверждает, что произвольная сцена идёт без лагов.

## Почему это не флак

- форма и порядок тел фиксированы seed-детерминированным PRNG (splitmix64),
  без libc `rand`;
- проверки физики идут по каноническому состоянию двух прогонов, а не по
  времени;
- никаких wall-time порогов в default-части нет.

## Что нужно от root

1. CMake-цель `simulation_of_sins_construct_complex_test` из
   `construct_complex_test.c`, линкуется с `simulation_of_sins_core`, на
   non-Windows — `m`, `sos_configure_c_target`, `sos_deploy_runtime(... laiue::
   numeric laiue::world laiue::physics)`.
2. CTest: `simulation_of_sins.construct_complex` — default. Дать щедрый
   `TIMEOUT` (≥ 180 c): 4096-блочная готовка и O(блок×коробка) проверка укладки
   заметно дороже остальных game-тестов. `--stress` вынести отдельным custom
   target, в CI не включать.
3. Убедиться, что параллельный агент оставил поля `bodies/capacity` у
   `ConstructSystem` и `blocks/shapeBoxes`-указатели у `ConstructBody`; при
   другом написании тест правится в одном месте.

## Точный запуск

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug -R simulation_of_sins.construct_complex --output-on-failure
```

Явное число тиков (не меньше 64 из-за сценария столкновения; корень может
передать больше):

```powershell
build\windows-msvc\bin\Debug\simulation_of_sins_construct_complex_test.exe --ticks=64
```

Тяжёлый сценарий отдельно (в CI не включать):

```powershell
build\windows-msvc\bin\Debug\simulation_of_sins_construct_complex_test.exe --stress
build\windows-msvc\bin\Debug\simulation_of_sins_construct_complex_test.exe --stress --bodies=8 --blocks=4096 --ticks=512
```

## Известные места для сверки при первом запуске

- Тест ждёт динамические `system->bodies`/`system->capacity`; на старой
  фиксированной системе он не скомпилируется — это ожидаемо.
- Если готовка по-прежнему схлопывает комб в одну коробку, упадёт
  `EXPECT(shape->boxCount > 256)`.
- Если `shapeBoxes` выделен ровно под `boxCount`, сравнение в
  `ExpectConstructBodiesEqual` читает только `boxCount` элементов — правок не
  требуется.
- При `--ticks` меньше ~48 тела могут не успеть сомкнуться, и упадёт
  `EXPECT(closest < 0.1)`. Дефолт 64 рассчитан на касание (g = -24, зазор
  0.2 закрывается примерно за 17 тиков).
