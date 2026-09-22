# Construct spawner: рабочие заметки по тестам

Задача: независимые regression/soak тесты для нового дефолтного спавнера
1000 «г»-построек. Root реализует код и CMake и собирает; здесь только тест и
handoff.

## Зона записи

- `tests/construct_spawner_test.c` (новый)
- `docs/construct_spawner_test_work.md` (этот файл)

Другие файлы не трогались. CMake-интеграцию и сборку делает root; целевая
сборка уже заведена в `tests/CMakeLists.txt`.

## Публичный контракт, на который опирается тест

Тест ожидает ровно это (имена и типы из задания):

```c
#define SIMULATION_CONSTRUCT_SPAWN_LIMIT 1000u
#define SIMULATION_CONSTRUCT_SPAWN_BLOCKS 9u
bool SimulationConstructSpawnerAttach(ConstructSystem *system);
```

и добавленные поля `SimulationCubeField` (сверено с фактическим заголовком):

- `bool (*spawnBody)(void *context, const double position[3]);`
- `void *spawnContext;` — после Attach равен `system`;
- `uint64_t spawnLimit;` — после Attach равен `SIMULATION_CONSTRUCT_SPAWN_LIMIT`,
  `0` означает «без лимита»;
- `bool spawningStopped;` — наблюдается, но тест на него не опирается.

Проверяется поведение, а не коды:

- `Attach` отклоняет `NULL`, отсутствие `world`/`field`, ненулевые
  `field->count`/`field->spawnCounter`; на валидной системе ставит колбэк,
  `spawnContext` и `spawnLimit`.
- `AdvanceTick` сохраняет 100 спавнов/с на 128 тиках: ровно 1000 тел на
  1280-м тике, к этому моменту `spawnPhase == 0`.
- Счётчик `spawnCounter` увеличивает сам `AdvanceTick` после успешного
  колбэка (как и написано в шапке `falling_cubes.h`), а колбэк только читает
  `spawnCounter` как ordinal. Прямой вызов колбэка счётчик не двигает — тест
  это учитывает. После лимита спавнов нет, но `tickCount` растёт, а
  `count`/`spawnCounter`/`randomState` заморожены.
- Позиция/слой сетки читают `field->spawnCounter` до инкремента (тест выставляет
  счётчик в 100 и ждёт второй слой, `+6` блоков).
- Без `Attach` (`spawnBody == NULL`, `spawnLimit == 0`) поле остаётся
  безлимитным стресс-спавнером кубов и перешагивает 1000.
- Форма: 9 блоков, стойка `(0,0,0..4)` и перекладина `(1..4,0,4)`, материал не
  проверяется.
- Сетка: 10x10 с шагом 8; центрируется центр пятиширинной фигуры, поэтому блок
  `(0,0,0)` смещён на `-2.5` по X и `-0.5` по Y. Тест добавляет повёрнутые
  полуразмеры и требует 10 различных координат на ось, `max-min == 72` и
  нулевое среднее. Каждый следующий слой из 100 начинается на 6 блоков выше.
  Тела в дешёвом прогоне успевают сделать один шаг физики до выключения,
  поэтому геометрия проверяется с допуском 0.05, а не побитово.
- Начальная закрутка без линейного броска: `|w_i| <= 0.35`, `|w| <= 0.35*sqrt(3)`.
- `CONSTRUCT_MAX_BODIES` поднят: тест требует `>= 1024` и `> 128`.
- Слот `ConstructSystem[i]` совпадает со спавном `i` (слоты не освобождаются),
  поэтому по слоту читается позиция сетки.

## Что делает executable

`main(argc, argv)`, опционально `--soak`. Другие аргументы — код возврата 2.
Код возврата 0/1.

Default (дешёвый, для `ctest`):

1. `TestAttachContract` — валидация и поля после Attach.
2. `TestSpawnerShapeGridAndInterior` — прямой вызов колбэка (без шага физики):
   форма, отсутствие линейного броска, лимит закрутки, луч сквозь пустую
   внутренность и в стойку (`ConstructRaycast`), затем счётчик 100 → слой +6.
3. `TestSpawnerLimitBookkeeping` — 1280 тиков `AdvanceTick`. Новые тела сразу
   помечаются `active = false`, поэтому шаг физики остаётся дешёвым; это
   проверка логики лимита, а не физики. Далее проверяются форма и сетка всех
   1000 тел, заморозка `count`/`spawnCounter`/`randomState` на 64 лишних тиках
   и рост `tickCount`.
4. `TestLegacyUnattachedUnlimited` — поле без Attach, 1001 куб.
5. `TestSpawnerReplay` — два независимых поля/системы с одинаковым seed по
   128 тиков; сравниваются тела (bigint exact), формы, `ConstructBody`,
   побайтовый `FieldHash` (bigint сериализуется по лимбам, не по указателям).

`--soak` дополнительно запускает `RunSoak` (сначала default-блок, потом soak):

- 2 полных прогона по 1536 тиков + 64 тика после лимита на настоящей физике,
  **без** выключения тел, телепорта и фейкового сна;
- сравниваются оба прогона по состояниям и хешу;
- печатает: сколько тел реально симулируется и сколько блоков, `count`/`active`/
  `awake`/`contacts`, минимум z вершины дочернего блока (по всем повёрнутым
  вершинам, не по COM), максимальные линейную/угловую скорости, максимальную
  ошибку нормы кватерниона, replay-хеши и число ошибок;
- консервативные границы: `min child vertex z > 0`, `orientation error < 1e-3`,
  скорости конечны, `failed == false`.

## Что нужно от root

1. CMake уже заведён (сверено с `tests/CMakeLists.txt`):
   - `simulation_of_sins_construct_spawner_test` из `construct_spawner_test.c`,
     линкуется с `simulation_of_sins_core`, на non-Windows — `m`,
     `sos_configure_c_target`, `sos_deploy_runtime(laiue::numeric laiue::world
     laiue::physics)`;
   - `laiue::task` не нужен: тест идёт без executor (serial).
2. CTest:
   - `simulation_of_sins.construct_spawner` — default, `TIMEOUT 60`;
   - `--soak` вынесен в отдельный custom target
     `simulation_of_sins_construct_spawner_soak`, чтобы дефолтный CI оставался
     дешёвым. Если гонять soak в CI, дать длинный timeout: это 2 прогона x 1600
     тиков с 1000 составными телами.
3. Заголовок `src/game/construct_spawner.h` и поля поля названы так, как в
   контракте выше (`spawnBody`/`spawnContext`/`spawnLimit`/`spawningStopped`);
   `CONSTRUCT_MAX_BODIES == 1024`.
4. Позиция спавна берётся как в приложении: `(0.5, 0.5, 1.0 + 18.0)`, где
   18.0 — `SIMULATION_CUBE_SPAWN_HEIGHT` из `application.c`, а `1.0` — верх
   слоя пола. Тест дублирует это число константой, публичного заголовка не
   заводит.

## Известные места для сверки при первом запуске

- Если `Attach`/`AdvanceTick` не вызовут колбэк при `spawnBody != NULL`,
  упадут `TestSpawnerShapeGridAndInterior` и `TestSpawnerLimitBookkeeping`.
- Если лимит считается не по `spawnCounter`, упадёт проверка заморозки.
- Если центровка центров фигур не «10 значений с шагом 8 и нулевым средним»,
  упадёт `ValidateShapeAndGrid` — это осознанно, по контракту.
- `--soak` — настоящая физика с плотной укладкой; если решатель отдаёт
  `failed` на контактах, тест вернёт 1 и напечатает тик.
