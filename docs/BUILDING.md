# Building Pizza Legacy

**Pizza Tycoon HD:** the scripts in `scripts/` do all of this for you with
pinned dependency versions (`scripts/deps.env`):

| | Windows (PowerShell) | Linux / WSL2 |
|---|---|---|
| Check tools (REQUIRED vs optional) | `scripts\doctor.ps1` | `scripts/doctor.sh` |
| Dependencies | `scripts\fetch-deps.ps1` | `scripts/fetch-deps.sh` |
| Build (+tests, zero warnings) | `scripts\build.ps1` | `scripts/build.sh` |
| Lint (scripts, Python tools) | `scripts\lint.ps1` | `scripts/lint.sh` |
| Scans (markers, secrets, deps, bandit) | `scripts\scan.ps1` | `scripts/scan.sh` |
| Tests and smoke run | `scripts\test.ps1` | `scripts/test.sh` |
| HD gates (needs the original game) | `scripts\hd-gates.ps1` | `scripts/hd-gates.sh` |
| Everything, as local CI | `scripts\ci-local.ps1` | `scripts/ci-local.sh` |
| Windows release (+ release gate) | `scripts\release.ps1` | `scripts/build-windows-cross.sh` |
| Release gate on an existing zip | `scripts\check-release.ps1` | `scripts/check-release.sh` |

Tests that need the original game read `PLEGACY_TEST_GAME_PATH` (the folder
with `PT.EXE`) and are skipped without it. The original game is never part of
the repository.

## CI runner on your PC (self-hosted, free)

`.github/workflows/ci-local.yml` runs `scripts\ci-local.ps1` on your own
Windows PC through a GitHub *self-hosted runner*: on every push to `main` or
an `integration/...` branch, on pull requests from this repository (never
from forks), by hand (Actions tab, *Run workflow*), and every night from a
clean checkout. The workflow has no job for GitHub-hosted runners, so it
uses no Actions minutes, and it uploads nothing to GitHub's artifact
storage: the evidence stays on the PC (`build\hd-gates`, `build\coverage`,
`build\ci-local-skipped.txt`). Self-hosted runners cost nothing.

Before you start: `scripts\doctor.ps1` passes in a terminal, and PowerShell 7
is installed (`winget install --id Microsoft.PowerShell -e`; the workflow
uses `pwsh`).

1. On GitHub: the repository, **Settings > Actions > Runners > New
   self-hosted runner**, **Windows**, **x64**. The page shows the download
   commands and a registration token (valid for about an hour).
2. In PowerShell, make the runner folder on `C:` (or `G:`, never `D:`), for
   example `C:\actions-runner`, then run the page's download and extract
   commands in it.
3. Register it: `.\config.cmd --url https://github.com/<owner>/<repo> --token <token>`.
   Accept the default labels (`self-hosted`, `Windows`, `X64`: the workflow
   asks for `self-hosted` and `windows`). Answer **Y** to *run the runner as
   a service*, and give your own Windows account, so the job sees the Python,
   Visual Studio and PSScriptAnalyzer you installed for yourself. As a
   service the runner opens no window.
4. Tell the jobs where the game is: create `C:\actions-runner\.env` with
   `PLEGACY_TEST_GAME_PATH=C:\GOG Games\Pizza Connection` (your folder with
   `PT.EXE`; the runner reads this file when it starts). Optional lines:
   `PLEGACY_TEST_HD_PACK=<your 4K pack>` (else the gates build a baseline
   pack) and `PLEGACY_REQUIRE_TOOLS=1` (gitleaks, pip-audit, bandit and
   shellcheck must be installed). Then restart the service:
   `Get-Service actions.runner.* | Restart-Service`.
5. Push a commit, or start the workflow from the Actions tab, and watch the
   job's log there.

Keep the repository private: a self-hosted runner runs the repository's code
with your account's rights. On GitHub, **Settings > Actions > General**, set
*Allow ... actions* to actions created by GitHub (the workflow uses only
`actions/checkout`, pinned to a commit) and *Require approval for all
outside collaborators*. To remove the runner: `.\config.cmd remove --token <token>`
(the token comes from the same Runners page). The runner updates itself.

Not tried yet on the owner's PC: whether the HD gates' Direct3D rendering
works for a runner service, which runs without a desktop. If they fail only
in the Actions job, while `scripts\hd-gates.ps1` passes in a terminal, the
logs are in `build\hd-gates\run-*\` on the PC.

The first run of the coverage gate fails until its budgets are recorded
(see `docs/HD_REMASTER.md`, "HD coverage"): run
`scripts\hd-gates.ps1 -UpdateCoverage` once in a terminal, read
`build\coverage\coverage.md`, and commit `tools\ci\coverage-budget.json`.

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
- **Windows:** `scripts\fetch-deps.ps1` builds the pinned SDL3, zlib and
  Catch2 (`scripts\build.ps1` runs it when needed)

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
