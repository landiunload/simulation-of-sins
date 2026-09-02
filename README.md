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
| Linux x86_64 | GCC/Clang, Docker CI | ещё нет графического backend |
| Linux ARM64 | проверено в Docker; native CI настроен | ещё нет графического backend |
| Steam Deck / SteamOS | Linux x86_64 core | нужен Vulkan/input/audio client |
| macOS arm64/x86_64 | macOS 11+, native CI настроен, локально не запускался | ещё нет Metal backend |
| Android ARM64 | NDK r29 `.so` link CI настроен | ещё нет APK/Vulkan/input/audio shell |
| iOS/iPadOS ARM64 | iOS 15+ unsigned app link CI настроен | ещё нет Metal application shell |
| tvOS/visionOS | общий mobile adapter contract | presets/device tests ещё не добавлены |
| Xbox / PlayStation / Nintendo | external static seam | нужны закрытый SDK и hardware |
| WebAssembly/WebGPU | не заявлен | нужен отдельный web port |

Полный интерактивный клиент пока требует Windows x86_64, Visual Studio
2022/MSVC либо `clang-cl` и Ninja. Для любой конфигурации нужны CMake 3.28+
и установленный SDK `laiue` совместимой OS/architecture/ABI. Headless CI на
Linux и macOS проверяет игровое состояние, fixed-step, координаты и rebasing,
но не доказывает работу окна, GPU, ввода или готовность распространяемого
bundle на этих платформах.

Android и iOS profiles собирают игру вместе с исходниками движка через
`SOS_ENGINE_SOURCE_DIR`. Android создаёт и финально линкует нативную `.so`;
iOS создаёт unsigned build-only app bundle, чтобы проверить Mach-O/LTO
closure. Это ещё не устанавливаемые APK/IPA и не device runtime tests.

Игра намеренно подключается к установленному SDK через `find_package`, а не
к `laiue/src`. Сначала подготовьте SDK движка:

```powershell
cd C:\Users\landi\projects\laiue
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target laiue_engine_bundle --parallel
```

Затем соберите игру:

```powershell
cd C:\Users\landi\projects\simulation-of-sins
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --no-tests=error
```

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

Development CI временно получает `laiue` из ветки `main`, чтобы сразу видеть
cross-repository несовместимости. Это не атомарная зависимость: перед первым
релизом проект обязан хранить проверенный immutable commit SHA в обновляемом
lock-файле и собирать релиз только с ним.

## Android и iOS core

Android ARM64 (API 28+, NDK r29):

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
cmake --preset android-arm64-core
cmake --build --preset android-arm64-core-release --parallel
```

iOS/iPadOS ARM64 (iOS 15+, Xcode 26 на macOS 26):

```sh
cmake --preset ios-arm64-core
cmake --build --preset ios-arm64-core-release --parallel
```

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
форм-фактора. WebAssembly/WebGPU потребует отдельного sandbox/filesystem/thread
adapter и пока не заявлен.
