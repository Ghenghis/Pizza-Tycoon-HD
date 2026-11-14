# Contract compliance: gap analysis

Repository: Pizza Tycoon HD (fork of Pizza Legacy 1.0.0-beta.2), commit
`8b92b4f` (HEAD of `main`; `origin/main` is one commit behind at `94d616e`).
Audit date: 2026-11-14.

**Method.** This was a read-only static audit. Nothing was built, configured
or tested, because another engineer was using the 2-CPU machine. The evidence
comes from `git ls-files`, `git log`, `grep`/`find`/`wc`, a read of the
existing build logs under `build/`, `python3 -I` byte-compiling of each tool
(all 7 compile) and a read of the release zip's file list. Wherever a check
needs a build or a lint run, the row says **unverified**.

**Legend.** PASS = meets the contract. PARTIAL = some of it exists, with gaps.
FAIL = missing. N/A = does not apply, but needs a written waiver.

**Origin.** "Upstream" means the line came in with the unmodified import
`a685766`, confirmed with `git log -S`. "Fork" means this repo added it.
**Every forbidden-pattern hit is upstream code.** The fork's own code (60 files
changed or added under `src/` and `tests/`, 7 Python tools) has none.

---

## 0. Scoreboard

| Area | PASS | PARTIAL | FAIL | N/A |
|---|---|---|---|---|
| Contract items 1-14 (85 rows) | 8 | 48 | 27 | 2 |
| Licensing (7 rows) | 3 | 4 | 0 | 0 |
| Copyrighted material (4 rows) | 2 | 2 | 0 | 0 |
| Doc accuracy (section 3) | - | 20 stale or wrong claims | - | - |

No contract item passes in full. The biggest FAIL clusters: security
scanning (secrets and dependencies), the forbidden-pattern, release and
screenshot-on-failure gates, clang-tidy, safe mode, and repo layout.

---

## 1. Checklist

### 1. No placeholders, stubs or fake wiring in runtime code

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| No TODO/FIXME/STUB/PLACEHOLDER/NOT_IMPLEMENTED in runtime code | PARTIAL | 68 raw hits in `git ls-files src tools scripts`. 20 are real (all upstream) and 48 are false positives. See section 2 | Fix or allowlist each real hit (section 2). Rename the false-positive identifiers so a scanner can run with no exceptions |
| No unimplemented handlers | PARTIAL | `src/core/conversations/MayorConversation.cpp:226` (`advance_line_choice() not implemented`, returns false); `:368` (`lines() not implemented for <state>`, returns `{}`) | `:226`: replace with an explicit "the mayor has no choice lines" `Logger::error` + `assert(false)`, or implement it. `:368`: delete the `default:` so `-Wswitch` (with `-Werror`) forces every `MayorLine` to be handled |
| No UI buttons that do nothing | FAIL | `src/views/screens/Advertising.cpp:621-627`: the Shop click returns silently when a campaign already runs (`// TODO: Show error message`) | Post `state_.status().post_direct(StatusMsg{.payload = "There is already a campaign in this category.", .scope = ...})`, as `CityView.cpp:618` does |
| No silent failures in user flows | FAIL | `src/views/screens/menus/MenuSaveAsGame.cpp:123-125`: a failed save only logs (`// TODO: Show error popup to user`). `src/core/save/SaveGameManager.cpp:1223-1225`: `catch (...) { return false; }` drops the reason | Show a popup or status message on failure. In the `catch`, `Logger::error("save {}: {}", filename, e.what())` (catch `std::exception&` first) |
| No empty catch blocks | PASS | Multi-line grep: 0 empty `catch {}` in `src/`, `tests/`. Four `catch (...)` blocks return a value with no log: `SaveGameManager.cpp:323, 406, 1170, 1223` | Add a `Logger::warn` with the file name to all four (upstream code) |
| No "coming soon" / mock data | PASS | `grep -i 'coming soon\|mock data'`: 0 hits | none |
| Unfinished features hidden or marked experimental | PARTIAL | `MenuOptions.cpp:228-245`: "Lighting" and "HD text" show at `hd_scale` 1, where they do nothing (the help text says "HD only"). `NewspaperEventSystem.cpp:119`: `GroundWaterClear` is never scheduled | Build those two items only when `gfx::hd_scale() > 1`, or draw them disabled. For `GroundWaterClear`, schedule it behind the "fix original bugs" rule or record it in `docs/MODERNIZATIONS.md` and delete the TODO |
| Forbidden-pattern gate | FAIL | No scanner in `scripts/` or `tools/` (grep for the pattern list: none) | See fix #5 in section 5: `scripts/scan-forbidden.{sh,ps1}` plus `config/forbidden-allowlist.txt` with expiry dates |

### 2. No mock-only tests

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| C++ tests use real code paths | PASS | `CMakeLists.txt:527-533`: `test_plegacy` links the real engine (`ENGINE_PLEGACY_SOURCES` without `Main.cpp`) and `game_plegacy`. No mocks in `tests/*.cpp` (grep `mock`: 0) | none |
| Each mock has a matching integration/e2e test | PARTIAL | `tools/hdpack/tests/test_hdpack.py:139` `_FakeComfy` (fake ComfyUI HTTP server) has no test against a real ComfyUI. `test_export.py:70,78` monkeypatches `gpu_utilisation` and nothing calls the real `nvidia-smi`. `depthmaps.main()` (model inference) is not tested at all | Add `tests/test_comfy_live.py`, gated on `PLEGACY_TEST_COMFY_SERVER`, that runs `hdpack.py doctor` and `comfy --limit 1` against the real server. Add a test that calls `gpu_utilisation(0)` and accepts `None` or `0..100`. Add a `depthmaps.py --max-side 64` run on 1 picture, gated on `PLEGACY_TEST_CUDA=1` |
| Tests run real code without the original game | PARTIAL | 42 Catch2 cases, all HD rendering (`grep -c TEST_CASE tests/*.cpp`). The ~760 upstream source files (economy, save/load, conversations, AI) have **no tests**. `AssetExportTests.cpp:22` and `HdPackTests.cpp:113` SKIP without game files | Add a save/load round-trip test (`SaveGameManager::save` then `load`, then compare `dump fingerprint`). It needs only the engine's own `data/*.json`, so check first whether it runs without game files |

### 3. Zero warnings by default

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Compiler warnings fail the build | PARTIAL | No `-Werror`, `/WX` or `CMAKE_COMPILE_WARNING_AS_ERROR` anywhere. `scripts/build.sh:14-16` and `build.ps1:18-19` grep `build.log` instead. On an incremental build, files already compiled print no warnings, so old warnings go unseen | `CMakeLists.txt` after line 133: `option(PLEGACY_WERROR "Warnings are errors" ON)`. After the targets: `set_target_properties(plegacy game_plegacy test_plegacy PROPERTIES COMPILE_WARNING_AS_ERROR ${PLEGACY_WERROR})`. CMake 3.24+ has this property and the minimum is 3.25. Keep it off `third_party/libadlmidi` |
| Warning set is strict on every compiler | FAIL | GCC gets only `-Wall` (`CMakeLists.txt:104-115`, with `-Wextra` etc. commented out). The existing logs were built with GCC (`build/release/CMakeCache.txt`: `/usr/bin/c++`), so "0 warnings" there means `-Wall` only. MSVC disables C4100/C4189 (`:119-120`) and Clang `-Wunused-parameter` (`:92`) for the whole project | Uncomment `CMakeLists.txt:107-112` (`-Wextra -Wshadow -Wnull-dereference -Wimplicit-fallthrough`, keep `-Wno-unused-parameter`). Move the global `-Wno-*` and `/wd*` flags into `config/warning-allowlist.cmake`, each with a reason and an expiry date (the contract's time-limited allowlist). Expect an upstream cleanup pass |
| Linker and CMake warnings caught | FAIL | `build.ps1:18` pattern `warning C\d+\|warning:` misses MSVC `warning LNK4xxx` and `Command line warning D9xxx`. `build.sh:10-11` sends configure output to `/dev/null`, which hides CMake `message(WARNING)` (for example `CMakeLists.txt:74`) | Pattern `'warning (C\|LNK\|D)\d+\|warning:\|CMake Warning'`. Configure with `--log-level=WARNING` and fail on `CMake Warning`. On MSVC add `target_link_options(plegacy PRIVATE /WX)` |
| Python linters clean | PARTIAL (unverified) | ruff, black and mypy run only inside `tools/hdpack` (`test.ps1:24-28`, `test.sh:32-37`). `tools/ci/hd_gates.py` is outside that root, so it is never checked (a stray `tools/ci/.ruff_cache` shows someone ran ruff there once). mypy runs on `hdpack.py` and its import `pixelart.py` only | Run from `tools/`: `ruff check hdpack ci`, `black --check hdpack ci`, `mypy hdpack/*.py ci/*.py` (move `pyproject.toml` to `tools/` or add `tools/ci/pyproject.toml`) |
| Script linters | PARTIAL | `scripts/lint.ps1:3-9`: if PSScriptAnalyzer is missing it prints a note and **passes**. No shellcheck for the 10 `.sh` files | Throw when the module is missing (`doctor.ps1` should check for it). Add `shellcheck scripts/*.sh tools/*.sh` to `lint.sh` (new) and to `ci-local.sh` |
| Time-limited allowlist | FAIL | No allowlist file. Inline suppressions: `pixelart.py:15` (file-wide `noqa`), `hdpack.py:48,419,421,529`, `redraw.py:36`, `GlobalStats.cpp:481`, `CityView.cpp:1447` | Keep the justified inline ones. Record the global ones in the allowlist file from the row above |

### 4. Full wiring, validated resources, pinned dependencies

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Every UI control wired | PARTIAL | Advertising Shop button no-op (row 1.3). HD-only toggles do nothing at 1x (row 1.7). `hd_scale` and `hd_pack` cannot be set in game, only in the settings file or on the command line (`HD_REMASTER.md:69-70`) | See rows 1.3 and 1.7. Add `IntSliderMenuItem "HD scale" 1..8` (help: "takes effect after a restart") to `MenuOptions::build_graphics` |
| Every referenced resource exists | FAIL | `CMakeLists.txt:587` runs `tools/package_haiku_zip.sh`, which does not exist. `tools/gen_game_location_screen:21` imports `plcheck`, and there is no `tools/plcheck.py` (ImportError). `docs/CREDITS.md:33` points to `LICENSES/zlib.txt`, but the file is `LICENSES/Zlib.txt` (breaks on case-sensitive systems) | Delete the `release_haiku` target (`:584-594`) or add the script. Restore `tools/plcheck.py` from upstream or inline `game_path`/`resolve_binary`. Change CREDITS to `LICENSES/Zlib.txt` |
| Runtime resources validated | PARTIAL | Pack PNGs: aspect check `HdPack.cpp:172-184` and `hdpack.py validate`, good. `data/*.json`: fatal with a clear message, `DataFile.cpp:24`. But the size is checked only after the PNG is fully decoded (`Png.cpp:27`), so there is no dimension cap | Read the IHDR width/height (bytes 16-23) before `SDL_LoadPNG`. Reject anything over 16384 per side or more than 64x the target size, with a warning |
| C/C++ dependencies pinned | PARTIAL | `scripts/deps.env:2-5` pins by **tag** (mutable) and nothing checks the commit. Vendored headers have no version manifest (they are nlohmann 3.12.0, magic_enum 0.9.6, stb_truetype 1.28, utfcpp unknown) | Add `SDL_SHA=...` etc. to `deps.env`. In `fetch-deps.*`, after cloning, `test "$(git -C $SRC/$1 rev-parse HEAD)" = "$SHA" \|\| fail`. Add `third_party/VERSIONS.md` listing version, upstream URL and SHA256 per header |
| Python lockfile | FAIL | `requirements.txt` pins only Pillow and numpy, with no hashes or transitive lock. `depthmaps.py:67-68` imports `torch` and `transformers`, which are pinned nowhere. `depthmaps.py:30` loads `Depth-Anything-V2-Large-hf` with no `revision=` | `pip-compile --generate-hashes requirements.in -o requirements.lock` (or `uv pip compile`), same for dev and a new `requirements-ai.in` (torch, transformers). Install with `--require-hashes`. Pass `revision="<commit>"` to `pipeline(...)` |
| AI model inputs pinned | PARTIAL | `make-pack.ps1`, `redraw.py:235-236` name checkpoints (`juggernautXL_v9`, `diffusers_xl_depth_full`, `4x-UltraSharp`) with no hashes | Add `tools/hdpack/models.json` with the SHA256 of each file. `hdpack.py doctor` already lists models: also compare hashes through ComfyUI's `/view_metadata` or by hashing the local file |

### 5. Reproducible from a clean clone; local CI mirrors local commands

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Clean clone builds | PARTIAL (unverified) | No tracked file is missing from the build (`comm` of disk against `git ls-files`: only ignored files differ). `source_manifest.cmake` covers all 819 sources. But `CMakeLists.txt:15-19` writes `Version.h` **into the source tree**, and Linux `fetch-deps.sh` relies on system zlib, pkg-config and clang-format, which no doctor checks | Generate `Version.h` into `${CMAKE_BINARY_DIR}/generated` and add that to the include path. Add `scripts/doctor.sh` (git, cmake >= 3.25, ninja, c++, pkg-config, zlib headers, clang-format, python3 >= 3.10, Pillow, numpy) |
| Local CI stops on every failure | FAIL | `scripts/ci-local.ps1:8` runs `& doctor.ps1`. `doctor.ps1:40` ends with `exit 1`, and a script that exits does not throw, so CI **carries on**. Worse, `doctor.ps1:27-28` always fails on a clean clone ("dependencies built", "game built") | `ci-local.ps1:8`: `& "$PSScriptRoot\doctor.ps1" -Prerequisites; if ($LASTEXITCODE) { throw 'doctor failed' }`. Turn the two build-state checks in `doctor.ps1` into `Write-Note` lines when `-Prerequisites` is set |
| The two CI scripts do the same | PARTIAL | `ci-local.ps1:8-14`: doctor, deps, build, test, lint, hd-gates, release. `ci-local.sh:6-34`: deps, build, test, HD smoke, hd_gates `--software`. The sh version has no lint, doctor or release. The ps1 version has no HD smoke | Use one ordered list in both: doctor, deps, build, format-check, lint, scan (forbidden, secrets, deps), test, smoke, e2e gates, release. Add `lint.sh`, `scan.sh`, `release.sh` (or reuse `build-windows-cross.sh`) |
| CI definition in the repo (free, self-hosted) | FAIL | There is no `.github/` folder | `.github/workflows/ci.yml` with `runs-on: [self-hosted, windows]` and a single step `pwsh -NoProfile -File scripts/ci-local.ps1`. Self-hosted runners cost nothing, which keeps to the owner's no-paid-Actions rule |
| Releases traceable to a commit | FAIL | `git log --decorate`: no `v1.0.0`/`v1.1.0` tags, only `upstream-pizzalegacy-1.0.0-beta.2`. `release/PizzaTycoonHD_v1.1.0_win64.zip` (built 09:54) contains `data/fonts/*.ttf` from commit `94d616e` (09:31), which CHANGELOG lists under **Unreleased**. So the "v1.1.0" artifacts were overwritten with later code | See fix #2 in section 5 (release gate) |

### 6. Definition of done

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Builds from clean | PARTIAL (unverified) | Row 5.1 | Row 5.1. CI should build with `cmake --build --clean-first` once a day |
| One-command dev run | PARTIAL | `scripts/run-dev.ps1:14` throws "Run scripts\build.ps1 first" when there is no build. There is no `run-dev.sh` | `run-dev.ps1`: `if (-not (Test-Path (Get-GameExe))) { & "$PSScriptRoot\build.ps1" }`. Add `run-dev.sh` |
| Smoke tests | PARTIAL | A smoke test exists only with game files (`ci-local.sh:11-29`, `hd-gates.ps1:18`). With none, nothing runs the executable | Always run `plegacy --version` and `plegacy --help` and require exit code 0 (`Main.cpp:84-87`, no game files needed) |
| Formatting with no diff | PARTIAL | `test.ps1:14-17` checks only C++ files changed since the upstream tag, and black covers only `tools/hdpack`. Nothing formats `.ps1`, `.sh`, CMake or JSON. `CMakeLists.txt:478-485` has a `format` target that rewrites all of `src/` (upstream files too), which disagrees with the scripts | Run black on `tools/ci` too. Make the `format` target use the same changed-file list (or delete it). Optionally `Invoke-Formatter` for ps1 and `shfmt -d` for sh |
| Strict lint and typecheck | FAIL | No clang-tidy (`clang-tidy` is installed at `/usr/bin/clang-tidy` but no `.clang-tidy` exists). mypy covers 2 of 7 Python tools | Section 14 row 2. Row 3.4 |
| Unit, integration, e2e | PARTIAL | Unit: 42 C++ cases plus pytest. Integration and e2e: `hd_gates.py` and the asset-export tests, but all are **skipped, and so pass,** without `PLEGACY_TEST_GAME_PATH` (`hd-gates.ps1:18`, `hd_gates.py:107-109`: with no pack, `gate_perf` prints "skip perf" and returns True) | With `PLEGACY_REQUIRE_GAME=1` (set on the owner's PC), make every skip a failure: `hd-gates.ps1:18` throws, `gate_perf` returns False with no pack, and `test.ps1` fails when Catch2 or pytest report any skipped tests |
| Secret scan | FAIL | No tool (grep for gitleaks/trufflehog/detect-secrets: none). A manual grep found **no secrets** | `scripts/scan.{sh,ps1}`: `gitleaks detect --source . --redact --no-banner` (free local binary), plus a `.gitleaks.toml` allowlist |
| Dependency vulnerability check | FAIL | No pip-audit or osv-scanner | `pip-audit -r tools/hdpack/requirements.lock --strict`. `osv-scanner scan -r deps/src` checks the cloned SDL, zlib, libADLMIDI and Catch2 by commit. Both go in `scan.*` |
| README/ARCHITECTURE/CHANGELOG/SECURITY up to date | PARTIAL | Section 3 (stale claims) | Section 3 |
| Release build with artifacts | PARTIAL | `release.ps1` and `build-windows-cross.sh:59-76` make the zip, exe and `.sha256` files | See below |
| Windows exe + zip + checksums | PARTIAL | Checksums: yes. But the bare exe cannot run alone. `PlatformPaths.cpp` puts `data_dir` next to the exe, and `DataFile.cpp:24` calls `fatal("Missing data file")` without `data/`. The MSVC exe also needs `SDL3.dll` (`release.ps1:53` says so). README:63 calls the cross-built exe "self-contained" | Either (a) ship `PizzaTycoonHD_v<ver>_win64_setup.exe` built with Inno Setup (free, local) that installs exe, data, hd and LICENSES, or (b) embed `data/` in the exe and fall back to it when `data/` is missing. Link SDL3 and the CRT statically in the MSVC path too (`-DSDL_STATIC=ON`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`) |

### 7. Repo layout

| Path | Status | Evidence | Exact fix |
|---|---|---|---|
| `src/ tests/ docs/ scripts/ assets/` | PASS | present | none |
| `config/` | FAIL | missing | Create it for the warning and forbidden-pattern allowlists, `.gitleaks.toml` and `models.json` |
| `.github/` | FAIL | missing | Row 5.4 (self-hosted workflow), `ISSUE_TEMPLATE/`, `SECURITY` link |
| `README.md`, `SECURITY.md` | PASS | at the root | none |
| `ARCHITECTURE.md`, `CHANGELOG.md` at the root | FAIL | they are in `docs/` | `git mv docs/ARCHITECTURE.md docs/CHANGELOG.md .` and update `release.ps1:13,38`, `build-windows-cross.sh:59,71`, `README.md:145,171` and `CMakeLists.txt` links |
| `LICENSE` | FAIL | only `LICENSES/GPL-3.0-or-later.txt` (GitHub's licence detection needs a root `LICENSE`) | `cp LICENSES/GPL-3.0-or-later.txt LICENSE` (or `COPYING`) |
| `.editorconfig` | FAIL | missing (`.clang-format` exists) | Add one: `root=true`, `[*] end_of_line=lf, insert_final_newline=true, charset=utf-8`; `[*.{cpp,h,inl}] indent_size=2`; `[*.py] indent_size=4, max_line_length=120`; `[*.ps1] end_of_line=crlf` |
| Python tests under `tests/` | PARTIAL | they are in `tools/hdpack/tests/` | Acceptable if documented in ARCHITECTURE ("tests/ = C++, tools/*/tests = Python") |

### 8. Required scripts (Windows and/or sh), fail fast with clear messages

| Script | ps1 | sh | Status | Evidence / fix |
|---|---|---|---|---|
| doctor | yes | no | PARTIAL | Fails on a clean clone (row 5.2). Add `doctor.sh` |
| run-dev | yes | no | PARTIAL | Doesn't build (row 6.2) |
| test | yes | yes | PARTIAL | `test.ps1:20` and `test.sh:29` run `test_plegacy` without checking it exists, so the error is "not recognized" or "not found". Add `if (-not (Test-Path ...)) { throw "No test build at ...; run scripts\build.ps1" }` |
| format | yes | no | PARTIAL | Add `format.sh` |
| lint | yes | no | PARTIAL | PSScriptAnalyzer optional (row 3.5). Add `lint.sh` |
| build | yes | yes | PARTIAL | Grep-based warning check (row 3.1) |
| release | yes | cross only | PARTIAL | `release.ps1:13`: if no CHANGELOG heading matches, StrictMode throws a cryptic `.Matches` error. Fix: `if (-not $m) { throw 'No "Pizza Tycoon HD <ver>" heading in CHANGELOG' }` |

### 9. Gates

| Gate | Status | Evidence | Exact fix |
|---|---|---|---|
| Static: format | PARTIAL | Row 6.4 | Row 6.4 |
| Static: lint | PARTIAL | Rows 3.4, 3.5; no clang-tidy | Row 14.2 |
| Static: typecheck | PARTIAL | mypy on 2 of 7 files | Row 3.4 |
| Static: forbidden-pattern scan | FAIL | none | Fix #5 |
| Runtime smoke | PARTIAL | Only with game files (`ci-local.sh:11-29`; ps1 has none) | Row 6.3 |
| Integration/e2e UI gates | PARTIAL | `hd_gates.py` drives screens with `nav`/`restaurant qs` console commands, not clicks, so UI wiring is never exercised | Add a gate that clicks through the options menu with synthetic mouse events (if `--exec` has an input command) and checks that each control changes `settings` |
| Screenshots on failure | FAIL | `hd_gates.py:149`: every screenshot goes into a `TemporaryDirectory`, which is **deleted** on a failure too | `--artifacts build/hd-gates` (default). Keep the folder when `ok` is False, write `eq_diff.png` (`ImageChops.difference(...).point(lambda v: min(255, v * 32))`) and print its path |
| Release gates | FAIL | `release.ps1` and `build-windows-cross.sh` check nothing before packaging: no tests, no clean tree, no tag, no version match, no scan of the stage for game files. The cross path never builds or runs the tests (`build-windows-cross.sh:40-42` has no `PLEGACY_BUILD_TESTS`) | Fix #2 |

### 10. Safety

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Cancellation of background work | PASS | `HdPack.cpp:41-45, 271-276` (`jthread`, `request_stop`, budget check); `HdTypeface.cpp:167-173` (`stop_token` checked per glyph) | none |
| No heavy work on the UI thread | PARTIAL | On a cache miss, PNG decode and resample run on the main thread (`HdPack::decode`, `HdPack.cpp:160-200`). Save and load run on the main thread (`BusyCursor::run`, upstream). `Renderer.cpp:632` and `HdCompositor.cpp:256` create up to 7 `jthread`s **per call** (per frame on the CPU path or for big uploads) | Use one persistent worker pool (created once in `Renderer`) for row expansion and premultiply. On a decode miss, show the indexed art for that frame and finish the decode in the prefetcher, instead of blocking |
| Bounded queues and caches | PARTIAL | Bounded: prefetch at 1.5 GiB (`Game.cpp:203`), GPU textures LRU at 1.5 GiB (`HdCompositor.h:173`), text pictures at 128 MiB (`Font.cpp:126`). Unbounded: the glyph cache (`HdTypeface.cpp:88`) and `workers` (`:95`, which grows by one thread per `prewarm` and is emptied only by `wait_for_prewarm`) | Cap the glyph cache (LRU, for example 32 MiB). Remove finished workers in `prewarm` (hold `std::stop_source` and a done flag) |
| Timeouts on network/process calls | PARTIAL | Python: `urlopen(timeout=)` `hdpack.py:421`, `nvidia-smi timeout=10` `:533`, ComfyUI job deadline `:472-483`, game run `hd_gates.py:59` (900 s), doctor `Invoke-RestMethod -TimeoutSec 5`. **Unbounded**: `wait_for_gpu` (`hdpack.py:542-553`, `while True`) | Add `--wait-max SECONDS` (default 3600) and raise `HdPackError` when it runs out |
| No leaks | PARTIAL | ASan/UBSan only for **Clang Debug** (`CMakeLists.txt:98-102`). No script runs it. The scripts' GCC Release build has none | CMake presets `asan` (`-fsanitize=address,undefined -fno-omit-frame-pointer`, GCC or Clang) and `tsan`, plus `scripts/sanitize.sh` that runs `test_plegacy` under both with `ASAN_OPTIONS=detect_leaks=1` |
| Safe mode for heavy loads | FAIL | No `--safe-mode` (grep `safe.?mode`: 0). The 1.5 GiB budgets are fixed. A failed texture creation only logs (`HdCompositor.cpp:106,189,197`) and nothing falls back | `--safe-mode` / `safe_mode` setting: `hd_scale=1`, no pack, lighting and smooth motion off, no prefetch, software renderer. Fall back on its own to CPU compositing (`PLEGACY_CPU_COMPOSE` path) after N texture failures. Scale the budgets to `SDL_GetSystemRAM()` |

### 11. Security

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| No secrets in the repo | PASS | grep for key/token/password/AKIA/ghp_/hf_/sk-/PRIVATE KEY in all tracked text: 0 real hits. One minor leak: local machine paths in `docs/hd-audit.md:9-10` (an export folder and a pack folder) | Replace them with `<export>` and `<pack12>` |
| Secret scanning | FAIL | none | Row 6.7 |
| Dependency scanning | FAIL | none | Row 6.8. `SECURITY.md:26-28` should describe how often deps are checked |
| SECURITY.md lists the risky surfaces | PARTIAL | Lists game files, packs, saves, ComfyUI and workflows (`SECURITY.md:12-18`). **Missing:** (a) `--exec` scripts and the debug console, which write files to any path (`screenshot`, `savevga`, `dumpgameloc`, `dump fingerprint`, `ScreenCommands.cpp:31-142`, `DebugCommands.cpp:182`); (b) recipe `.pc` file import (README:119-122); (c) save files from other people (README:179 asks for them; caps exist at `SaveGameManager.cpp:58,71`); (d) `data/fonts` TTFs parsed by stb_truetype, which is not hardened against untrusted fonts; (e) `hd_warm.txt` entries are paths that are opened without checks; (f) `depthmaps.py` downloads model weights from Hugging Face; (g) mods: none yet, roadmap only (`docs/blueprint/sdk-roadmap.md`) | Add rows (a)-(g). Change `SECURITY.md:20-21` "opens no network connections" to "the game opens none; `hdpack.py comfy`, `redraw.py` and `depthmaps.py` (Hugging Face download) do". Note "no PNG dimension cap" until row 4.3 is fixed |
| SECURITY.md is accurate | PARTIAL | `:16` "Saves and settings: Unchanged from Pizza Legacy", yet the fork added `hd_*` settings and `hd_warm.txt` (`ConfigManager.cpp` changed) | Change it to "settings gain `hd_scale`, `hd_pack`, `hd_text`, `lighting`, `smooth_motion`; `hd_warm.txt` is written next to them" |

### 12. UI

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| Tooltip or help text on every control | PARTIAL | `MenuOptions.cpp`: 31 `help_text` calls. Items without one: "Sound Volume" `:162`, "Music Volume" `:165`, "Auto save slots" `:278`, "Show advanced options" `:457` | Add `.help_text(...)` to those four |
| Search, filter and sort on lists | N/A (needs waiver) | The game's lists (saves, recipes, staff) recreate the 1994 screens. Adding search would change the original layout, which `HD_REMASTER.md:3-5` promises to keep | Write the waiver into `docs/ARCHITECTURE.md` ("faithful UI: no search/filter in game screens") and require search, filter and sort for any new tool UI |
| Dense control-panel style | N/A (needs waiver) | Same reason | Same waiver |

### 13. Python

| Tool | Status | Evidence | Exact fix |
|---|---|---|---|
| ruff | PARTIAL | `pyproject.toml:16-21` selects `E,F,W,I,B,UP,S,SIM,N` and runs on `tools/hdpack` only | Also run it on `tools/ci` and add `RUF,PT,PL` |
| black | PARTIAL | `tools/hdpack` only. `hd_gates.py` wraps at 100 columns, not the project's 120 | Row 3.4 |
| mypy (strict) | PARTIAL | `strict = true` (`pyproject.toml:24`), but it runs on `hdpack.py` and `pixelart.py` only, with `ignore_missing_imports = true` | `mypy *.py ../ci/*.py`. Replace the global `ignore_missing_imports` with per-module overrides for `torch.*` and `transformers.*` |
| pytest with real fixtures | PASS | `conftest.py:33-53` writes the engine's real export format. `real_export` uses the engine's own export. `filterwarnings=error` | none (mock gaps in row 2.2) |
| pip-audit | FAIL | none | Row 6.8 |
| bandit | PARTIAL | ruff `S` (flake8-bandit rules) is on, the real `bandit` is not run | `bandit -q -r tools -x tools/hdpack/tests` in `scan.*` |

### 14. C++

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| TreatWarningsAsErrors equivalent | PARTIAL | Log grep only (row 3.1) | Row 3.1 |
| clang-tidy or analyzers | FAIL | No `.clang-tidy`. `compile_commands.json` is produced (`CMakeLists.txt:64`) but nothing uses it | Add `.clang-tidy` (`bugprone-*,performance-*,modernize-use-nullptr,cppcoreguidelines-pro-type-member-init,-bugprone-easily-swappable-parameters`, `WarningsAsErrors: '*'`). In `lint.*`, run `run-clang-tidy -p build/release` on `Get-ChangedCppFiles`. On MSVC, `/analyze` is an option |
| Sanitizer build | PARTIAL | `build/tsan` exists but was set up by hand (`CMakeCache.txt`: `CMAKE_CXX_FLAGS=-fsanitize=thread -g`). It is in no preset or script, though CHANGELOG 1.1.0 says "Checked with ThreadSanitizer" | Row 10.5 (`tsan` preset and `sanitize.sh`) |

### Licensing

| Requirement | Status | Evidence | Exact fix |
|---|---|---|---|
| SPDX headers | PARTIAL | Fork C++ and Python files have them (for example `HdCompositor.cpp:1-4`, `hdpack.py:1-2`, `pixelart.py:1-3` MIT plus both copyright holders). **Missing** in all 19 `scripts/*`, `tools/check_dependency_rule.cmake`, `gen_game_location_screen`, `gen_icons.sh`, `gen_source_manifest.sh`, `package_*.sh/.ps1`, `tools/hdpack/pyproject.toml`, and 3 upstream sources: `src/engine/audio/AudioManager.h`, `src/views/helpers/TypewriterTextRenderer.{h,cpp}` | Add `# SPDX-FileCopyrightText: 2026 ShadowByte` and `# SPDX-License-Identifier: GPL-3.0-or-later`. Add a `REUSE.toml` covering binaries and JSON (`data/**`, `assets/**`, `screenshots/**`, `tools/hdpack/*.json`). Then `reuse lint` in `scan.*` |
| `LICENSES/` complete | PASS | 14 texts: GPL-3.0+, LGPL-3.0+, BSL-1.0, Zlib ×2, MIT ×4 (magic_enum, mmpx, nlohmann, stb), OFL ×5 | Optionally rename to SPDX names (`Zlib.txt`, `MIT.txt` plus per-holder notices) for `reuse lint` |
| `docs/CREDITS.md` correct | PARTIAL | Wrong path `LICENSES/zlib.txt` (`:33`). The MMPX entry (`:100-106`) names only `Mmpx.cpp`, not the Python port `tools/hdpack/pixelart.py`. Nothing records how the AI art was made, though commit `3b7f0b6` says `titel1.png` and the box cover were made with Flux dev and 4x-UltraSharp | Fix the path. Add `pixelart.py` to MMPX. Add an "Art provenance" section: tool, model and licence (FLUX.1-dev licence, 4x-UltraSharp CC BY-NC-SA 4.0), whether original art was an input, prompts and seeds |
| OFL fonts credited | PASS | `CREDITS.md:83-96`, the OFL texts are in `data/fonts/` and `LICENSES/` and ship in the zip (20 `.txt` files). No Reserved Font Name is declared, so the Archivo and Crimson Pro fixed instances are allowed | none |
| stb (MIT) credited | PASS | `CREDITS.md:76-81`, `LICENSES/MIT-stb.txt` | none |
| GPL source offer for binaries | PARTIAL | The release zip carries `LICENSES/` but neither a link to this fork's source nor the source itself. `CREDITS.md:53` points to the upstream Codeberg tree. The GitHub repo is private | If a release is ever shared, ship `PizzaTycoonHD_v<ver>_src.zip` (`git archive`) next to it, or a written offer (GPL-3.0 §6) |
| Copyright holder line | PARTIAL | `CREDITS.md:5` gives upstream's copyright as "(C) 2026 Cowomaly", but upstream file headers say 2025 (`CMakeLists.txt:1`) | Write "2025-2026" |

### Copyrighted material

| Check | Status | Evidence | Exact fix |
|---|---|---|---|
| No original game files committed | PASS | `git ls-files` for `.vga .bob .pc .fnt .e .cmf .voc .mid .xmi`: only `data/game_location.vga`, which the engine generates (`tools/gen_game_location_screen`, `VgaWriter`), comes from upstream and is shipped by upstream. The release zip contains no game files (only that `.vga`). `.gitignore:16-20` excludes `export/`, `pack/`, `hd/`, `shots/` | none |
| Derived from original art | PARTIAL (review) | `data/game_location.vga` is drawn **with the original game's fonts** (`gen_game_location_screen:4-6`). `screenshots/hd_compare.png` (fork, `c35e3fc`) shows original art and an upscale of it. `screenshots/citymap.png` and `pizzadesign.png` are upstream gameplay screenshots | Low risk and upstream does the same. Record it in `CREDITS.md` ("screenshots show the original game's art, © Cybernetic Corporation / Software 2000, for documentation"). Keep `hd_compare.png` out of release zips (it already is) |
| Large binaries | PARTIAL | `assets/art/PizzaTycoonHD_BoxCover_2880x3840.png` 13.1 MB, `assets/hd/gfx/titel1.png` 10.4 MB, `assets/Pizza Legacy.icns` 4.8 MB, all in plain git | `git lfs track "assets/art/*.png" "assets/hd/**/*.png"` for **future** art (old history stays). Or keep the masters outside the repo and commit WebP/JPEG versions |
| Release folder not committed | PASS | `.gitignore:23` `/release/`, `:25` `*.zip` | none |

---

## 2. Forbidden-pattern hits

Command: `git ls-files src tests tools scripts CMakeLists.txt | xargs grep -nIiE 'TODO|FIXME|\bSTUB|PLACEHOLDER|NOT_IMPLEMENTED|not implemented|unimplemented|coming soon|\bXXX\b|\bHACK\b|mock'`
gives **68 hits**: 0 in `tests/` and `scripts/`, 18 in `tools/`, 50 in `src/`.
All `src/` hits are **upstream** (`git log -S` points at `a685766`).

### Real problems (20)

| # | File:line | Text | Impact | Fix |
|---|---|---|---|---|
| R1 | `src/views/screens/Advertising.cpp:625` | `TODO: Show error message "There is already a campaign..."` | Button does nothing | Status message (row 1.3) |
| R2 | `src/views/screens/menus/MenuSaveAsGame.cpp:125` | `TODO: Show error popup to user` | Save failure is silent | Popup (row 1.4) |
| R3 | `src/core/conversations/MayorConversation.cpp:226` | `advance_line_choice() not implemented` | Unimplemented handler | Row 1.2 |
| R4 | `src/core/conversations/MayorConversation.cpp:368` | `lines() not implemented for <state>` | Empty dialogue if reached | Exhaustive switch |
| R5 | `src/core/newspaper/NewspaperEventSystem.cpp:119` | `TODO: ... never scheduled in the original` | Dead event | Decide and document (row 1.7) |
| R6 | `src/core/conversations/PoliceConversation.cpp:81` | `TODO: Find depot inspection date` | Missing value in dialogue | Implement, or document it as unknown in `docs/blueprint/game-systems.md` |
| R7 | `src/views/screens/restaurant/RestaurantView.cpp:1254` | `fill(14); // TODO: figure out proper background color` (fork-modified file) | Guessed colour | Measure it from the original, or document it as a choice |
| R8 | `src/views/screens/city_view/CityView.cpp:611` | `TODO deal with competitors` | Behaviour gap (likely faithful) | Confirm against the original and replace the TODO with a statement |
| R9 | `src/core/city/Car.cpp:19` | `TODO: Give cars a proper cleanup flag` | Code debt | Fix, or move to the issue tracker |
| R10 | `src/core/city/City.cpp:155` | `Todo: something with our premises array.` | Code debt | Same |
| R11 | `src/core/city/Property.cpp:221` | `TODO: Should we use some method in City...` | Code debt | Same |
| R12 | `src/core/character/Actor.h:42` | `Todo: get background_id from actor_type.` | Code debt | Same |
| R13 | `src/core/factories/CityFactory.cpp:693` | `Todo: restaurant?` | Unclear parsing of map type 0x04 | Check against the data and document it |
| R14 | `src/engine/input/InputManager.cpp:245` | `TODO: add options for this?` (first gamepad only) | Feature gap | Document the "first gamepad only" limit |
| R15 | `src/views/layers/ConsoleLayer.cpp:265` | `TODO calculate this` (page size) | Cosmetic | Use the visible line count |
| R16 | `src/views/layers/DebugLayer.cpp:23` | `TODO: Introduce some kind of DebugOverlayItem bus` | Debt | Issue tracker |
| R17 | `src/views/screens/Terminal.cpp:95` (fork-modified file) | `TODO: Some less hardcoded way...` | Debt | Issue tracker |
| R18 | `src/views/screens/Terminal.cpp:374` | `TODO: We have TextStyle now...` | Debt | Use `TextStyle` |
| R19 | `src/views/screens/menus/CharacterSelection.cpp:157` | `TODO: Move the background into their respective boxes` | Debt | Issue tracker |
| R20 | `src/views/screens/pizza/PizzaDesign.cpp:433` | `TODO maybe refactor this` | Debt | Issue tracker |

R9-R20 do not change runtime behaviour. Under the contract they still need
either a fix or an allowlist entry with an expiry date. The cheapest honest
route is to move each one to an issue and replace the comment with
`// See issue #N`, which the scanner allows.

### Misleading comment (1)

| File:line | Text | Verdict | Fix |
|---|---|---|---|
| `src/core/debug/commands/RestaurantCommands.cpp:164-165` | `For now, clearing is not implemented as there's no clear() method` | Stale: line 162 does call `restaurant->layout()->clear()` | Delete the comment |

### False positives (47)

| File:line(s) | Why | Fix (so the scanner can stay strict) |
|---|---|---|
| `src/core/state/GameState.cpp:348, 1073`; `GameState.h:164`; `src/engine/Game.cpp:312` | `create_placeholder_players()`: default players before character selection | Rename to `create_default_players` |
| `src/engine/gfx/PaletteBank.h:79, 88, 135, 157, 161, 177`; `src/resources/PaletteLoader.cpp:61` | A real fallback palette used before the game files load | Rename to `load_fallback` / `fallback_` |
| `src/views/ui/widgets/TextListBox.cpp:19, 25, 31, 35, 37, 106, 107, 108`; `TextListBox.h:38, 40, 65` | Empty-list hint text (UI "placeholder") | Rename to `empty_text` |
| `src/core/newspaper/ArticleDatabase.cpp:72, 235`; `ArticleDatabase.h:26` | `#` template placeholders in the original newspaper data | Allowlist (domain term) |
| `src/core/debug/command/ParsedValue.h:67` | Value for an omitted optional argument | Allowlist |
| `src/core/factories/CityFactory.cpp:519` | Describes a field of the original data format | Allowlist |
| `src/core/restaurant/Restaurant.h:391` | "1 kg placeholder, as the original does": faithful rule | Allowlist |
| `src/resources/BlitmapManager.cpp:32` | `static_assert` text: a compile-time guard | Allowlist |
| `tools/hdpack/hdpack.py:381, 383, 396, 402, 620, 800` | `{{placeholder}}` ComfyUI template syntax | Allowlist (`{{`) |
| `tools/hdpack/hdpack.py:610, 611, 613, 615, 622, 643, 647`; `tools/hdpack/depthmaps.py:74, 75, 79, 101, 103` | A variable named `todo` | Rename to `pending` |

The scanner (fix #5) should use word boundaries
(`\b(TODO|FIXME|STUB|XXX|HACK)\b`, `NOT_IMPLEMENTED`, `not implemented`,
`coming soon`, `mock data`) and treat `placeholder` as a hit only in comments.
After the renames, it would report R1-R20 and nothing else.

---

## 3. Docs against code: stale or wrong claims

| # | Doc:line | Claim | What the code does | Fix |
|---|---|---|---|---|
| D1 | `docs/HD_REMASTER.md:225-227` | "The indexed pixels still get the original letters ... only the HD picture shows the new ones" | `src/resources/Font.cpp:657-661` (and `finish_hd_line`, `:409-418`): with HD text on, **only** the HD letters are drawn (`finish_hd_line`), and the indexed pixels keep what was under them. The CHANGELOG "Unreleased" entry says the same as the code | "The letters live only in the HD picture: the indexed pixels under them keep the background, and turning `hd_text` off (or `hd_scale` 1) draws the 1994 letters instead" |
| D2 | `docs/HD_REMASTER.md:29-30` | "Fonts are not part of a pack yet: text is drawn from the original font, scaled up" | HD text (`94d616e`) draws text with 5 OFL typefaces at HD resolution | "Fonts aren't pack files: at hd_scale 2+ text is redrawn with modern typefaces (see HD text)" |
| D3 | `docs/HD_REMASTER.md:111-116` | "blitting a surface that has HD copies its texels", and text counts as plain pixels that clear HD | Surfaces keep an `HdList` draw list, not texel copies (`HdList.h:25-36`), as lines 122-124 of the same doc say. HD text is no longer a plain write | Rewrite in terms of the draw list: "a blit appends the source's pictures; a plain write ends them under that pixel" |
| D4 | `docs/HD_REMASTER.md:117-121` | "The renderer expands each layer ... HD texels are laid over them. Rows are split across CPU threads. The result is uploaded as one texture" | That is the `PLEGACY_CPU_COMPOSE=1` fallback. The default is GPU (`HdCompositor`) | Label it "CPU fallback", or delete it and keep lines 122-126 |
| D5 | `docs/HD_REMASTER.md:65-70` | The settings table lists only `hd_scale` and `hd_pack` | `hd_text`, `lighting` and `smooth_motion` exist (`MenuOptions.cpp:219-245`). Lighting and smooth motion are not documented anywhere in this file | Add the three rows and a short "HD lighting" section |
| D6 | `docs/ARCHITECTURE.md:53-58` | The HD overlay "lays the texels over the upscaled indexed pixels"; no mention of the GPU path | Missing: `HdList`, `HdCompositor` (GPU quads), `LightPass`/`Lighting`, `HdTypeface`, the prefetch thread and warm list, `GuestWalkScene` smooth motion | Rewrite the section, one paragraph per component, with its threads and budgets |
| D7 | `docs/ARCHITECTURE.md:64-65` | Key bindings configurable "though not through a UI yet" | There is a Key bindings screen (`MenuOptions.cpp:380`; README:131-133) | Delete the parenthesis |
| D8 | `docs/ARCHITECTURE.md:94` | "Binary releases are available for Windows, Linux, macOS, and Haiku" | This fork releases Windows only, and the Haiku packager is missing (row 4.2) | "Pizza Tycoon HD releases: Windows (exe + zip). Upstream: ..." |
| D9 | `docs/ARCHITECTURE.md:102-104` | "the only external files accessed are the original game assets" | Also read: the HD pack folder, `data/fonts`, the `hd/` folder, `hd_warm.txt`, the `--exec` script, the export target and console output paths | List them |
| D10 | `README.md:50` | "`build.ps1` Release build (vcpkg + CMake + Ninja)" | `build.ps1` uses `fetch-deps.ps1` (git clone + CMake), not vcpkg. `.gitignore:24` still has `/vcpkg_installed/` | "(pinned deps from fetch-deps + CMake + Ninja)". Drop the vcpkg ignore |
| D11 | `README.md:21-22` vs `HD_REMASTER.md:133` vs `CHANGELOG.md` 1.1.0 | 3.3 ms / 300 fps, vs 3.5 ms / 285 fps, vs 4.4 ms p99 | Three numbers for one benchmark | Quote one table (HD_REMASTER) and link to it |
| D12 | `README.md:26-27` | Smooth motion leaves the game unchanged, "which the local CI proves on every build" | The gate is skipped (and passes) without `PLEGACY_TEST_GAME_PATH` (`hd-gates.ps1:18`) | "...on every build where the original game is available" (or fix row 6.6) |
| D13 | `README.md:62-64` | `build-windows-cross.sh` makes a "self-contained" exe | The exe has no DLL dependencies but still needs `data/` next to it (`DataFile.cpp:24`) | "...an exe with no DLL dependencies; it still needs the `data` folder from the zip" |
| D14 | `README.md:7-27` | The fork feature list | Missing HD text and HD lighting (both on by default) | Add two bullets |
| D15 | `README.md:131-137` | The key bindings paragraph is duplicated (line 135 starts with a lower-case "gamepad bindings...") | Upstream copy error | Delete lines 135-137 |
| D16 | `SECURITY.md:16, 20-21` | Settings "unchanged"; the program "opens no network connections" | Row 11.4/11.5 | Row 11.4/11.5 |
| D17 | `docs/CREDITS.md:33` | `LICENSES/zlib.txt` | The file is `LICENSES/Zlib.txt` | Fix the path |
| D18 | `docs/BUILDING.md:36` | Linux "Check tools" cell is empty | Correct today, but it shows the missing `doctor.sh` | Add `scripts/doctor.sh` |
| D19 | `CMakeLists.txt:6, 11` vs `docs/CHANGELOG.md` | The game reports "Pizza Legacy 1.0.0-beta.2" (`Main.cpp:36`, `MenuMain.cpp:64`, window title `Game.cpp:651`), while the release is "Pizza Tycoon HD 1.1.0" | Two version sources | Fix #2 |
| D20 | `docs/hd-audit.md:9-10` | Local absolute paths | Not portable, and leaks machine paths | Row 11.1 |

---

## 4. Other notes (not contract rows)

* `HEAD` (`8b92b4f`, blueprint and HD audit) is **not pushed**:
  `origin/main` is at `94d616e`. Push once this report is reviewed.
* Settings and saves share upstream's folder (`nl.pizzalegacy/Pizza Legacy`,
  `PlatformPaths.cpp`), so the HD fork and upstream Pizza Legacy overwrite
  each other's `settings.conf` (and the fork adds `hd_*` keys to it). Consider
  `SDL_GetPrefPath("nl.pizzalegacy", "Pizza Tycoon HD")` with a one-time copy
  of the old settings.
* `Get-ChangedCppFiles` (`common.ps1:66-78`) and `test.sh:15-23` keep upstream
  files out of the format check on purpose. That is reasonable, but the same
  exception should cover warnings and clang-tidy in a written
  allowlist (row 3.6).

---

## 5. Ranked top fixes

Ranked by risk to the owner (lost or untraceable releases, silent CI passes)
first, then by effort (S under half a day, M 1-3 days).

1. **Real warnings-as-errors (S).** `COMPILE_WARNING_AS_ERROR` on `plegacy`,
   `game_plegacy` and `test_plegacy`. Turn GCC's `-Wextra` set back on
   (`CMakeLists.txt:107-112`). Move the global `-Wno-*` and `/wd*` flags into
   a dated allowlist. Catch `LNK` and `D9` warnings and `CMake Warning` in the
   scripts. Today "0 warnings" means GCC `-Wall` on the files that happened to
   recompile.
2. **Release gate and one version (M).** Set `project(... VERSION 1.1.0)`
   with an empty suffix, and show "Pizza Tycoon HD 1.1.0 (Pizza Legacy
   1.0.0-beta.2)" in the title, `--version` and the main menu. In `release.ps1`
   and `build-windows-cross.sh`, refuse to package when: `git status
   --porcelain` is not empty; `git describe --exact-match` is not `v$Version`;
   CHANGELOG's first heading is "Unreleased"; tests haven't passed on this
   commit (`build/release/.tests-passed-<sha>` stamp written by `test.*`);
   `release/<name>` already exists (unless `-Force`); or the stage contains any
   `.bob/.fnt/.e/.cmf/.voc/.pc` file or any `.vga` other than
   `game_location.vga`. Then tag `v1.0.0`/`v1.1.0` and rebuild 1.1.0 from its
   commit (the current 1.1.0 zip holds unreleased HD text).
3. **CI that cannot pass by accident (S).** Check `$LASTEXITCODE` after
   `doctor.ps1` (`ci-local.ps1:8`) and split doctor's prerequisites from build
   state. With `PLEGACY_REQUIRE_GAME=1`, every skipped test or gate fails
   (`hd-gates.ps1:18`, `gate_perf`, Catch2 and pytest skips). Make PSScriptAnalyzer
   required. Use the same step list in `ci-local.ps1` and `ci-local.sh`.
4. **Security scans (S).** Add `scripts/scan.{ps1,sh}`: `gitleaks detect`,
   `pip-audit --strict` on a hashed lockfile, `bandit -r tools`,
   `osv-scanner scan -r deps/src`, `reuse lint`. Run it in both CI scripts.
   Extend `SECURITY.md` with `--exec` and the console's file writes, recipe
   import, shared saves, stb_truetype fonts, `hd_warm.txt` and the Hugging
   Face download (row 11.4).
5. **Forbidden-pattern gate (S + M).** Add `scripts/scan-forbidden.{ps1,sh}`
   with the word-boundary regex from section 2, over `git ls-files src tools
   scripts` (without `third_party`), and `config/forbidden-allowlist.txt`
   (`path:line  sha1-of-line  expires=YYYY-MM-DD  reason`). Fail on any hit
   that is not listed, or whose entry has expired. Fix R1-R4 now: the two
   silent UI failures and the Mayor stubs. Rename the false-positive
   identifiers. Move R9-R20 to issues.
6. **e2e gates that keep evidence (S).** `hd_gates.py`: keep the work folder
   and write a diff image on failure (`--artifacts build/hd-gates`). Add an
   always-on smoke test (`plegacy --version`/`--help`). Add one options-menu
   click-through gate.
7. **Python strictness and lockfiles (S).** Run ruff, black and mypy on all
   of `tools/hdpack/*.py` and `tools/ci/*.py`. Use hashed lockfiles for
   runtime, dev and a new AI extra (torch, transformers). Pin the
   Depth Anything `revision=`. Add `--wait-max` to `wait_for_gpu`.
8. **C++ analyzers and sanitizers (M).** Add `.clang-tidy` and run it on the
   fork's changed files in `lint.*`. Add `asan` and `tsan` CMake presets and
   `scripts/sanitize.sh` that runs `test_plegacy` under each. This makes the
   CHANGELOG's ThreadSanitizer claim reproducible.
9. **A Windows exe that runs alone (M).** Either an Inno Setup installer exe,
   or `data/` embedded with a fallback when the folder is missing. Link SDL3
   and the CRT statically in the MSVC path too, so `release.ps1` and the cross
   build produce the same kind of artifact.
10. **Fix the stale docs (S).** D1-D20, starting with D1-D4 (HD_REMASTER),
    D6/D9 (ARCHITECTURE), D10/D12/D13 (README) and D17 (CREDITS).
11. **Pin C/C++ dependencies by commit (S).** Add `*_SHA` in `deps.env`,
    checked after clone. Add `third_party/VERSIONS.md` for the vendored
    headers.
12. **Repo layout (S).** Root `LICENSE`, `.editorconfig`, `config/`,
    `.github/workflows/ci.yml` on a **self-hosted** runner (free). Move
    `ARCHITECTURE.md` and `CHANGELOG.md` to the root and update the four
    scripts that read `docs\CHANGELOG.md`. SPDX headers on the 30 files that
    lack them, plus `REUSE.toml`.
13. **Broken references (S).** Delete `release_haiku` or add the script. Restore
    `tools/plcheck.py`. Fix `LICENSES/Zlib.txt` in CREDITS.
14. **UI honesty (S).** Hide "Lighting" and "HD text" at `hd_scale` 1. Add help
    text to the 4 items that lack it. Add an "HD scale" item. Write the
    faithful-UI waiver for search, filter and dense style.
15. **Safe mode and runtime budgets (M).** Add `--safe-mode`, an automatic
    CPU-compositing fallback after texture failures, budgets scaled to RAM, a
    persistent worker pool instead of per-frame `jthread`s, a capped glyph
    cache, and a PNG dimension cap before decode.
16. **Art provenance and GPL source offer (S).** Add a CREDITS section for the
    Flux and 4x-UltraSharp art (models, licences, whether original art was an
    input). Ship a source zip or written offer with any release that leaves the
    owner's machine. Put a credit line on the screenshots.

---

## Appendix: commands used

```
git ls-files | wc -l                      # 960 tracked files
git ls-files | grep -iE '\.(vga|bob|pc|fnt|e|cmf|...)$'
git ls-files -z | xargs -0 du -b | sort -rn | head
git log --oneline -S'<text>'              # origin of each hit
git log --format= --name-only a685766..HEAD -- src tests   # 60 fork files
grep -nIiE '<forbidden patterns>' $(git ls-files src tests tools scripts)
grep -Pzo 'catch\s*\([^)]*\)\s*\{\s*\}'   # empty catch: none
grep -c warning build/release/build.log build/win64/build.log   # 0 / 0 (GCC -Wall, incremental)
python3 -I -c 'py_compile.compile(..., cfile=/tmp/...)'          # 7/7 tools compile
python3 -I -c 'zipfile...' release/PizzaTycoonHD_v1.1.0_win64.zip   # 58 entries, 1 .vga (engine file)
```
