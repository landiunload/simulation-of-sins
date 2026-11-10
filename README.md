# Simulation of sins

A life simulation in an infinite 3D world, built on the
[laiue](https://github.com/landiunload/laiue) engine.

Early development. The world is procedural, volumetric and without edges.
Its first two people, a man and a woman, roam, pick berries from wild bushes,
drink, sleep, have children and pass on what they know. Time runs from a
pause through real time up to a game year per second. The simulation uses
only integer arithmetic, so one seed gives the same history on every machine.

A library of mechanics waits to be wired into the people, one layer at a
time:
- genes with unbounded traits, bodies, life stages, personalities and the seven
  sins;
- kinship, pair bonds, opinions, nested groups, status, territory, culture,
  languages and rumors;
- moods, memories, motives, skills and technology;
- climate, water, game animals, materials, items and disease;
- a chronicle of the world, a census and a spatial index.

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

On Android:

| Touch | Action |
|---|---|
| One finger drag | Rotate |
| Two fingers | Pinch to zoom, drag to move |
| Double tap | Follow the next person |
| Long press | Pause |
| Volume up, down | Faster, slower time |

## Platforms

| Platform | Headless | Client |
|---|---|---|
| Windows x86_64, ARM64 | CI | D3D12 or Vulkan, CI |
| Linux x86_64, ARM64 | CI | Vulkan on X11 when the engine is built with them |
| macOS arm64, x86_64 | CI | not yet |
| Android ARM64 | link check in CI | Vulkan, APK built by CI |
| iOS ARM64 | link check in CI | not yet |

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

Android needs `ANDROID_NDK_HOME`, `ANDROID_HOME` with build tools and a
platform, PowerShell 7 and a JDK. The `android-arm64-app` preset with
`-DSOS_ANDROID_KEYSTORE=<keystore>` builds the target
`simulation_of_sins_android_apk` into `build/android-arm64-app/apk`.

## Running

```sh
SimulationOfSins [--seed N] [--camera-distance M] [--graphics d3d12|vulkan] [--time-scale X] [--smoke]
SimulationOfSinsHeadless [--seed N] [--ticks N] [--time-scale X]
```

## License

[MIT](LICENSE)
