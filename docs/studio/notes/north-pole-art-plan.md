# North Pole flagship: art production plan for the 3090 Ti

2026-11-14. This plan runs only once the GPU broker grants time. Nothing in it has been generated, and no GPU was used to write it.

Sources:

* `NORTH-POLE-FLAGSHIP.md`, `HOLIDAY-CAST-ASSETS.md` and `ASSET-REUSE-POLICY.md` (codex-completion-20261114);
* the location definition `data/locations/north-pole/santas-pizza-village.json` (rev 2);
* the owner's `world-region-art/ASSET-INDEX.json`, read only;
* the pipeline in `tools/hdpack`: seeds, provenance and `maps.py` (`docs/studio/notes/hdpack-maps-layout.md`).

## 0. What exists and what it is not

| Item | Status | Use |
|---|---|---|
| `north-pole-santas-pizza-village` backdrop | 1536×960 juggernautXL_v9 master, seed 2026122500, 15.08 s on the 3090; plus a 4K resize | Mood and colour reference only. It is not a tile set |
| 20 holiday cast concepts (Santa, Mrs. Claus, Rudolph, Frosty, elf chef and courier, gingerbread baker, nutcracker host, four outfits, eight reindeer) | 1024×1536, opaque studio background, generated, unreviewed; several have defects listed in the index | Costume and colour reference after review. They are not sprites |
| `north-pole-runtime-candidates/` (spruce v1/v2, holiday jukebox 0317, kenney-holiday-cc0-v1) | Spruce v2 is `UNACCEPTED_CORRECTION_FAILED`. The jukebox has a byte-exact alpha derivation | The jukebox is the pattern for props: the original's alpha is kept exactly |
| Location rev 2 | 6 districts, 160×120 tiles, 44 premises + 1 depot, 7 premise sizes (12×12 … 18×17), 8 named buildings. It uses **35 distinct tile ids** of the classic `landsym` atlas and `med1` (`assets.tile_atlas = classic:gfx/landsym`, `kit: null`) | Today the map draws with the 1994 atlas; nothing North-Pole-specific is drawn yet |

The flagship spec names **six** districts: Village Square, Toy Workshop Quarter, Reindeer Ridge, Frosty's Snow Gardens, Candy Cane Market and Aurora Harbor. A seventh, the **Arrival / Sleigh Station** (the depot and the edge of the world map), is proposed here as an optional small district. It needs a location revision before any art is made for it.

## 1. Engine facts the art must fit

* **City map, close zoom:** `landsym` has 256 tile ids at 16×16 original pixels each (32 engine px; 192 texels per tile at 4K, k=6). It is top-down and oblique, lit from the top left.
  * Each tile id has a fixed category: nature, water, building, path, road, villa, residential, special or other (`City::tile_category`).
  * Art can change what a tile id *looks like*, never what it *is*.
* **City map, medium zoom:** `med1` is derived from HD landsym (M6, done), so it needs no separate art.
* **Restaurant:**
  * floors `gfx/k0..k13`;
  * back walls and interiors `gfx/back*`;
  * furniture, doors and the pizza `bob/moebel` (4 rotations per piece, isometric);
  * seated guests and staff `bob/gaeste`;
  * walking guests `bob/walk{N}`;
  * night sprites `bob/night*`;
  * guest faces `gfx/g0..g23` (64×46).
* **Pack rules:**
  * same path as the export;
  * aspect ratio within 2 %;
  * the original's alpha outline is kept;
  * 4K = 12× the original pixels;
  * missing pictures fall back to the original.

  This is why regional variety must come from art plus tile-id choices, not from tint.
* **District identity on one atlas:** the location has one tile atlas. Two routes:
  1. allocate distinct building, roof and ground tile ids per district within each category;
  2. a per-district atlas key, which is a runtime-lane feature not built today.

  Route 1 needs a location revision (the content lane), plus an audit of the 256 ids per category. **Decision needed from the integrator.**

## 2. Asset list per district

Counts are targets for a rich map, not what rev 2 uses today. The Arrival station is only made if approved.

| District | Ground and terrain | Buildings, roofs, windows | Props | Cast and costumes (HOLIDAY-CAST-ASSETS) |
|---|---|---|---|---|
| **Village Square** (business, warm gold) | snow cobbles, plaza snow, snowbank edges ×4, lamp-lit road ×6 (straight, corners, T, cross) | pizzeria facades, half-timber shops ×6, chimneys with smoke and lit windows; the named buildings: St. Nicholas church, North Star bank, Hotel Polaris | big tree, lamp posts, benches, wreaths, market stall | human holiday chef, server and delivery outfits; caroler host; Santa and Mrs. Claus as hosts |
| **Toy Workshop Quarter** (industrial, timber and copper) | packed snow and sawdust, cart tracks, loading yards | workshop halls ×5 (saw-tooth roofs, copper vents), Toy Emporium | crates, toy piles, workbench yards, gears | elf chef, elf courier (workwear and tool accents), gingerbread baker |
| **Reindeer Ridge** (leisure, lodge greens) | snow meadow, pine edge, fenced paddock, ridge slope ×4 | lodges ×4 (green roofs, antler gables), stables, Cocoa Corner | feeders, sleigh, hay, pines | nine reindeer (Rudolph, Dasher, Dancer, Prancer, Vixen, Comet, Cupid, Donner, Blitzen) as decorative and customer roles; lodge outdoor uniforms |
| **Frosty's Snow Gardens** (residential, icy blues) | snow garden beds, frozen pond (water tiles), ice paths | cottages ×6 (blue roofs, frosted windows), villas ×2, Elf Academy | snowmen, ice sculptures, frosted hedges | Frosty (controlled silhouette, see §4); blue and white winter outfits |
| **Candy Cane Market** (shopping, red and white) | striped pavement, market square, snow piles | market halls ×4, candy shops ×5 (striped awnings), Snowglobe disco | candy-cane poles, stalls, garlands | striped chef and server variants; nutcracker host |
| **Aurora Harbor** (traffic, indigo) | quay, ice floes, harbour water edges ×8, snow docks | warehouses ×3, harbour office, Aurora Cinema, lighthouse | boats in ice, cranes, lanterns | insulated delivery and host outfits |
| *(optional)* **Arrival / Sleigh Station** | runway snow, station plaza | depot, station hall | sleigh, parcel stacks | courier elves |

**Restaurant interiors:** 6 district themes × (1 floor set from `gfx/k*` + back wall from `gfx/back*`), on the existing premise sizes. Furniture accents come from `bob/moebel`. Each piece keeps its 4 rotations and the original alpha (the jukebox 0317 method): tree, wreath tables, festive chairs, ovens with glow and a jukebox.

**Characters:** the walking, seated and staff frames already exist (`walk*`, `gaeste`, `night*`). Costumes are **overlays and repaints of those frames at their native size**, which keeps the silhouette, anchor, frame count and timing. They are then magnified with MMPX for HD. Diffusion is not used on 16–32 px characters: it breaks anatomy. Portraits (`gfx/g*`, 64×46 → 768×552 at 4K) are the place for detailed painted cast faces, made from the reviewed concepts.

## 3. Reuse before generation

Per ASSET-REUSE-POLICY, every candidate is recorded with source, licence, hash, size, alpha and anchor, and gets one disposition: *admitted*, *adapt*, *reference only* or *rejected*.

| Source | Fit | Planned use |
|---|---|---|
| Kenney Isometric Vector Buildings / Modular Buildings (CC0; 224×206 images, 180×115 tiles) | Isometric. **The city map is top-down**, so this is **reference only** for city tiles | Roof and awning shapes as drawing reference; vector awnings re-projected for facades |
| Kenney Holiday Pack 2016 (CC0; RTS, voxel and isometric trees, presents, snow pieces) | The isometric pieces fit the **restaurant interior** projection | Admit-after-adapt for interior props (tree, presents), redrawn to the moebel palette and rotations |
| PC3 / Barro's (owner-approved, private) | 3D game textures and UI; different projection | Material reference (wood, brick, snow albedo) and light-colour reference; not admitted directly |
| Tapper (owner-approved) | Bar props in another style | Reference for bar and cocoa props only |
| Existing generated concepts | §0 | Colour, costume and silhouette reference after a human pass; never shipped as they are |

## 4. Per-asset outputs (one bundle per pack picture)

Everything goes through the hdpack seed and provenance system: `seed = sha256(global:workflow:group)`, every file with a `_provenance/<asset id>/provenance.json` holding hashes, models, licences, seed, prompts, tool, grant id and review flag.

| Output | Where | How |
|---|---|---|
| Albedo (straight alpha) | `pack/<file>.png` (what the engine loads) and a 16-bit master in `pack-src/<asset id>/master_16bit.png` | §5 workflows, cut to the original alpha |
| Depth, 16-bit | `maps/<asset id>/depth_16.png` | `maps.py run` (DA-V2 Small) on the **HD albedo** |
| Normal, tangent space (+X right, +Y up, +Z out) | `maps/<asset id>/normal.png` | from depth, bilateral-smoothed, strength clamped; characters masked to flat at first |
| **Emissive** (window, oven, lamp, lantern and aurora glow) | `maps/<asset id>/emissive.png`: RGB glow colour × mask, black elsewhere | Mask from SAM 2 (Apache-2.0) clicks on windows, lamps and ovens, plus a rule pass (warm lit pixels inside the mask). Colour picked per district (§6). Hand-touched for the hero buildings |
| Material IDs (optional) | `maps/<asset id>/mask_ids.png` | wood, snow, glass, metal, cloth, candy: SAM 2 plus labels; for later specular and footstep sounds |

`maps.py` gets an `emissive` and a `mask_ids` stage, following the depth stage's pattern (same layout, gate and provenance). That is a small code task, to do before the first run.

## 5. ComfyUI workflows and licences

Apache, MIT and BSD models are preferred. Every model goes into the `--models` index with its SHA-256 and licence, so every sidecar carries them.

| Step | Model (licence) | Notes |
|---|---|---|
| Base redraw, tiles and facades | SDXL 1.0 base or Juggernaut XL v9, the model the existing concepts used (**CreativeML OpenRAIL++-M**: commercial use allowed with use restrictions; **confirm Juggernaut's own terms** before release, as the index already notes) | img2img from an MMPX-magnified original at denoise 0.35–0.55 for tiles and 0.6–0.8 for facades, as `redraw.py` does |
| Structure lock | SDXL depth ControlNet (`diffusers_xl_depth_full`, OpenRAIL++), depth from **DA-V2 Small (Apache-2.0)** | Holds the 1994 shapes and perspective; strength 0.85 until 90 % of the steps (redraw.py) |
| Style lock | IP-Adapter SDXL (Apache-2.0 code; check the weights card) on 2–4 approved reference tiles per district | The district look without prompt drift |
| Alternative base | **FLUX.1-schnell (Apache-2.0)**, fp8 | Allowed. **FLUX.1-dev is non-commercial: do not use it** |
| Edits and variants (winter, night) | **Qwen-Image-Edit (Apache-2.0)**, fp8, about 20 GB | Colour-locked variants with the same seed per group; tight on 24 GB |
| Upscale | **RealESRGAN x4plus (BSD-3-Clause)**, or the current model **after a licence check**. **4x-UltraSharp is CC-BY-NC-SA: flagged, do not ship it** | Only after the diffusion step; never a resize claimed as 4K detail |
| Depth | **DA-V2 Small (Apache-2.0)**. Base and Large are CC-BY-NC-4.0 and refused by the tool | `maps.py` |
| Masks | **SAM 2 (Apache-2.0)** | emissive and material masks |
| Pixel characters | MMPX (in the repo) | no model |

## 6. Style locks

* **Palette:**
  * each district has a 16–24 colour key palette sampled from the approved pilot;
  * after generation, the `--keep-colours` / `--keep-light` pass from redraw.py puts the original's colours back over areas of about 1.5 original pixels;
  * a ΔE check against the palette flags drift.

  District glow colours: Village warm 2700 K amber, Workshop copper 3000 K, Ridge lantern 3200 K, Gardens cool 5500 K with blue fill, Market red and white neon-ish (restrained), Harbor indigo plus aurora green and violet.
* **Viewpoint and light:**
  * city tiles top-down oblique, light from the top left (the same as the far-map bevel and the classic tiles);
  * interiors isometric, matching the moebel rotations;
  * a light-direction checker on the depth and normal maps rejects tiles lit from another side.
* **Scale:**
  * generation at 1024–2048 px on the long side (`generation_size`), then fitted to 12× the original;
  * one roof pitch and one window size per district;
  * the tile grid is kept: every tile edge must match its neighbours, checked by `edge_match` on a 3×3 contact sheet.
* **Outline:**
  * the original alpha outline is kept (`apply_reference_alpha`);
  * the 1994 dark rim is kept only where the original has it;
  * no new black outlines.
* **Tiles:** every tile is generated as a **3×3 neighbourhood crop** (so seams are painted, not stitched), then the centre tile is cut out. Seam ΔE at the tile edge must be at most 3.

## 7. Pilot first, then review gates

| Gate | Content | Pass criteria (human review plus checks) |
|---|---|---|
| **G0 style board** | 1 district (Village Square): 12 tile ids, 1 pizzeria facade, 3 props, 1 interior floor and wall, 1 costume overlay (holiday server on `walk1`), 1 portrait | Owner and art review **at real play scale in the game** (hd_scale 6, close and medium zoom, day and night), against the Classic look; palette ΔE, seams, light direction, alpha halos on dark and light |
| **G1 district** | all Village Square assets plus maps | `hdpack.py validate --provenance`; `hdpack.py provenance maps/`; hole fraction ≤ 5 %; 3×3 seam sheet; normal and emissive preview in the lighting test scene |
| **G2 remaining districts** | 1 district per batch | as G1, plus a cross-district contact sheet for coherence |
| **G3 cast** | costume overlays (all walking, seated and staff frames) and 20 portraits | frame count and anchors identical to the originals; every frame checked; rejected concepts (extra-arm Frosty, gingerbread) stay rejected |
| **G4 verifier** | the whole destination | flagship acceptance (dense streets, interiors, night, reduced motion); every failed or skipped item listed |

Rejected outputs keep their provenance with `review: "rejected"`, so they are never regenerated by accident. There are no blind rerolls: a third try on the same asset needs a changed input (mask, reference or ControlNet strength) and a note.

## 8. Batches, VRAM and time (3090 Ti, 24 GB)

These are estimates, not measured on this setup except where marked. The measured anchor is the existing village concept: a 1536×960 SDXL generation took **15.08 s**, and the GPU memory record shows about 9.1 GB in use during that session (`gpu-cleanup-20261114.json`).

| Batch | Items | Per item (estimate) | VRAM (estimate) | Batch time (estimate) |
|---|---|---|---|---|
| G0 pilot | about 20 pictures (with 2–3 candidates each, about 60 generations) | 10–14 s SDXL + ControlNet at 1024², +3 s upscale | 11–14 GB | about 15–20 min of GPU, plus review |
| One district's tiles | about 40–60 tile ids × 3 candidates, as 3×3 crops at 1536² | 15–20 s | 12–15 GB | 40–60 min |
| Facades and named buildings (per district) | about 10 × 3 | 20–30 s (1536–2048 px) | 14–18 GB | 15 min |
| Interior floor and wall (per district) | 2–3 × 3 | 20 s | 14 GB | 3 min |
| Props (moebel, 4 rotations, shared seed) | about 15 × 4 × 2 | 12 s | 12 GB | 25 min |
| Portraits | 20 × 4 | 12 s | 12 GB | 16 min |
| Qwen-Image-Edit night and winter variants | as needed | 30–60 s | about 20–22 GB (fp8) | separate grant; nothing else on the card |
| Maps (depth, normals) | about 600 pictures | under 0.2 s GPU + CPU normals | under 2 GB | under 5 min |
| SAM 2 emissive and material masks | about 200 buildings and props | 1–2 s + clicks | 4–6 GB | interactive |

Total for six districts is roughly 8–12 GPU hours, spread over several grants (one district per grant). The broker grant lists the batch and its expected time.

**RX 7800 XT (16 GB, separate jobs, never pooled with the 3090):** it is suited to work that doesn't need CUDA, once its software path is tested:

* RealESRGAN through `realesrgan-ncnn-vulkan` (Vulkan, BSD);
* DA-V2 Small depth (ROCm on Linux or DirectML on Windows, to be tested);
* SAM 2 mask passes;
* contact sheets, seam and ΔE checks.

SDXL production on it is not planned until a ROCm ComfyUI run passes the pilot gate.

## 9. How the runtime lighting lane consumes the maps

* **Albedo** stays the pack picture the engine already loads.
* **Emissive** is usable **today**, without shaders. It is a second picture drawn **additively** (`SDL_BLENDMODE_ADD`) over its sprite or tile, at a night factor from game time (0 by day, ramping at dusk). The same command batching applies, and a missing emissive draws nothing. This gives the night windows, ovens and lanterns.
* **Normals and depth** need the SDL_GPU path (the report's item 4):
  * one fragment shader: albedo × (ambient + Σ N·L × light colour) + emissive;
  * lights are the sun and moon direction plus a small set of point lights (street lamps, ovens), placed from the emissive masks' centroids;
  * depth is used for soft contact shadows and for sorting lights in front of or behind props.
* **Format:**
  * normals RGB8, tangent space, +Y up, (128,128,255) where transparent;
  * depth 16-bit near-bright;
  * emissive RGB8.

  Albedo, normal and emissive share **the same atlas UV layout**, so one lookup serves all three. The engine uses whichever maps exist and falls back silently.
* **Budget:** maps are optional per picture. The 4K VRAM budget is taken in the platform lane's pack loader; BC5 normals and BC7 albedo come later on SDL_GPU.

## 10. Before the first grant

1. Integrator: decide the district-identity route (§1).
2. Content lane: location revision with the per-district tile ids (and the Arrival station if approved).
3. Curation (this lane):
   * the reuse dispositions for Kenney Holiday 2016 and the jukebox method for props;
   * the `--models` index with SHA-256 and licences for every model in §5;
   * the `emissive` and `mask_ids` stages in `maps.py`;
   * the G0 batch list with asset ids and group seeds.
4. Owner: review and disposition of the 20 cast concepts and the backdrop, so they can serve as references.
