# Building Pizza Legacy

**Pizza Tycoon HD:** the scripts in `scripts/` do all of this for you with
pinned dependency versions (`scripts/deps.env`):

| | Windows (PowerShell) | Linux / WSL2 |
|---|---|---|
| Check tools | `scripts\doctor.ps1` | |
| Dependencies | `scripts\fetch-deps.ps1` | `scripts/fetch-deps.sh` |
| Build (+tests, zero warnings) | `scripts\build.ps1` | `scripts/build.sh` |
| All quality gates | `scripts\test.ps1` | `scripts/test.sh` |
| Everything, as local CI | `scripts\ci-local.ps1` | `scripts/ci-local.sh` |
| Windows release from Linux/WSL2 | | `scripts/build-windows-cross.sh` |

Tests that need the original game read `PLEGACY_TEST_GAME_PATH` (the folder
with `PT.EXE`) and are skipped without it. The original game is never part of
the repository.

The rest of this file is the upstream build guide.

## Prerequisites

- CMake 3.25+
- [Ninja](https://ninja-build.org/) build system (optional)
- A C++20 compiler: **GCC 13+**, **Clang 17+**, or **MSVC 2022+**.
  GCC 12 works with the libfmt-dev library as a fallback for std::format; GCC
  11 and below are not supported.
- SDL3 (>= 3.4.0)
- zlib (save file compression)
- [libADLMIDI](https://github.com/Wohlstand/libADLMIDI) for music support.
  Music is enabled by default, so the build requires this unless you configure
  with `-DPLEGACY_ENABLE_MUSIC=OFF`. A system-installed copy is used if one is
  found; the sources can also be picked up automatically from
  `third_party/libadlmidi/`. Download the [1.6.1
  release](https://github.com/Wohlstand/libADLMIDI/releases/tag/v1.6.1)
  and extract it so that `third_party/libadlmidi/CMakeLists.txt` exists (the
  archive unpacks to `libADLMIDI-1.6.1/`, so rename it).

Platform-specific setup:

- **Linux (Debian/Ubuntu):** `sudo apt-get install ninja-build libsdl3-dev pkg-config`
- **macOS:** `brew install sdl3 pkg-config cmake ninja`
- **Windows:** Install SDL3 via `vcpkg`

## Compile

```bash
cmake --preset release
cmake --build --preset release -j
```

If you don't have Ninja, you can build without presets:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
