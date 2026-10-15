# Simulation of sins

A life simulation in an infinite 3D world, built on the
[laiue](https://github.com/landiunload/laiue) engine.

Early development. The world is procedural, volumetric and without edges.
Its first two people, a man and a woman, roam, pick berries from wild bushes,
drink, sleep, have children and pass on what they know. Time runs from a
pause through real time up to a game year per second. The simulation uses
only integer arithmetic, so one seed gives the same history on every machine.

The engine's window, input, graphics, camera math and meshing are loaded at
run time. Any of them can be missing; the game then runs without that
technology instead of failing to start.

## Controls

| Input | Action |
|---|---|
| Mouse wheel | Zoom from 2 m to 80 km |
| Right mouse button | Rotate |
| W A S D, Shift | Move, faster |
| E | Follow the next person |
| Space | Pause |
| 1–6 | Real time, a minute, an hour, a day, a month or a year per second |
| Esc | Quit |

## Platforms

| Platform | Headless | Client |
|---|---|---|
| Windows x86_64, ARM64 | CI | D3D12 or Vulkan, CI |
| Linux x86_64, ARM64 | CI | Vulkan on X11 when the engine is built with them |
| macOS arm64, x86_64 | CI | not yet |
| Android ARM64, iOS ARM64 | link check in CI | not yet |

## Building

Clone the engine next to this repository; the game builds it from source:

```sh
git clone https://github.com/landiunload/laiue.git
git clone https://github.com/landiunload/simulation-of-sins.git
cd simulation-of-sins
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release
```

Linux and macOS use the `linux-gcc`, `linux-clang` and `macos-clang-arm64`
presets. A prebuilt engine SDK is selected with `-Dlaiue_DIR=<sdk>/lib/cmake/laiue`.
CMake 4.2 or newer is required.

## Running

```sh
SimulationOfSins [--seed N] [--camera-distance M] [--graphics d3d12|vulkan] [--time-scale X] [--smoke]
SimulationOfSinsHeadless [--seed N] [--ticks N] [--time-scale X]
```

## License

[MIT](LICENSE)
