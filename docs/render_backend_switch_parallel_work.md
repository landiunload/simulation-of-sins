# Переключение бэкенда рендера (D3D12/Vulkan) в игре — параллельная работа

Отчёт о правке. Зона — только игра (`src/app/`, `src/main_windows.c`,
`tests/CMakeLists.txt`, этот документ). `src/game/*` не тронут: мир, тела,
камера, ввод и пул физики от рендера не зависят.

Движок уже предоставляет диспетчер бэкендов
(`../laiue/src/render/renderer.h`): `RendererBackendIsAvailable`,
`RendererCreateWithBackend`, `RendererGetBackend`. Игра этим раньше не
пользовалась вовсе.

## 0. Как воспроизвести

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --parallel 3
ctest --preset windows-msvc-release --no-tests=error
```

Зеркало CI с clang-tidy (`WarningsAsErrors: '*'`):

```bat
cmake --preset windows-clang -B build/ci-mirror -DSOS_ENGINE_SOURCE_DIR= ^
  -Dlaiue_DIR=C:/Users/landi/projects/laiue/build/windows-clang/bundles/engine/Release/lib/cmake/laiue ^
  -DSOS_ENABLE_CLANG_TIDY=ON
cmake --build build/ci-mirror --config Release --parallel 3
ctest --test-dir build/ci-mirror --build-config Release --no-tests=error
```

## 1. Выбор бэкенда при запуске

Переменная окружения `SOS_RENDER_BACKEND`:

| Значение | Смысл |
| --- | --- |
| `auto` | не задана или `auto` — бэкенд по умолчанию для этой сборки (сегодня Windows → D3D12) |
| `d3d12` | явно D3D12 |
| `vulkan` | явно Vulkan |

Разбор сделан по образцу `SOS_PHYSICS_THREADS` / `SOS_NO_VSYNC`
(`RenderBackendFromEnvironment`, `src/app/application.c`). Поведение при
отказе:

1. если запрошенный бэкенд **недоступен** (`RendererBackendIsAvailable ==
   false`) — один раз пишем в лог и переходим на `RENDERER_BACKEND_AUTO`;
2. если `RendererCreateWithBackend` с запрошенным бэкендом вернул `NULL` —
   один раз повторяем с `RENDERER_BACKEND_AUTO`.

В обоих случаях в stderr пишется, какой бэкенд поднят в итоге
(`RendererGetBackend`), а в кадровый CSV добавлена последняя колонка
`render_backend` (`d3d12` / `vulkan` / `none`). Парсить всегда проще с
конца строки, поэтому колонка именно последняя.

На этой машине `VULKAN_SDK` не выставлен, поэтому сборка только D3D12:
`SOS_RENDER_BACKEND=vulkan` печатает
`requested backend is not available in this build; using auto` и поднимает
D3D12; `SOS_RENDER_BACKEND=d3d12` работает как обычно.

## 2. Горячая смена по F7

`INPUT_KEY_F7` в игре не занята. Обработка — в начале кадра, до
`ChunkStreamingPump`/`RendererBeginFrame` (сразу после разбора
`WindowConsumeResize`, `src/app/application.c`):

1. `RendererGetBackend` текущего рендера, целевой — противоположный
   (D3D12 → Vulkan, Vulkan → D3D12);
2. если целевой `RendererBackendIsAvailable == false` — только лог,
   сессия не трогается;
3. иначе `RenderSessionDestroy` → `RenderSessionCreate(target)`;
4. если создать целевой не удалось — `RenderSessionCreate(previous)`;
5. если не удалось и это — приложение закрывается с кодом возврата `11`
   (окно без рендера бессмысленно), без падения.

Мир, тела, камера, ввод и пул физики между шагами не трогаются. Куб чанков
после смены пуст и заполняется заново стримингом — это ожидаемо и
измерено (раздел 4).

`RendererResize` вызывается только для актуального `application->renderer`,
а смена происходит после разбора resize: новый рендерер создаётся сразу в
текущем размере клиента, отложенный движковый resize получает уже новый
рендерер.

## 3. Устройство: рендер-сессия

В `src/app/application.c` появились две функции (файлы не создавались):

- `RenderSessionCreate(application, requested)` — строго в порядке
  `RendererCreateWithBackend` → `RendererSetMaterialNames` →
  `RendererPrepareWorldFrom` → `RendererSetVerticalSync` →
  `CreateCubeMesh` → `ChunkStreamingCreate(world, renderer, 2)` →
  `ChunkStreamingSetCenter` по текущей позиции камеры.
  При ошибке снимает всё созданное (`RenderSessionDestroy`) и
  возвращает `false`; прежний код возврата старта сохраняется в
  `sessionErrorCode` (5 — рендерер/материалы/мир, 7 — стриминг).
  Меш куба, не создавшийся из-за нехватки памяти, отказом не считается:
  `OnFrame` повторяет попытку, как раньше.
- `RenderSessionDestroy(application)` — `ChunkStreamingDestroy` →
  `RendererDestroyMesh(cubeMesh)` → `RendererDestroy`, терпимо к
  частично созданной сессии.

Стартовый путь `SimulationApplicationRun` и `DestroyApplication` переведены
на них. Коды возврата старта 2..7 сохранены (мир/тела/физика создаются до
сессии, ошибочные ветки отдают те же 6 и т.д.).

**Отклонение по порядку стадий.** `RenderSessionCreate` включает
`ChunkStreamingCreate`, которому нужен готовый `World`, и
`ChunkStreamingSetCenter`, которому нужна инициализированная камера. Поэтому
стадии запуска мира/тел/камеры теперь идут **до**
`renderer_create`/`renderer_material_names`/`renderer_prepare_world`/
`cube_mesh_create`/`streaming_create`, а не вперемешку с ними. **Все имена
стадий сохранены** (тот же набор из 14 строк, ни одна не потеряна и не
добавлена), и `tests/cmake/startup_profile_test.cmake` проходит:
`first_filled` по-прежнему согласован (все заявки построены, меши
загружены). Полностью сохранить исходный порядок нельзя, не вынося
`ChunkStreamingCreate` из `RenderSessionCreate`; это противоречило бы
заданию. Горячая смена стадии запуска не пишет (гейт `runLoopStarted`),
поэтому startup-профиль не засоряется.

## 4. Что измерено

Стенд — режим `--backend-switch-smoke` на этой машине (Vulkan недоступен,
поэтому проверяется пересоздание D3D12 → D3D12 и корректный отказ Vulkan),
5 прогонов, Release MSVC:

| Прогон | `RenderSessionDestroy`, мс | `RenderSessionCreate`, мс | Кадров до перезаливки куба | Мешей |
| --- | --- | --- | --- | --- |
| 1 | 47.121 | 198.710 | 2 | 25 |
| 2 | 63.491 | 208.715 | 2 | 25 |
| 3 | 53.087 | 204.354 | 2 | 25 |
| 4 | 57.199 | 225.519 | 2 | 25 |
| 5 | 46.045 | 207.870 | 2 | 25 |

Суммарно смена (destroy + create) — примерно **0.25–0.29 с**; большую часть
занимает `RenderSessionCreate` (создание device/swapchain, подготовка
текстурпака). Куб чанков перезаливается за **2 кадра** (25 мешей) — при 2
чанках радиуса обзора это весь центр обзора. Числа получены из stderr-строк
`[render-backend] switch timing: ...` и
`[render-backend] chunk cube refilled after N frames (M meshes)`.

## 5. Режим проверки и тест

Новый режим `SIMULATION_RUN_BACKEND_SWITCH_SMOKE`, флаг
`--backend-switch-smoke` (`src/main_windows.c`). Схема: 3 кадра прогрева →
смена на второй доступный бэкенд (на этой машине второго нет, поэтому
пересоздание **того же** D3D12 — это честная проверка механики
пересоздания) → ожидание перезаливки куба (`uploadedMeshes > 0`) → попытка
переключиться на **недоступный** бэкенд → ещё 3 кадра. Закрытие —
самостоятельное, `maximumPresentedFrames` = 300 (страховочный предел).

Возврат 0 только если:

- все кадры после смены показаны (`RendererEndFrame == true`; иначе
  `OnFrame` выставляет код 9);
- после смены `ChunkStreamingGetStats().uploadedMeshes > 0` (куб
  перезалит);
- отказ недоступного бэкенда ничего не сломал (`RendererGetBackend` до и
  после отказа совпадает, кадры продолжаются);
- фаза проверки дошла до конца (иначе код 12).

Иначе — 11 (сменить/восстановить сессию не удалось) или 12 (проверка не
выполнена). Зарегистрированы `add_test(simulation_of_sins.backend_switch)` и
`add_custom_target(simulation_of_sins_backend_switch_smoke)` рядом с
`rebase_render`, с `RESOURCE_LOCK d3d12`.

## 6. Результаты прогонов

| Конфигурация | Сборка | clang-tidy | ctest |
| --- | --- | --- | --- |
| windows-msvc Release | ок | — | 10/10 passed |
| windows-clang Release (ci-mirror, SDK движка, clang-tidy ON) | ок | чисто | 10/10 passed |

Отдельно проверено руками: `SOS_RENDER_BACKEND=d3d12` (старт на D3D12),
`SOS_RENDER_BACKEND=vulkan` (лог об отсутствии + AUTO → D3D12),
`--backend-switch-smoke` (код 0), кадровый CSV с колонкой `render_backend`,
startup CSV с полным набором стадий и валидным `first_filled`.

## 7. Чего не удалось проверить

На этой машине нет Vulkan SDK (`Vulkan_*—NOTFOUND`, `renderer_vulkan.c` не
слинкован), поэтому **не проверены**: реальная смена D3D12 → Vulkan,
создание Vulkan-рендера, кадры на Vulkan. Когда движок соберётся с Vulkan,
тот же код должен заработать без правок: `RendererBackendIsAvailable`
вернёт `true`, и `--backend-switch-smoke` выберет смену на Vulkan, а
недоступным для отказа станет уже D3D12. Напомню ограничение движка из его
отчёта: пока `LaiueShader.cmake` не компилирует оба набора шейдеров, кадр
на Vulkan может не нарисоваться даже при успешном создании рендера.

## 8. Файлы

```
 src/app/application.c | ~500 строк: RenderSessionCreate/Destroy, выбор
                         бэкенда, F7, smoke-режим, колонка render_backend
 src/app/application.h | +1 строка: SIMULATION_RUN_BACKEND_SWITCH_SMOKE
 src/main_windows.c    | +4 строки: флаг --backend-switch-smoke
 tests/CMakeLists.txt  | тест simulation_of_sins.backend_switch и
                         custom target simulation_of_sins_backend_switch_smoke
 docs/render_backend_switch_parallel_work.md | этот отчёт
```
