# Simulation of sins

A life simulation in an infinite 3D world, built on the
[laiue](https://github.com/landiunload/laiue) engine.

Early development. The repository holds the application base: a fixed-step
simulation clock, a headless runner for servers and a client that loads the
engine's window, input and graphics modules at run time and presents frames.
Any of those modules can be missing; the game then runs without that
technology instead of failing to start.

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
SimulationOfSins [--graphics d3d12|vulkan] [--time-scale X] [--smoke]
SimulationOfSinsHeadless [--ticks N] [--time-scale X]
```

## License

[MIT](LICENSE)
