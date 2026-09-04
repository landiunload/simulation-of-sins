# Simulation of sins

`Simulation of sins` — новая игра на встраиваемом voxel engine
[`laiue`](https://github.com/landiunload/laiue). Проект начат с чистого листа:
старый `laiue-game`, его gameplay, сеть, сервер и ассеты не используются.

Текущий vertical slice содержит:

- отдельное Windows-приложение с собственным жизненным циклом;
- D3D12-кадр и встроенные fallback-шейдеры/текстуры движка;
- небольшую вручную заданную техническую сцену;
- свободную камеру и Raw Input;
- асинхронный chunk streaming;
- бесконечные координаты с chunk-aligned rebasing;
- headless, D3D12 и live-rebase smoke tests плюс unit tests игровой логики.

Это ещё не дизайн игры и не набор игровых механик. Стартовая сцена нужна,
чтобы безопасно развивать продукт поверх стабильной границы движка.

## Платформенная матрица

| Платформа | Игровая логика/headless | Полный клиент |
|---|---:|---:|
| Windows x86_64 | CI | D3D12, окно и ввод, CI |
| Windows ARM64 | код собран локально, native CI job | D3D12 собирается; на устройстве не запускался |
| Linux x86_64 | GCC/Clang, glibc и musl, проверено в Docker | у движка есть Vulkan offscreen и ALSA; игрового окна и ввода ещё нет |
| Linux ARM64 | glibc и musl, проверено в Docker; native CI настроен | Vulkan-профиль движка на ARM64 не собирался |
| Steam Deck / SteamOS | Linux x86_64 core | у движка есть Vulkan offscreen и ALSA; нужны окно, ввод и проверка на устройстве |
| macOS arm64/x86_64 | macOS 11+, native CI настроен, локально не запускался | ещё нет Metal backend |
| Android ARM64 | NDK r29: `.so` собрана и слинкована локально, CI настроен | ещё нет APK/Vulkan/input/audio shell |
| iOS/iPadOS ARM64 | iOS 15+ unsigned app link CI настроен | ещё нет Metal application shell |
| tvOS/visionOS | общий mobile adapter contract | presets/device tests ещё не добавлены |
| Xbox / PlayStation / Nintendo | external static seam | нужны закрытый SDK и hardware |
| WebAssembly/WebGPU | не заявлен | нужен отдельный web port |

Полный интерактивный клиент требует Windows и Visual Studio 2022/MSVC либо
`clang-cl` с Ninja; поддерживаются x86_64 и ARM64 (`windows-msvc-arm64`,
`windows-clang-arm64`). Для любой конфигурации нужны CMake 3.28+ и
исходники `laiue` либо его SDK совместимой OS/architecture/ABI. Headless CI на
Linux и macOS проверяет игровое состояние, fixed-step, координаты и rebasing,
но не доказывает работу окна, GPU, ввода или готовность распространяемого
bundle на этих платформах.

Windows ARM64 проверен локально до уровня компиляции и линковки движка; сама
игра компилируется, а её финальная линковка требует ARM64 CRT из компонента
«MSVC v143 — VS 2022 C++ ARM64 build tools». Запуск на ARM64-устройстве
выполняет отдельный native CI job, поэтому клиент на этой платформе пока
описывается как «собирается», а не «проверен в работе».

Android и iOS profiles собирают игру вместе с исходниками движка через
`SOS_ENGINE_SOURCE_DIR`. Android создаёт и финально линкует нативную `.so`;
iOS создаёт unsigned build-only app bundle, чтобы проверить Mach-O/LTO
closure. Это ещё не устанавливаемые APK/IPA и не device runtime tests.

## Локальная сборка

По умолчанию игра собирает соседний `../laiue` вместе с собой: изменения
движка сразу попадают в приложение, предварительно обновлять SDK не нужно.
При этом игровой код использует публичные CMake targets движка, а не его
внутренние заголовки. Для разработки клонируйте оба репозитория рядом:

```powershell
git clone https://github.com/landiunload/laiue.git
git clone https://github.com/landiunload/simulation-of-sins.git
```

В x64 Developer PowerShell соберите игру и движок одной командой:

```powershell
cd simulation-of-sins
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --no-tests=error
```

Другой каталог исходников задаётся через `-DSOS_ENGINE_SOURCE_DIR=/path/to/laiue`.
Если исходников рядом нет и SDK не указан, `SOS_ENGINE_FETCH=ON` загружает
актуальную `origin/main`. Повторный `cmake --preset <preset>` обновляет
скачанную копию через [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html).
`-DSOS_ENGINE_FETCH=OFF` запрещает загрузку. Для воспроизводимой сборки
выберите `-DSOS_ENGINE_REVISION=locked` (SHA из `engine.lock`) либо явно
укажите commit SHA вместо `locked`. Выбор ревизии относится только к
скачанному движку: соседние исходники и явный SDK он не подменяет.

Локальные правки в `../laiue` подхватываются обычной пересборкой игры,
включая незакоммиченные изменения; обновлять `engine.lock`, публиковать
движок и вручную пересобирать SDK для этого не нужно. Это зависимость
сборки, не горячая замена кода в уже запущенной игре.

Установленный SDK остаётся отдельным поддерживаемым режимом. Соберите в
движке цель `laiue_engine_bundle` и явно передайте игре
`-Dlaiue_DIR=/path/to/bundle/lib/cmake/laiue`. CI проверяет именно этот режим.
`find_package(laiue 0.7.0 EXACT)` отклоняет другую версию, но не устаревший
commit с тем же номером: SDK нужно пересобирать после обновлений движка.
Если в старом build-каталоге сохранился `laiue_DIR`, он продолжит выбирать
SDK; для перехода к соседним исходникам используйте новый build-каталог
(`-B build/local-source`) либо удалите только эту настройку: `-U laiue_DIR`.
`-DSOS_BUILD_ENGINE_TESTS=ON` добавляет тесты движка к тестам игры при
совместной сборке.

Исполняемый файл появится в `build/windows-msvc/bin/Debug/SimulationOfSins.exe`.
Цель `simulation_of_sins_bundle` создаёт самодостаточную папку рядом с build
tree. Runtime DLL движка автоматически копируются к приложению, а сама игра
статически связывает MSVC runtime. Debug игры намеренно совместим с Release
SDK движка: граница — стабильный C ABI, а DLL движка не используют CRT.
Release bundles на Unix-платформах автоматически очищаются от неиспользуемых
символов после копирования; исходные build-артефакты остаются нетронутыми.

Release-сборки по умолчанию ориентированы на скорость: используются
максимальная оптимизация, агрессивное встраивание функций, удаление и
объединение неиспользуемого кода, AVX2/x86-64-v3 и полный LTO. У clang-cl
LLVM LTO и codegen дополнительно работают на уровне 3. Отключить LTO для
диагностического сравнения можно параметром `-DSOS_ENABLE_LTO=OFF`.
Более быстрая пересборка clang-cl с ThinLTO доступна через
`-DSOS_CLANG_LTO_MODE=thin`. Строгая математика остаётся на игровом коде с
координатами и rebasing; быстрый FP уже применяется внутри неавторитетных
графических модулей движка.

Стандартный Release требует AVX2 у MSVC либо полного x86-64-v3 у clang-cl.
Отдельный совместимый artifact можно собрать с `-DSOS_X86_64_LEVEL=sse2` и
соответствующим SSE2 bundle движка.
AVX-512 и настройка `amd_zen4` доступны как opt-in, но не выбраны по умолчанию:
на текущем A/B общий AVX2-профиль оказался быстрее и переносимее.

MSVC `/Ob3` включён в стандартном скоростном профиле игры. Вернуться к `/Ob2`
для сравнения размера и производительности можно через
`-DSOS_AGGRESSIVE_INLINING=OFF`. В проверочном microbenchmark `/Ob3` уменьшил
полное время workload примерно на 1,3%, увеличив приложение на 512 B.

Ручной microbenchmark игровой логики не входит в обычную сборку и CTest:

```powershell
cmake --preset windows-msvc -DSOS_BUILD_BENCHMARKS=ON
cmake --build --preset windows-msvc-release `
    --target simulation_of_sins_game_benchmark --parallel
./build/windows-msvc/bin/Release/simulation_of_sins_game_benchmark.exe
```

Методика, текущая исходная точка и границы интерпретации результатов описаны в
[docs/performance.md](docs/performance.md).

Для статического анализа clang-cl-конфигурацию можно создать так:

```powershell
cmake --preset windows-clang -DSOS_ENABLE_CLANG_TIDY=ON
```

## Headless Linux и macOS

Linux CI собирает движок и игру внутри Debian 13 Docker container:

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-debug --parallel
ctest --preset linux-gcc-debug --no-tests=error
```

Для Clang используется `linux-clang`. Linux ARM64 headless уже прошёл сборку
и тесты в ARM64 Docker; workflow дополнительно назначает его нативному
GitHub-hosted ARM64 runner. macOS имеет отдельные native CI jobs и presets,
которые из текущей Windows-среды не запускались; один slice не подтверждает
другой:

```sh
cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-debug --parallel
ctest --preset macos-clang-arm64-debug --no-tests=error

# На Intel host/runner используйте macos-clang-x86_64.
```

Development CI один раз разрешает ветку `main` в commit SHA и передаёт
его всем платформенным jobs: обновление ветки во время прогона не меняет
ревизию между сборками. Теги и ветки `release/*` собираются с ревизией из
[engine.lock](engine.lock). Обновление lock-файла — отдельный commit после
зелёной матрицы на новой ревизии `laiue`.

## Android и iOS core

Android ARM64 (API 28+, NDK r29):

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
cmake --preset android-arm64-core-closure
cmake --build --preset android-arm64-core-closure --parallel
cmake --preset android-arm64-core
cmake --build --preset android-arm64-core-release --parallel
```

iOS/iPadOS ARM64 (iOS 15+, Xcode 26 на macOS 26):

```sh
cmake --preset ios-arm64-core
cmake --build --preset ios-arm64-core-debug --parallel
cmake --build --preset ios-arm64-core-release --parallel
```

Debug/no-LTO проверяет полную линковку до удаления неиспользуемого кода;
Release отдельно проверяет оптимизированную сборку.

Mobile shell должен получить resource и writable data directories от ОС,
поместить data-only packs в app container и передать явный root каталогу
содержимого. Нативные загружаемые моды отключены. Полный mobile client требует
Vulkan/Metal, lifecycle, touch/controller input, audio, suspend/resume,
thermal/memory handling, store packaging и тестов на устройствах.

## Управление

- `W/A/S/D` — движение;
- `Space` / `Ctrl` — вверх / вниз;
- `Shift` — ускорение;
- мышь — направление взгляда;
- `Escape` — выход.

## Граница game / engine

Игра владеет сценариями, игровым состоянием, материалами, authored world,
сохранениями и пользовательским опытом. В движок переносится только функция,
для которой можно сформулировать независимый от `Simulation of sins` API,
владение, ошибки и тестовый контракт. Правила подробно зафиксированы в
[docs/architecture.md](docs/architecture.md).

## Консоли и другие устройства

Публичный репозиторий не содержит консольный SDK, toolchain, proprietary API
или команды hardware. Здесь остаются portable game rules и контракт для
будущего внешнего platform adapter. Xbox, PlayStation и Nintendo пока не
заявлены как поддерживаемые платформы. Реальная сборка, запуск, packaging и
решение о доступности модов, shader packs и пользовательского содержимого
возможны только в закрытом integration repository зарегистрированного
разработчика на официальном dev/test hardware. Nintendo Switch 2 пока нельзя
планировать как проверяемую цель: публичный портал сейчас не принимает заявки
на development environment.

Steam Deck использует Linux x86_64 core, Android TV/Quest — Android ARM64
core, а iPad — iOS core. Это уменьшает объём platform-specific логики, но не
заменяет renderer, ввод, звук, packaging и реальное тестирование каждого
форм-фактора. Отдельной сборки «под Steam Deck» нет и не требуется: это
обычный Linux x86_64. Движок уже умеет рисовать кадр через Vulkan
(`cmake --preset linux-vulkan-offscreen` в репозитории `laiue`), но пока
только offscreen — до окна, ввода и UI играть на устройстве не на чем.
WebAssembly/WebGPU потребует отдельного sandbox/filesystem/thread adapter и
пока не заявлен.

## Лицензия

MIT, см. [LICENSE](LICENSE). Движок [`laiue`](https://github.com/landiunload/laiue)
распространяется на тех же условиях.
