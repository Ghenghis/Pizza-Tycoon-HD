# HD remaster: how it works and how to make a pack

Pizza Tycoon HD keeps every rule, number and screen layout of Pizza Legacy (and
so of the 1994 original) and adds one thing: any original picture can be
replaced by a high-resolution PNG, and the game is drawn at up to 3840x2400.

```mermaid
flowchart LR
  A[Original game<br/>PT.EXE folder] -->|plegacy --export-assets| B[export/<br/>1,699 PNGs + manifest.json]
  B -->|hdpack.py upscale| C[pack/<br/>baseline, no AI]
  B -->|hdpack.py comfy<br/>ComfyUI on your GPU| C
  C -->|hdpack.py validate| C
  C -->|--hd-pack / hd folder| D[Game drawn at<br/>hd_scale x 640x400]
```

## What there is to replace

Exported from the English DOS release with this engine's own decoders and
palettes:

| Kind | Files | Images | Sizes (original pixels) |
|---|---|---|---|
| VGA pictures (`gfx/`, `z/`) | 145 | 145 | 320x200 screens, 192x176 rooms, 64x46 portraits, icon strips |
| BOB sprites (`bob/`) | 51 | 1,554 frames | 9x11 guests up to ~120x120 characters |
| **Total** | 196 | **1,699** | |

Many BOB frames are animation poses of the same subject (25 character types x
6 frames, 13 walk cycles x 12 frames, 341 furniture pieces, 322 ingredients).
Fonts (`fonts/*.fnt`) are not part of a pack yet: text is drawn from the
original font, scaled up.

## Quick start

```powershell
# 1. Export the original art (no window opens)
.\scripts\export-assets.ps1 -GamePath "C:\GOG Games\Pizza Connection"

# 2a. Baseline pack, no AI, a couple of minutes
.\scripts\make-pack.ps1 -Mode upscale

# 2b. Or with ComfyUI on the RTX 3090 Ti (see below)
.\scripts\make-pack.ps1 -Mode comfy -Checkpoint "juggernautXL_v9.safetensors" -UpscaleModel "4x-UltraSharp.pth" -Limit 10

# 3. Check, then play at 1920x1200
.\scripts\run-dev.ps1 -HdScale 3
```

No engine build on the GPU machine? `tools/hdpack/export.py` writes the same
export with Python only (checked pixel for pixel against the engine):
`python tools/hdpack/export.py "<folder with PT.EXE>" export`.

When another job may be using a second GPU, add `--wait-gpu <index>` to
`hdpack.py comfy`: it waits before each picture until that GPU is below
`--wait-below` percent busy, so two cards never run flat out together.

On Linux/macOS the same steps are:

```sh
plegacy --game-path ~/games/pizza --export-assets export
python3 tools/hdpack/hdpack.py upscale export pack --scale 6
python3 tools/hdpack/hdpack.py validate export pack
plegacy --hd-scale 3 --hd-pack pack
```

## Settings

| Setting (`settings` file) | Command line | Meaning |
|---|---|---|
| `hd_scale = 1..8` | `--hd-scale N` | Texels per pixel of the 640x400 game screen. 1 = original. 3 = 1920x1200, 4 = 2560x1600, 6 = 3840x2400. Read at startup. |
| `hd_pack = <folder>` | `--hd-pack <folder>` | The pack. Empty means a folder called `hd` next to the program, if there is one. |

A pack is used only when `hd_scale` is 2 or more. With `hd_scale` above 1 and no
pack, the game is drawn at the higher resolution with the original pixels
(sharp, no blur from the GPU stretching a small frame).

## Pack layout and rules

The pack mirrors the export, lower case, forward slashes:

```
pack/gfx/back1.png           replaces GFX/BACK1.VGA
pack/gfx/euromap.png         replaces GFX/EUROMAP.VGA
pack/z/zb3.png               replaces Z/ZB3.VGA
pack/bob/moebel/0012.png     replaces frame 12 of BOB/MOEBEL.BOB
```

* **Any size, same shape.** The engine resizes each PNG once, on first use, to
  exactly the original size x 2 x `hd_scale` (area-averaged when shrinking,
  bilinear when growing, in premultiplied alpha so edges stay clean). The
  aspect ratio must be within 2% of the original's or the file is ignored with
  a warning. 12x the original is a 1:1 match for `hd_scale` 6.
* **Missing files keep the original.** Packs can be built a few pictures at a
  time.
* **Transparency.** Index 0 in the original is see-through. In the HD art,
  alpha does the same, and soft alpha edges may reach a little past the
  original outline. Inside the original outline the HD art should be opaque;
  `hdpack.py validate` flags sprites where more than 5% of it is see-through.
* **Colours are absolute.** The original recolours some things with palette
  tricks (screen fades are kept; fades darken HD art too). Palette *cycling*
  and per-scene palette swaps do not change HD art, which shows the colours it
  was painted with.

## How the engine does it

The game logic never sees HD art. Everything is still drawn into 8-bit indexed
640x400 surfaces, so hit-testing, layout and every screen work exactly as
before. Each `gfx::Surface` can carry an extra **HD overlay**: a true-colour
image `hd_scale` times larger, shared copy-on-write.

* Loading a VGA picture or a BOB frame attaches the pack's image, if any.
* Every drawing operation keeps the overlay in step with the indexed pixels:
  * blitting a surface that has HD copies its texels (opaque source pixels
    replace, transparent ones blend soft edges over);
  * drawing *plain* pixels (text, lines, fills, sprites with no HD art) over
    HD art clears those texels, so the original pixels show through. This is
    why every screen keeps working with a partial pack.
* The renderer expands each layer at `hd_scale`: indexed pixels become blocks
  of their palette colour and the HD texels are laid over them. Rows are split
  across CPU threads. The result is uploaded as one texture and the GPU
  scales it to the window (letterboxed, CRT aspect if enabled).

The HD art is drawn by the GPU. Each surface keeps the HD pictures drawn on
it as a list (`src/engine/gfx/HdList.h`) rather than copying texels, and
`HdCompositor` draws that list as textured quads into one texture per layer.
The CPU only expands the 640x400 indexed pixels. Every SDL renderer works
(Direct3D 11/12, Vulkan, Metal, OpenGL); there are no custom shaders.

Measured with `bench 600` in a restaurant, every layer redrawn every frame
(Ryzen 7 5800X3D, RTX 3090 Ti, Direct3D 11, 3840x2400):

| Compositing | avg | p99 | fps |
|---|---|---|---|
| GPU (default) | 3.5 ms | 5.3 ms | 285 |
| CPU (`PLEGACY_CPU_COMPOSE=1`) | 80 ms | 143 ms | 12 |

Both give the same picture (largest difference 2/255 per channel); the tests
check the draw list against the old texel-copy rules and the GPU result
against the CPU result on random drawing.

**DLSS and similar** do not apply: they need a 3D renderer with motion vectors
and depth. This is a 2D game drawn at native resolution, so there is nothing
for them to reconstruct.

## ComfyUI on the RTX 3090 Ti

`tools/hdpack/hdpack.py comfy` sends each exported picture through a ComfyUI
workflow and writes the result into the pack. It talks to ComfyUI's own HTTP
API (`/upload/image`, `/prompt`, `/history`, `/view`) on `127.0.0.1:8188`; no
custom nodes are needed for the shipped workflows.

1. Start ComfyUI (portable build is fine) with the 3090 Ti as its GPU. To keep
   the 3060 for other work, set `CUDA_VISIBLE_DEVICES` to the 3090 Ti's index
   before starting it.
2. Put an upscale model in `ComfyUI/models/upscale_models` and, for the refine
   workflow, an SDXL checkpoint in `ComfyUI/models/checkpoints`.
3. Check everything:
   `python tools/hdpack/hdpack.py doctor --checkpoint <file> --upscale-model <file>`
4. Try ten pictures first: add `--limit 10`, look at them in the game, adjust
   `prompts.json` (prompt and `denoise` per group of files), then run the
   rest. Runs resume: files already in the pack are skipped unless `--force`.

Sprites and other small pictures are best done without AI:
`hdpack.py upscale export/ pack/ --scale 12 --method mmpx --only 'bob/*'`
magnifies them with MMPX, a pixel-art scaler that keeps edges and colours
(about 20 s for all 1,554 frames). Diffusion models need more pixels than a
32x40 sprite has.

Workflows (`tools/hdpack/workflows/`, ComfyUI API format with `{{placeholders}}`):

* `upscale_only_api.json`: two model upscale passes (16x), then a resize to
  the target. Fast and faithful; no new detail is invented.
* `sdxl_refine_api.json` (default): model upscale, then SDXL img2img at a low
  denoise so the layout stays and the detail is redrawn, then a second model
  upscale so the final resize to 4K starts from a sharp picture. Sprites are
  sampled with the long side at 1024 or more and scaled down afterwards,
  because diffusion models make poor pictures far below their training size.
  The original's transparency is put back on every sprite.

Pictures smaller than 4096 original pixels (width x height, so about 64x64)
skip the diffusion model and go through `--small-workflow` (by default
`upscale_only_api.json`). A first test on an RTX 3060 showed why: a 9x11
sprite or a 64x46 sheet of portrait frames carries too little detail, and
SDXL turned faces and figures to mush. Change the limit with `--small-area`;
`--small-area 0` sends everything through the main workflow.

For 4K, use `--scale 12`: the originals are 320x200, the engine's logical
screen is 640x400, and 12x the original is 3840x2400. The default 6x is
1920x1200.

To use your own workflow, export it from ComfyUI with *Save (API Format)* and
put these strings where the values go: `{{input_image}}`, `{{positive}}`,
`{{negative}}`, `{{seed}}`, `{{denoise}}`, `{{checkpoint}}`,
`{{upscale_model}}`, `{{gen_width}}`, `{{gen_height}}`, `{{width}}`,
`{{height}}`, `{{prefix}}`. Unknown placeholders are refused, so a typo can't
reach ComfyUI.

What to expect, by group:

| Group | Expectation |
|---|---|
| Rooms, maps, posters, newspaper pictures | Very good: big, static, low denoise keeps them faithful |
| Portraits, jury, chef | Good; check faces |
| Furniture, ingredients | Good, but run them as one batch with one prompt and seed so they match |
| Animated characters, walk cycles | Hardest: frames are generated one at a time and can drift. Use `upscale_only` or denoise 0.2 or lower, or paint one key frame and redo the rest by hand |
| Logos and anything with text | Keep denoise at 0.1-0.15; models garble letters |

## Redrawing furniture with depth (2.5D-ready art)

MMPX keeps the sprites faithful but they still look like magnified pixel
art. `tools/hdpack/redraw.py` paints them again as real HD art without
moving a single pixel of their outline:

1. `depthmaps.py <pack> <maps>` makes a depth map (Depth Anything V2 Large)
   and a normal map for every picture: about 6 pictures a second, so a few
   minutes for the whole pack.
2. `redraw.py` starts from the pack picture, lets SDXL (`juggernautXL_v9`)
   paint over it at denoise 0.8, and holds the shape and perspective with a
   depth ControlNet (`diffusers_xl_depth_full`, strength 0.85 until 90% of
   the steps). Every rotation of one piece of furniture shares a seed, so the
   four views match. The start picture and the depth map are blurred over
   about half an original pixel first: left sharp, the 1994 pixel grid
   survives the redraw and the ControlNet reads every pixel edge as a step.
3. The result is cut to the original sprite's alpha, so placement, click
   areas and everything the game checks stay the same. The original's
   painted floor shadow is kept, because the model tends to repaint it as
   part of the object.
4. The original's colours are put back over areas of about 1.5 original
   pixels (`--keep-colours`, `--keep-light`): SDXL likes to turn every chair
   into brown leather, and a cream seat should stay cream. The model's own
   picture is kept under `raw/`, so `--from-raw` redoes this step with other
   values in seconds, without ComfyUI.

```
python tools/hdpack/redraw.py pack/ maps/ redraw-out/ --server http://127.0.0.1:8188 \
    --denoise 0.8 --skip-done --items-json tools/hdpack/furniture_redraw.json
```

`furniture_redraw.json` lists 341 restaurant and kitchen furniture frames
(`bob/moebel`, `bob/kuebobs`) with a description each. Plants are left out
(at this size the model turns leaves into blocks, and the MMPX versions look
better), and so are two sprites that are mostly lettering or digits. Running
several `--denoise` values side by side writes `redraw-out/<denoise>/`
folders and a `sheet.png` to compare them. About ten
seconds per frame on an RTX 3090 Ti, so the whole list takes about an hour.
Copy `redraw-out/0.80/bob/` into the pack when you're happy with it (keep the
old folders: they're the pixel-art versions).

## Legal

The engine is GPL-3.0-or-later. The original game's files are not included
and are not redistributable; you need your own copy (GOG and Steam sell it).
Exports and packs are made from that copy, so keep them for your own use
unless you have the rights to share them. `.gitignore` keeps `export/`,
`pack/` and `hd/` out of the repository.
