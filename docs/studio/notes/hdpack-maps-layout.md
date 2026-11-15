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

## What the engine reads (fx::MapStore)

Added 2026-11-14 by the rendering lane. The engine looks for maps in `<pack>/_maps/`, so copy or link the `maps.py` output folder there. The bundle layout is the one above:

```
<pack>/_maps/<asset id>/normal.png     as above; the engine decodes 128 as 0, so (128,128,255) is exactly flat.
                                       Texels with alpha < 128 count as flat.
<pack>/_maps/<asset id>/emissive.png   NEW, optional: RGBA 8-bit, straight alpha. RGB is the light the picture
                                       gives off by itself (lit windows, neon, oven mouths, screens); alpha is how
                                       much. Transparent means no glow. It is added over the lit frame at night,
                                       scaled by the day/night night factor ("Day and night" option).
```

* A map may be any size with the picture's aspect ratio (within `HdPack::aspect_tolerance`). It is resized to the picture.
* `depth_16.png` is not read yet.
* With no `_maps` folder, or no map for a picture, the picture is drawn exactly as before.
* `maps.py` does not make emissive masks. They are hand-painted or made by a future stage, and need a provenance sidecar like every other output.

## Proposed stage: emissive masks (`emissive.py`), spec for the curator

Added 2026-11-14 by the rendering lane, for the curator to build.

**Implemented 2026-11-14 as `tools/hdpack/emissive.py`** (`plan`, `run`, plus `sheet` for a review contact sheet; tests in `tests/test_emissive.py`). It differs from the text below in four places:

* **Sidecar location.** The sidecar is `<maps>/_provenance/<asset id>/emissive/provenance.json`, so it sits beside the depth+normal sidecar of the same picture instead of replacing it.
  * `input` is the HD picture.
  * The new `inputs` list holds both the export picture and the HD picture, with their roles.
* **Scoped keys.** The key file may also hold `scopes`, entries of the form `{"palette": "4", "only": ["bob/moebel/010[1-4]"], "lamp": [...]}`. Their keys apply only to the matching asset ids. `assets` entries may be globs.
  * Scopes were needed because, in the shared restaurant palette, every proposed warm colour is also wood, brass, floor or brick somewhere.
* **Spill colour.** In the dilated spill, the RGB is taken from the nearest kept texel instead of the wall under it.
* **Resume rule.** A sidecar is also stale when the input hashes or the parameters (including `keys_sha256`) changed.

**Goal.** Write `<maps>/<asset id>/emissive.png` (format above) for the pictures that have lit windows, lamps, signs, screens or flames. It must be cheap, deterministic, CPU only (Pillow and numpy), and need no model or GPU. That way it can run in CI and on any machine, with no broker grant.

### Inputs (per asset)

* the original picture from the asset export (`<export>/<asset id>.png`). The export writes RGBA decoded through the picture's palette, with index 0 as alpha 0. `manifest.json` names that palette. Because each texel is a palette colour exactly, its RGB identifies its palette entry. Two entries with the same colour count as one key.
* the HD pack picture (`<pack>/<asset id>.png`), the same picture at k times the size;
* a key file, `tools/hdpack/emissive_keys.json`, the only hand-made input:

```json
{
  "schema": "plegacy.emissive-keys/1",
  "palettes": {
    "<palette id>": {
      "window": [ "#rrggbb", ... ],
      "lamp":   [ "#rrggbb", ... ],
      "sign":   [ "#rrggbb", ... ],
      "flame":  [ "#rrggbb", ... ]
    }
  },
  "assets": { "<asset id>": { "skip": true } }
}
```

### Step 1: propose the keys (`emissive.py plan <export> --out keys.proposed.json`)

* Count every distinct colour (palette entry) over all exported pictures that use the palette, as `manifest.json` lists them.
* Propose a colour as a light colour when its RGB, in HSV, falls in one of the classes below:

| Class | Hue | Saturation | Value | Covers |
|---|---|---|---|---|
| window | 38–62° | ≥ 0.45 | ≥ 0.70 | yellow window panes |
| lamp | any | ≤ 0.20 | ≥ 0.92 | lamp whites |
| flame | 10–38° | ≥ 0.55 | ≥ 0.75 | oven mouths and fire |
| sign | any | ≥ 0.60 | ≥ 0.85 | neon and screens |

* Exclude a colour when it covers more than 20% of the texels of the pictures that use it, since that is a wall or sky, not a light. The 20% is a parameter.
* For each palette, write the proposal with a swatch, a count and three example assets.
* The curator reviews the proposal and copies the colours to keep into `emissive_keys.json`. Only that file decides; the proposal is never used directly.

### Step 2: make the masks (`emissive.py run <export> <pack> <maps>`)

1. **Key mask:** the original picture's texels whose RGB is exactly a key colour of its palette (alpha 255), with the class kept. Upscale it to the HD size with **nearest** sampling: k × k texels per original pixel, from `--k` or from the size ratio.
2. **Follow the HD art:** keep a key texel only where the HD texel is bright enough to be the light. The test is luma ≥ 0.55 × the key colour's luma and alpha ≥ 128. A redraw that moved a window by a texel or two then still lines up.
3. **Dilate:** a square structuring element of radius `r = max(1, round(0.5 × k))` texels, so the glow spills just past the pane edge. Then a 3×3 box blur feather. Both are deterministic integer operations.
4. **Colour and strength:**
   * The RGB is the HD texel colour, scaled so its largest channel is 1. The glow keeps the art's colour but is always bright.
   * The alpha is mask × class strength: window 0.85, lamp 1.0, sign 0.9, flame 0.8.
   * It is written straight (not premultiplied), 8-bit.
5. **Skip:** pictures with no key texel get no file, so the engine draws them unchanged. So do pictures listed with `"skip": true`.

Parameters (`--luma-ratio`, `--radius`, class strengths, the 20% limit) go into `parameters` of the sidecar.

### Provenance and determinism

* **Sidecar:** `stage` is `emissive`. `input` lists the export picture and the HD picture with their SHA-256. The key file's SHA-256 goes under `parameters.keys_sha256`. `workflow` and `models` are null, and `seed` is null with `seed_scheme` `none`.
* **Resume rule:** the same as the other stages. A stale sidecar means the picture is redone.
* **Determinism:** the same inputs and key file give byte-identical PNGs. Write them with Pillow `optimize=False` and a fixed compression level.
* **Validation:** `hdpack.py validate --provenance` covers the new files like every other output.

### Tests for the stage

* A synthetic 8×8 sprite with two key colours, plus its 4× HD copy:
  * the mask covers exactly the dilated key area;
  * a non-key bright texel stays dark;
  * a key texel whose HD texel is dark (the redraw moved it) stays dark.
* Two runs give identical bytes.
* A picture with no keys writes no file.
* The plan step proposes a yellow pane colour and not a yellow wall that covers 60% of the picture.

### Engine side (already in)

* `fx::MapStore` reads `emissive.png` from `<pack>/_maps/<asset id>/` and resizes it to the picture if needed.
* `HdCompositor::draw_emissive` adds the glow only where the art is visible, with later pictures covering it.
* The Renderer adds the glow over the lit frame, scaled by the night factor, when "Day and night" is on in a preset other than Classic.
* With no `_maps` folder, nothing changes.

### Emissive at run time (2026-11-14, rendering lane)

* **Formats.** RGBA straight alpha (alpha = strength) and the plan's RGB8 (glow × mask, black elsewhere) both work. An RGB8 map loads opaque, and black adds nothing.
* **Location pictures.** A location picture from `views/hd/LocationArt` (`<pack>/locations/<ns>/gfx/k3.png`) has its own asset id, `locations/<ns>/gfx/k3`. Its maps go in `<pack>/_maps/locations/<ns>/gfx/k3/`.
* **Strength.** The night factor is the game clock's night (0 by day, ramping at dawn and dusk) times the preset's glow: Natural 0.85, Vivid and Cinematic 1.0, Studio 0.7, Classic 0. It applies on the city map, in the restaurant and in the kitchen. It does not need the "Day and night" option.
* **Feather.** From hd_scale 6 up, maps are feathered on load: two box blurs of k/2 texels in premultiplied space. This hides the original's pixel grid, the blocky edges seen at 12×. `emissive.py` may still feather at pack scale. The runtime feather comes on top.
