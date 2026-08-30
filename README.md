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

## Зависимости

- Windows x86_64;
- CMake 3.28+;
- Visual Studio 2022/MSVC либо `clang-cl` и Ninja;
- соседний checkout `C:\Users\landi\projects\laiue` версии 0.7+.

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

Для статического анализа clang-cl-конфигурацию можно создать так:

```powershell
cmake --preset windows-clang -DSOS_ENABLE_CLANG_TIDY=ON
```

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
