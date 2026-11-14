# HD pack: seeds, provenance, and the depth/normals stage

2026-11-14. This covers `tools/hdpack` (`hdpack.py`, `provenance.py`, `depthmaps.py` and `maps.py`). It follows the asset pipeline section of `docs/research/2026-remaster-wow-report.md`.

## Seeds

Each asset's ComfyUI seed is:

```
int(sha256(f"{global_seed}:{workflow_id}:{group}").hexdigest()[:8], 16) & 0x7fffffff
```

* `global_seed` is `hdpack.py comfy --seed`, default 1994.
* `workflow_id` is the SHA-256 of the workflow's canonical JSON, so key order and whitespace do not matter. Small pictures go through the upscale-only workflow, so they get that workflow's id.
* `group` is the asset key (`gfx/back1`, `bob/moebel`). Every frame of one sprite therefore shares a seed, so its views match, which is the same policy as `redraw.py`.

The seed never depends on where the asset sits in the queue, so `--only`, `--limit` and resuming all give an asset the same seed (`tests/test_provenance.py`). Changing the workflow or `--seed` changes every seed, as it should.

## Provenance sidecars

Every picture the tool writes, from `upscale`, `comfy`, `depthmaps.py` or `maps.py`, gets a sidecar:

```
<root>/_provenance/<asset id>/provenance.json      asset id = the path without .png, e.g. gfx/back1
<root>/_provenance/_workflows/<workflow sha256>.json   one copy of each workflow used
```

The picture is written first, then its sidecar. Each is written whole: a temporary file in the same folder is flushed, fsynced and renamed over the target.

| Field | Meaning |
|---|---|
| `schema` | `plegacy.hd-provenance/1` |
| `stage` | `comfy`, `upscale` or `depth+normal` |
| `asset` | file, key, kind, frame, id |
| `input` | the file it was made from and its SHA-256 |
| `outputs` | every file written (relative to the root) and its SHA-256 |
| `workflow` | `id` (SHA-256) and file name; null for `upscale` and the transformers route |
| `models` | role, name, SHA-256, licence, revision. ComfyUI cannot report hashes, so `comfy --models index.json` supplies them (`{"<file>": {"sha256", "license"}}`); unknown ones are recorded as null/`unknown` and the check warns |
| `seed`, `seed_scheme`, `seed_inputs` | the seed and the three inputs it was hashed from |
| `parameters` | prompts, denoise, sizes, scale, feather, server; for maps: route, grant id, GPU, depth bits, smoothing |
| `tool` | `plegacy-hdpack`, version and Python version |
| `created_utc` | ISO time |
| `review` | `pending` (later `approved` or `rejected`) |

Resuming uses the sidecars:

| Picture in the pack | What a resume does |
|---|---|
| Its sidecar names it as it is now | Done |
| Its sidecar names another version (stale) | Redone |
| It has no sidecar (an older pack) | Kept, never silently regenerated; counted and reported |

The pack-level check is `hdpack.py provenance <folder>`, or `hdpack.py validate <export> <pack> --provenance`. It finds:

* every PNG outside `_provenance/` that has no sidecar naming it with its current SHA-256;
* every output a sidecar names that is missing;
* every unreadable or unknown-schema sidecar.

It warns about models with no hash or licence.

## Depth model licence

Depth Anything V2 **Small** is Apache-2.0; Base and Large are **CC-BY-NC-4.0**.

* `depthmaps.py` and `maps.py` default to Small (`depth-anything/Depth-Anything-V2-Small-hf`, or `depth_anything_v2_vits.pth` in ComfyUI).
* They refuse Base and Large unless `--allow-noncommercial` is given.
* Every sidecar records the model and its licence.
* Maps made earlier with Large have no sidecar, so the check reports them. They, and redraws that used them as ControlNet input, should be regenerated with Small before any commercial release.

## The depth/normals stage (`maps.py`)

**Layout:** one bundle per pack picture, named by its asset id. This is the flat-pack form of the report's `pack-src/<asset_id>/`.

```
<maps>/<asset id>/depth_16.png   16-bit greyscale, near = bright, visible pixels spread over 0..65535
                                 (1st..99th percentile), transparent pixels 0
<maps>/<asset id>/normal.png     RGB 8-bit tangent space: R = +X (right), G = +Y (up), B = +Z (out of the screen);
                                 (128,128,255) where the picture is transparent
<maps>/_provenance/<asset id>/provenance.json
```

**Depth:** the HD picture on neutral grey, with its long side at most `--max-side` (1024), through the model. The result is resized back to the picture's size and normalised over its visible pixels.

**Normals:** derived from the depth after an edge-keeping (bilateral) smoothing. The defaults are radius 3, σs 2 and σr 0.08, so the large planes show and not the model's fine noise (the report's Siege and the Sandfox note). Strength is 6.

**Routes:**

| Route | How | Depth |
|---|---|---|
| `transformers` (default) | Hugging Face pipeline on the granted GPU (`cuda:<gpu>`); `--revision` pins the model commit and is recorded | 16 bits |
| `comfy` | `workflows/depth_anything_v2_api.json`: LoadImage → `DepthAnythingV2Preprocessor` (comfyui_controlnet_aux; `ckpt_name` `{{depth_model}}`, `resolution` `{{resolution}}`) → SaveImage | 8 bits, because SaveImage writes 8-bit PNGs (recorded as `depth_bits: 8`) |

For the `comfy` route, check the server first with `hdpack.py doctor --workflow tools/hdpack/workflows/depth_anything_v2_api.json --depth-model depth_anything_v2_vits.pth`. Doctor now fails a node that ComfyUI answers with `{}` for.

**GPU gate:**

* `maps.py plan` needs no GPU.
* `maps.py run` refuses before it loads a model or contacts ComfyUI, unless `--grant-file` (or `PLEGACY_GPU_GRANT_FILE`) points to a broker grant:

```json
{"grant_id": "...", "scope": "hdpack-maps", "gpu": 0, "not_after_utc": "2026-11-15T03:00:00+00:00"}
```

It also refuses if the grant has no time zone, has a different scope or has ended. The grant id and GPU go into every sidecar. No grant has been issued, so nothing has run. The tests drive the stage with a stand-in model.

**Running it on the 3090 Ti, once granted:**

```
python tools/hdpack/maps.py plan  <pack> <maps>
python tools/hdpack/maps.py run   <pack> <maps> --grant-file <grant.json> --revision <pinned commit>
python tools/hdpack/hdpack.py provenance <maps>
```

The engine does not read these maps yet. Normal-mapped lighting is the report's SDL_GPU item.
