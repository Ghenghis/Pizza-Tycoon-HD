# Security

## Reporting

Open a private security advisory on the GitHub repository, or an issue
without exploit details asking for a contact. For problems in the
unchanged Pizza Legacy engine, also tell upstream:
https://codeberg.org/cowomaly/pizzalegacy/issues

## What the program touches

The game itself opens no network connections, loads no plugins and runs no
downloaded code. Everything below is data it reads or files it writes, plus
the pack tools, which do talk to the network.

| Surface | Risk | Mitigation |
|---|---|---|
| Original game files (VGA, BOB, FNT, VOC, CMF, data) | Malformed files from an untrusted copy | Read-only; bounds checks in the loaders; the exporter checks headers before decoding and skips bad files |
| HD packs (PNG files, the `hd` folder, `--hd-pack`) | Untrusted images in packs shared by others | Data only: a pack can't run code. Decoded by SDL3's PNG loader; size and aspect checked before use; a bad file is ignored with a warning, never fatal. A decoder bug in a dependency could still crash the game |
| Mods | Code from others | There are none: the game has no mod or plugin loader (the SDK is a roadmap item, `docs/blueprint/sdk-roadmap.md`). Packs are the only add-ons, and they are pictures |
| Saved games from other people (bug reports ask for them) | Crafted files | zlib-compressed JSON behind a `PLSAVE` header; payload capped at 256 MB and metadata at 1 MB (`SaveGameManager.cpp`). Loading one changes only that game |
| Recipe import (`.pc` files in `recipes/`, right-click the NEW row) | Crafted recipe files | Read only from the `recipes/` folder of the user's data folder; a recipe is data (toppings and amounts), no code |
| Settings (`settings.conf`), key bindings, `hd_warm.txt` | Local files in the user's data folder | Unknown keys are dropped with a warning. The fork adds `hd_scale`, `hd_pack`, `hd_text`, `lighting` and `smooth_motion`, and writes `hd_warm.txt` (pack pictures to pre-load) next to the settings; its entries must be relative paths inside the pack, without `..` |
| HD typefaces (`data/fonts`, `hd_fonts.json`) | stb_truetype is not hardened against malicious fonts (its own documentation says so) | Only the fonts shipped in `data/fonts` are used. `hd_fonts.json` takes plain file names only (a name with a folder in it is refused), so it can't point outside `data/fonts`. Don't put fonts from untrusted sources there |
| Self-hosted CI runner (`.github/workflows/ci-local.yml`) | GitHub jobs run the repository's code on the owner's PC with the runner account's rights | The repository is private; the job skips pull requests from forks; only `actions/checkout` is used, pinned to a commit, with no stored credentials; no secrets are used and nothing is uploaded. Setup and settings: `docs/BUILDING.md` |
| `--exec` scripts and console commands | Commands write files wherever they are told: `screenshot`, `savevga`, `dumpgameloc`, `dump fingerprint`, `debug bindings save`, and `--export-assets` | The debug console has no key by default (bind one on the Key bindings screen to open it). Run `--exec` scripts and `console <cmd>` lines only if you wrote them or read them: they act with your rights |
| `hdpack.py comfy`, `redraw.py` | HTTP to a ComfyUI server | Only `http`/`https` URLs accepted; default is `127.0.0.1`; nothing else is contacted; no credentials are sent |
| ComfyUI workflows (`tools/hdpack/workflows`, `--workflow`) | Workflow JSON is sent to ComfyUI as-is | Only use workflows you trust: ComfyUI custom nodes can run code on the GPU machine. Unknown `{{placeholders}}` are refused |
| `depthmaps.py` | Downloads Depth Anything V2 (`depth-anything/Depth-Anything-V2-Small-hf` by default; Base/Large only with `--allow-noncommercial`) from Hugging Face through `transformers` on first use (`maps.py run` too, and only with a GPU grant file) | No token is needed or sent. `--revision` pins the model commit (recorded in each provenance sidecar), and torch/transformers are not in a requirements file, so `scripts/scan.*` can't audit them; install them from PyPI in a separate environment |

## Threat model in short

Treat HD packs, saves and recipes from strangers like any download of
pictures or documents: they can't run code here, but a decoder bug in a
dependency could crash the game. `--exec` scripts are programs: run only
your own.

## Keeping dependencies current

* C and C++ libraries are built from the pinned release tags in
  `scripts/deps.env` by `scripts/fetch-deps.*`; bump the tags there to pick
  up fixes. The header-only libraries are vendored in `third_party/`.
* `scripts/scan.ps1` (or `scan.sh`), part of local CI on every run, checks
  the pack tool's pinned Python requirements with pip-audit, the Python
  tools with bandit, and the repository for secrets (gitleaks, plus a
  built-in scan for keys, tokens and private machine paths). Each scanner
  that is not installed is listed as skipped; `PLEGACY_REQUIRE_TOOLS=1`
  makes that a failure.
