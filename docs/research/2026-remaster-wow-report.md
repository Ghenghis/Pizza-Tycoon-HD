# Pizza Tycoon HD: "I can't believe it's back" research report

*Date: 2026-11-14. Read-only study of the repository plus web research.*
*Labels: **[V]** = verified from a cited source or the code. **[I]** = my inference or recommendation.*

## What the code already has (grounding) [V]

- **Simulation loop.** `src/engine/Game.cpp` already runs a fixed-timestep simulation with an accumulator and a catch-up cap, and publishes `context.clock.tick_alpha`.
  - Only `GuestWalkScene.cpp` reads `tick_alpha`. Everything else still moves in whole ticks.
- **Renderer.** It is created with `SDL_CreateWindowAndRenderer`, so the platform default is used (D3D11 on Windows). SDL 3.4.0 or newer is required (`CMakeLists.txt`).
- **Lighting.** `LightPass` is fixed-function only: a light map multiplied into the layer, plus additive halos. It follows the game clock (day, evening, night) and is off in the 1994 look.
- **Comparison.** F8 split compare exists (`renderer.compare`).
- **Art pipeline.** `tools/hdpack` has MMPX for sprites, ComfyUI upscale and SDXL-refine workflows, and `depthmaps.py`, which makes depth and normal maps for every picture with **Depth Anything V2 Large**. `redraw.py` uses a depth ControlNet.
- **Seeds depend on order.** `hdpack.py comfy` draws each asset's seed from `random.Random(settings.seed)` in queue order. A run with `--limit`, a pattern or a resume therefore gives different seeds to the same asset. There is no per-asset provenance sidecar.
- **Audio.** A custom SDL_AudioStream callback mixer (atomics, no allocation) with the FM music player and an effects-pack override.

---

## Executive summary: top 10 "wow" upgrades (impact ÷ effort)

| # | Upgrade | Path | Effort | Classic risk |
|---|---|---|---|---|
| 1 | **Interpolate every moving thing with `tick_alpha`**: cars, pedestrians, guests, camera, UI counters. The "buttery" fix. | Now | S–M | None; render-only |
| 2 | **Layered restaurant ambience**: room tone, pooled one-shots, screen-position pan, music ducking | Now | M | None if off in Classic audio |
| 3 | **Smooth eased zoom and camera inertia** on the city map, scale steps cross-faded | Now | S–M | None |
| 4 | **Normal-mapped lighting** for furniture and room walls, using the existing depth/normal maps, day/night driven | Next (SDL_GPU) | M | None; HD layer only |
| 5 | **Particles**: oven steam, kitchen smoke, candle sparks, North Pole snow, sparkle on cash/awards | Now | M | None |
| 6 | **UI juice**: eased popups, hover lift, number roll-ups, button press squash | Now | S | Low; keep timings off the sim |
| 7 | **Photo mode plus a before/after slider and "day timelapse"** | Now/Next | M | None |
| 8 | **Bloom, colour grade (LUT), optional tilt-shift and vignette** on city and restaurant views | Next | M | None |
| 9 | **Offline in-betweens** (RIFE) for walk cycles, shown only when the HD toggle is on | Later | L | Medium; must not change timing |
| 10 | **HDR output** (scRGB) so candles, ovens and neon really glow | Next | S–M | None |

---

## 1. Visual

**What acclaimed remasters did [V]**
- *Diablo II Resurrected* runs the legacy game "literally running underneath the remaster". It keeps 25 fps logic and breakpoints, adds 60 fps visuals, and has an any-time legacy toggle ([eip.gg](https://staging2.eip.gg/news/how-diablo-ii-resurrected-will-run-two-games-at-once), [TweakTown](https://www.tweaktown.com/news/77961/diablo-2-remaster-supports-breakpoint-builds-even-with-uncapped-fps/index.html)).
- *C&C Remastered* extended its real-time classic/HD toggle to all single-player modes after playtesting ([Game Developer](https://www.gamedeveloper.com/design/ea-s-strategy-for-remastering-i-command-conquer-i-)).
- *AoE II DE* rebuilt all assets in 4K while "we stayed faithful" ([Gamereactor](https://msn.gamereactor.eu/we-stayed-faithful-with-age-of-empires-ii-definitive-edition/)).
- *HD-2D* is defined by dynamic lighting, depth of field, tilt-shift, bloom, volumetric fog, particles and parallax, with point lights making sprites cast shadows ([Wikipedia](https://en.wikipedia.org/wiki/HD-2D), [Unreal spotlight](https://www.unrealengine.com/en-US/spotlights/octopath-traveler-s-hd-2d-art-style-and-story-make-for-a-jrpg-dream-come-true)).
- The cautionary tales are about motion, not resolution:
  - *Monkey Island SE*: voices had to match the original text timing, which reviewers found stiff ([bit-tech](https://www.bit-tech.net/reviews/gaming/pc/the-secret-of-monkey-island-special-edn/2/)).
  - *Commandos 2 HD*: "sharper while maintaining the charm", but movement felt less smooth than the original ([Use A Potion](https://useapotion.com/2020/12/commandos-2-hd-remaster-review)).
- **[I]** Lesson: Pizza Tycoon HD's architecture (original sim, HD presentation layer, toggle) is already the D2R/C&C model. The next leap comes from motion, light and sound, not more pixels.

**SDL capability facts [V]**
- SDL 3.4 adds `SDL_CreateGPURenderer`, custom fragment shaders for 2D (`SDL_CreateGPURenderState`, `SDL_SetGPURenderStateFragmentUniforms`), HDR colour spaces on the GPU renderer, and `SDL_SCALEMODE_PIXELART` ([linuxadictos summary](https://en.linuxadictos.com/SDL-3.4-strengthens-interoperability-between-the-3D-GPU-API-and-2D-rendering.html), [SDL wiki](https://wiki.libsdl.org/SDL3/SDL_CreateGPURenderState)).
- HDR output: `SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER = SDL_COLORSPACE_SRGB_LINEAR` is supported by the direct3d11, direct3d12 and metal renderers ([man page](https://manpages.debian.org/unstable/libsdl3-doc/SDL_CreateRendererWithProperties.3.en.html)).
- Shaders are authored in HLSL and cross-compiled with [SDL_shadercross](https://moonside.games/posts/introducing-sdl-shadercross/) to DXIL, SPIR-V and MSL.
- **[I]** In practice, custom render states mean opting into the `"gpu"` render driver (`SDL_HINT_RENDER_DRIVER` or `SDL_CreateGPURenderer`). That keeps all existing `SDL_Render*` calls working and adds per-draw fragment shaders. This is the cheapest route to a "next" tier without rewriting the compositor.

| Technique | SDL_Renderer now? | Needs SDL_GPU shader? | Offline / runtime | Cost | Classic risk |
|---|---|---|---|---|---|
| Light pools, day/night, flicker | Yes (exists) | — | Runtime | done | none |
| **Normal-mapped lighting** (N·L per light, from existing normal maps) | No (fixed-function can't do a dot product per light) | **Yes**, one fragment shader reading albedo, normal and light uniforms | Maps offline, lighting runtime | M | none |
| Soft contact shadows from height field | Fake now: blurred alpha blob, multiply | Better with a shader that ray-marches the depth map toward the light | Offline depth, runtime shade | M | none |
| Bloom | Approximation: downscale emissive layer, `SDL_SCALEMODE_LINEAR` up, additive | Proper threshold plus dual-Kawase blur | Runtime | S/M | none |
| Tilt-shift / DoF (city map) | Approximation: blend a blurred copy with a gradient-alpha mask | Proper with a depth-aware blur | Runtime | S | none; HD only, default subtle |
| Colour grade | No | 3D LUT (32³) in a shader | LUT authored offline | S | none |
| HDR (scRGB) | **Yes on D3D11** via output colourspace; emissives above 1.0 | Tonemap in shader optional | Runtime | S–M | none |
| Snow, steam, smoke, sparkles | **Yes**: textured quads, additive/alpha; `SDL_RenderGeometry` batches | — | Sprites offline (ComfyUI), sim runtime | M | none; cosmetic RNG must not use game RNG |
| Water / fountains / neon shimmer | UV-scroll of two textures | Distortion shader | Runtime | S | none |
| Parallax (title, city backdrop clouds) | Yes | — | Layers offline (Qwen-Image-Layered) | S | none |
| Animated tiles (traffic lights, signs, TVs) | Yes | — | Frames offline | S | low |
| Sub-pixel scroll and zoom | Yes: float dst rects (`SDL_FRect`), linear filter on HD layer | — | Runtime | S | none |
| CRT / "1994 look" | Simple scanline overlay texture now | Real CRT (CRT-Royale-like) needs a shader | Runtime | S | it *is* Classic; offer as a filter only |

**Normal maps: what works on pixel art [V]**
- In *The Siege and the Sandfox* the team found hand-painted normals beat automatic ones. They saw that fine normal detail reads as noise on 32×32 art, and recommend chunky, chiseled planes ([Game Developer](https://gamedeveloper.com/art/adding-depth-to-2d-with-hand-drawn-normal-maps-in-i-the-siege-and-the-sandfox-i-)).
- **[I]** The project's normals come from depth on the *HD-redrawn* furniture (at 1024 px or more), which sidesteps the tiny-sprite problem.
- Smooth the derived normals heavily (a bilateral filter on the depth map), clamp the strength, and mask out characters at first.
- Tools for manual touch-ups:
  - [Laigter](https://laigter.readthedocs.io/en/latest/Introduction/intro.html) (free, open source)
  - SpriteIlluminator (commercial)
  - [Marigold Normals](https://huggingface.co/prs-eth/marigold-normals-v1-1): direct normal estimation, OpenRAIL++-M, fine-tuned SD2.

**⚠ Licence finding [V]**
- Depth Anything V2 **Small is Apache-2.0, but Base, Large and Giant are CC-BY-NC-4.0** ([repo](https://github.com/DepthAnything/Depth-Anything-V2)). `depthmaps.py` uses **Large**.
- **[I]** This is fine for a free GPL fan project. If the game may ever be sold or put on a storefront, regenerate with V2-Small or Marigold, or record which model made each map (see §4).

**Upscalers in 2026 [V/I]**
- MMPX (McGuire & Gagiu 2021) preserves palette, transparency and single-pixel features better than EPX, xBR or HQX ([paper](https://casual-effects.com/research/McGuire2021PixelArt/McGuire2021PixelArt.pdf)). It is the right faithful baseline and is already used.
- SDL 3.4's `SDL_SCALEMODE_PIXELART` gives sharp-but-smooth runtime magnification for anything without an HD replacement **[V]**.
- Upscaler licences ([easygoing0114 table](https://huggingface.co/easygoing0114/AI_upscalers)):

  | Model | Licence | Use |
  |---|---|---|
  | **4x-UltraSharp / V2, 4x-AnimeSharp** | CC-BY-NC-SA-4.0 | Non-commercial only |
  | RealESRGAN x4plus anime_6B | BSD-3 | Permissive |
  | NMKD models | WTFPL | Permissive |
  | Nomos DAT/HAT | CC-BY-4.0 | Permissive, with attribution |

- **[I]** State of the art for this game is a hybrid, not one upscaler: MMPX for sprites, then diffusion re-paint under a depth ControlNet with colour lock (`redraw.py`), then an image-edit model for consistency across rotations and variants.
  - [Qwen-Image-Edit-2511](https://huggingface.co/Qwen/Qwen-Image-Edit-2511) (Apache-2.0) explicitly targets reduced drift and identity consistency.

**Interpolated animation [V]**
- Options:
  - [Practical-RIFE](https://github.com/hzwer/Pratical-RIFE): MIT, v4.25/4.26, anime improved.
  - [FILM](https://github.com/google-research/frame-interpolation): Apache-2.0, large motion.
  - [ToonCrafter](https://github.com/ToonCrafter/ToonCrafter): Apache-2.0, generative, 512×320, up to 16 frames, about 24 GB (community builds about 10–12 GB).
- **[I]**
  - Do in-betweening **offline** only. Runtime optical flow on sprites is not worth it.
  - The risks are alpha (interpolate RGB on a flat key colour, then interpolate alpha separately or re-threshold it) and silhouettes changing hit areas.
  - Ship in-betweens as *display-only* sub-frames indexed by `tick_alpha`: the sim still advances the original frame index, so Classic is untouched.
  - Use RIFE for walk cycles and FILM for big motions. Keep ToonCrafter for hero animations (chef, mafia), reviewed one by one.

## 2. Motion and feel

**Fixed timestep with interpolation [V]**
- Fiedler's canonical loop: `alpha = accumulator/dt`, render `lerp(prev, curr, alpha)`, and cap steps to avoid the spiral of death ([Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/)).
- OpenRCT2 tweens peeps and vehicles the same way, and found it was wasting work on off-screen entities and zoomed-out views ([OpenRCT2 blog](https://openrct2.io/blog/2025/05/behind-scenes-0-4-23)).

**[I] Concrete steps**
1. Give every renderable entity (`BobRenderable`, city cars, pedestrians, guests) a `prev_pos` captured at the start of each tick. Draw at `lerp(prev, cur, tick_alpha)` with float rects. Skip the lerp on teleports (a distance threshold) and when off-screen.
2. The camera already keeps float accumulators (`CameraController.h`). Add critically damped spring smoothing toward the target (`x += (target-x)*(1-exp(-k*dt))`) and wheel-zoom easing anchored at the cursor.
   - Between discrete map scales, cross-fade the two scale textures over 150–250 ms while scaling. Hit-tests use the *target* scale immediately, so input never lags.
3. **Frame pacing.** Default to VSync on, which is already an option. When VSync is off, prefer `SDL_DelayPrecise` pacing. Measure frame-time variance in the debug overlay. Don't let `FpsManager` sleep past vblank.
4. **Input latency.** Draw the cursor last, from the newest mouse position each frame (not the tick position). Keep the queue at 1 frame (SDL default).
5. **Juice** ("screen-space feedback", Jonasson & Purho, *Juice it or lose it*, 2012):
   - easing (`easeOutBack` for popups 180 ms, `easeOutCubic` for panels);
   - hover lift and glow;
   - money counters rolling up;
   - a small shake or flash only on big events (mafia attack, award).
   - All of it is driven by wall-clock `dt` in the presentation layer, never by sim ticks, and Classic disables it. The golden fingerprints cover sim state, so it can't break them as long as no code path reads presentation state.

## 3. Audio realism

**Libraries [V]**

| Library | Licence | Relevant features |
|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | Public domain or MIT-0 | Node graph, sound groups, panning, attenuation models, streaming ([manual](https://miniaud.io/docs/manual/index.html)) |
| [SoLoud](https://solhsa.com/soloud/) | zlib | Buses, filters, 3D. Copyright runs to 2020, so it is less active |
| [Steam Audio](https://valvesoftware.github.io/steam-audio) | Apache-2.0 since Feb 2024 ([FOSS Force](https://fossforce.com/2024/02/valve-releases-steam-audio-as-open-source/)) | HRTF, occlusion and reflections. Overkill for a top-down 2D game |
| [FFTConvolver](https://kandi.openweaver.com/c++/HiFi-LoFi/FFTConvolver) | MIT | Real-time partitioned convolution reverb |

**[I] Recommendation:** keep the existing lock-free callback mixer (it is well engineered). Add three things:
- buses: music, ambience, SFX, voice, UI;
- per-voice gain and constant-power pan;
- one reverb send through FFTConvolver with 3 short impulse responses (small, medium, large room) chosen by restaurant size.

Use miniaudio only if a full node graph becomes necessary.

**Design [I]**, the "living restaurant" layer:
- **Room tone loop.** Busyness is guest count / capacity, which comes from the sim and is only read. It cross-fades between quiet, medium and busy beds.
- **One-shot pools.** Cutlery, plates, chair scrapes, door bell, oven door, cash register, laughs, kitchen shouts. Each has 6–12 variants and plays with random pitch ±3%, gain ±2 dB, no-repeat-last-2 selection, and a Poisson trigger rate scaled by busyness. Pan from the sprite's screen X; gain falls with distance from the camera centre.
- **Mix rules.**
  - Duck music by 6–8 dB with 50 ms attack and 600 ms release when an alert, popup or voice line plays.
  - Muffle ambience with a low-pass when a full-screen menu is open.
  - Use a "Christmas" ambience variant (sleigh bells, carollers outside) for the existing Christmas playlist and the North Pole.
- **Cosmetic randomness** uses its own RNG stream, never the sim's. The codebase already has per-thread random streams.

**Loudness [V]**
- Sony and the IESD recommend **−23 LUFS ±2 integrated**, true peak ≤ −1 dBTP, for console. Sony's handheld figure was −18 LUFS ([Designing Sound](https://designingsound.org/2013/02/loudness-in-game-audio/)).
- **[I]** The −16 LUFS figure is a mobile and streaming convention and not a game standard I could verify. For a PC game, master music and ambience stems to about −20 to −23 LUFS integrated and −1 dBTP, with `ffmpeg -af loudnorm` (EBU R128) in the offline pipeline.

**Voice / TTS licences [V]**

| Model | Licence | Notes |
|---|---|---|
| [Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M) | Apache-2.0 | 54 voices, 8 languages |
| [Qwen3-TTS](https://gigazine.net/gsc_news/en/20260123-qwen3-tts-family-opensource) | Apache-2.0 | 0.6B and 1.7B; 10 languages including German and Italian; VoiceDesign from text descriptions |
| [F5-TTS](https://huggingface.co/SWivid/F5-TTS) weights | CC-BY-NC-4.0 | Non-commercial |
| [XTTS-v2](https://huggingface.co/coqui/XTTS-v2) | Coqui Public Model License | Non-commercial |
| StyleTTS2 | MIT code | Not verified here |

- **[I]** Use **Qwen3-TTS VoiceDesign** ("gruff Italian-American chef, warm, 50s") for character barks of 1–3 words ("Mamma mia!", "Ecco!", "Grazie!").
  - Short barks avoid the Monkey Island SE stiffness problem: no lip sync, no fixed text timing.
  - Never clone real voice actors.
  - Generate 5 takes per line per character, pick the best by hand, then loudness-normalise.

**Music [V]**
- [ACE-Step 1.5](https://github.com/ACE-Step/ACE-Step-1.5): code MIT (weights licence not stated on the page). It runs in under 4 GB in its smallest configuration; XL needs about 20 GB without offload. It supports stem extraction, repaint and cover. Its disclaimer warns of possible copyright similarity.
- **[I]**
  - Generate original tracks as **stems**: rhythm, bass, harmony, melody.
  - Layer them adaptively by busyness: empty restaurant = harmony only; full = all stems. Cross-fade over 4–8 bars, beat-aligned.
  - Keep the FM originals as the Classic track.

## 4. Asset pipeline on the 3090 Ti

**[I] Structure.** One content-addressed "asset bundle" per source sprite or frame:

```
pack-src/<asset_id>/
  source.png            # exported 1994 original (hash = identity)
  master_4k.png         # best HD albedo (premultiplied-free, straight alpha)
  master_16bit.png      # highest-res, 16-bit, pre-downscale (future: 8K / re-grade)
  depth_16.png          # 16-bit depth, visible pixels normalised
  normal.png            # tangent space, +Y convention documented
  emissive.png          # flames, screens, neon (mask x colour)
  mask_ids.png          # semantic parts: wood, cloth, metal, glass, skin, food
  roughness.png         # optional, for later PBR-ish specular
  layers/*.png          # Qwen-Image-Layered RGBA separations (bg/props/fx)
  variants/{winter,northpole,christmas,night}/albedo.png
  inbetween/{k}_{t}.png # RIFE/FILM sub-frames between key k and k+1
  provenance.json
```

**`provenance.json`** records:
- source SHA-256 and output SHA-256s;
- the ComfyUI workflow JSON hash, plus a copy of the workflow;
- model file names and their SHA-256s;
- each model's licence string (for example `"CC-BY-NC-4.0"`);
- the seed, prompt, negative prompt and denoise;
- tool versions, GPU and date;
- a reviewer flag (`approved` / `rejected` / `pending`).

**Fix the seed scheme now [V→I].** The current `rng.randrange` in queue order gives different seeds when `--limit`, patterns or resume change the queue.
- Use `seed = int(sha256(f"{global_seed}:{asset_group}:{asset_id}").hexdigest()[:8], 16)`.
- Use the *group* (for example one furniture item across all 4 rotations) where the art must match. That is already the policy in `redraw.py`.

**Determinism.**
- Pin the ComfyUI commit and custom-node commits.
- Use `--deterministic` and the same sampler and scheduler.
- Bit-identical outputs across driver versions are not guaranteed. Store the outputs and hashes; don't count on regenerating them.

**Runtime packing.**
- Atlas the albedo, normal and emissive maps in the *same* UV layout, so one shader lookup serves all three.
- Consider BC7 (albedo) and BC5 (normals) for VRAM at 4K once on SDL_GPU. Keep PNG for now.
- The engine consumes whichever maps exist and falls back silently.

**What to store now even if unused (future-proofing) [I]**
1. 16-bit masters at the highest resolution generated, before the final resize.
2. Depth (16-bit) and normals for *every* sprite and room, plus the model ID. Regenerate the NC-licensed ones with Apache or OpenRAIL models if needed.
3. Emissive and semantic material masks: cheap to make now, and they enable bloom, specular and per-material sound (footsteps on tile vs carpet).
4. Floor-contact points and footprints per sprite, for shadows and occlusion.
5. Layer separations for rooms and city backdrops, for parallax.
6. Clean ambience and voice stems at 48 kHz/24-bit, unnormalised, plus loudness-normalised exports.
7. Every rejected candidate's provenance, so it is never regenerated by accident.

## 5. Showcase

**[V]** C&C and D2R both made the instant toggle a headline feature. **[I]**
- **Before/after slider.** Extend F8 (already a vertical split at the pointer) with:
  - a wipe-handle UI;
  - a "pinned" mode the mouse can leave;
  - a horizontal mode;
  - a keyboard shortcut to animate the wipe (a trailer shot).
- **Photo mode.** Pause the presentation only (the sim is already frozen when paused). Free camera, hide UI, time-of-day slider (it drives `lighting::Scene` only), grade and LUT picker, tilt-shift strength, and a 2×/3× supersampled capture via an offscreen target with `with_current_frame`.
- **Restaurant day timelapse.** Record presentation snapshots (entity positions plus light scene) at low frequency into a ring buffer, *not* sim state, and play them back at 60× with the music stems. Then add an "export MP4" step with ffmpeg as a hidden background process (hidden windows, as the owner requires).
- **Trailer capture.**
  - A `--capture` mode that renders fixed-dt frames to PNG at 60 fps regardless of real time. Deterministic, so shots can be re-taken.
  - Scripted camera paths (JSON keyframes, eased).
  - Golden-seed demo saves. North Pole in snow at night is the money shot.

---

## Phased plan

**Now (SDL_Renderer, no shader code)**
1. `tick_alpha` interpolation for all entities and the camera; float rects everywhere.
2. Eased zoom, camera spring, UI easing and juice, number roll-ups.
3. Particle system (pooled quads, its own RNG): steam, smoke, snow, sparkles, candle embers.
4. Audio buses, pan by screen X, variation pools, busyness ambience, ducking, FFTConvolver room reverb; loudness-normalise all assets.
5. Approximate bloom (downsample and additive) and tilt-shift (gradient-masked blurred copy) as HD options, default subtle.
6. Photo mode, wipe slider, deterministic capture mode.
7. Fix the hdpack seed scheme; add `provenance.json`.

**Next (opt-in `"gpu"` renderer plus `SDL_GPURenderState` fragment shaders, HLSL via shadercross)**
1. Normal-mapped multi-light shader (up to 16 point lights in uniforms) replacing `LightPass` in the HD path. Keep `LightPass` as the fallback.
2. Height-field soft shadows from depth maps; emissive into a proper threshold and dual-Kawase bloom.
3. LUT colour grading, a vignette, a proper CRT filter as a "1994 TV" option.
4. HDR scRGB output with a paper-white slider (D3D11 already supports it on the existing renderer, so this could even move to "Now").
5. Water and neon distortion; snow accumulation masks on North Pole roofs, using semantic masks.

**Later**
1. Offline in-betweens (RIFE/FILM) as display-only sub-frames; ToonCrafter for hero animations.
2. Adaptive stem music (ACE-Step originals), Qwen3-TTS barks.
3. Seasonal and North Pole variant packs from Qwen-Image-Edit-2511, plus redraw with colour lock.
4. Optional per-pixel specular from roughness masks; screen-space ambient occlusion from depth.

## What to generate on the 3090 Ti now
1. Re-run all ComfyUI work with **hash-derived group seeds** and write provenance. Keep 16-bit masters.
2. **Depth and normals** for every pack picture. Add an **Apache-2.0 set (DA-V2-Small) or a Marigold normals set** beside the current DA-V2-Large set.
3. **Emissive masks** (ovens, candles, TVs, jukebox, neon, arcade) and **semantic material masks**: paint them semi-automatically, using SAM-style segmentation plus manual fixes.
4. **Particle sprite sheets**: steam puffs, smoke, snowflakes (3 sizes), sparkles and embers, as 8–16 frame loops with alpha.
5. **Room and city layer separations** with Qwen-Image-Layered, for parallax and depth-aware tilt-shift.
6. **North Pole and winter variants** of city tiles and restaurant exteriors (Qwen-Image-Edit-2511, colour-locked, same seeds per group).
7. **RIFE in-betweens for 2–3 walk cycles** as a pilot, with hit areas compared against the originals.
8. **Audio**: ACE-Step stems for 2–3 original tracks; Qwen3-TTS bark sets per character; impulse responses rendered from a simple room reverb at 3 sizes.

## Sources
- SDL: [SDL_CreateGPURenderState](https://wiki.libsdl.org/SDL3/SDL_CreateGPURenderState) · [SDL_CreateGPURenderer](https://www.mankier.com/3/SDL_CreateGPURenderer) · [SDL 3.4 summary](https://en.linuxadictos.com/SDL-3.4-strengthens-interoperability-between-the-3D-GPU-API-and-2D-rendering.html) · [SDL_CreateRendererWithProperties (HDR)](https://manpages.debian.org/unstable/libsdl3-doc/SDL_CreateRendererWithProperties.3.en.html) · [SDL_shadercross](https://moonside.games/posts/introducing-sdl-shadercross/)
- Remasters: [D2R two games at once](https://staging2.eip.gg/news/how-diablo-ii-resurrected-will-run-two-games-at-once) · [D2R breakpoints](https://www.tweaktown.com/news/77961/diablo-2-remaster-supports-breakpoint-builds-even-with-uncapped-fps/index.html) · [C&C Remastered strategy](https://www.gamedeveloper.com/design/ea-s-strategy-for-remastering-i-command-conquer-i-) · [AoE II DE](https://msn.gamereactor.eu/we-stayed-faithful-with-age-of-empires-ii-definitive-edition/) · [Monkey Island SE review](https://www.bit-tech.net/reviews/gaming/pc/the-secret-of-monkey-island-special-edn/2/) · [Commandos 2 HD review](https://useapotion.com/2020/12/commandos-2-hd-remaster-review) · [HD-2D](https://en.wikipedia.org/wiki/HD-2D) · [Octopath on Unreal](https://www.unrealengine.com/en-US/spotlights/octopath-traveler-s-hd-2d-art-style-and-story-make-for-a-jrpg-dream-come-true) · [OpenRCT2 0.4.23](https://openrct2.io/blog/2025/05/behind-scenes-0-4-23) · [Two Point Hospital audio](https://audient.com/news/2018/09/17/two-point-hospital-880/)
- Techniques: [Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/) · [Siege and the Sandfox normals](https://gamedeveloper.com/art/adding-depth-to-2d-with-hand-drawn-normal-maps-in-i-the-siege-and-the-sandfox-i-) · [Laigter](https://laigter.readthedocs.io/en/latest/Introduction/intro.html) · [MMPX paper](https://casual-effects.com/research/McGuire2021PixelArt/McGuire2021PixelArt.pdf)
- Models: [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2) · [Marigold Normals](https://huggingface.co/prs-eth/marigold-normals-v1-1) · [Upscaler licence table](https://huggingface.co/easygoing0114/AI_upscalers) · [Practical-RIFE](https://github.com/hzwer/Pratical-RIFE) · [FILM](https://github.com/google-research/frame-interpolation) · [ToonCrafter](https://github.com/ToonCrafter/ToonCrafter) ([arXiv 2405.17933](https://arxiv.org/abs/2405.17933)) · [Qwen-Image-Layered](https://github.com/QwenLM/Qwen-Image-Layered) ([arXiv 2512.15603](https://arxiv.org/abs/2512.15603)) · [Qwen-Image-Edit-2511](https://huggingface.co/Qwen/Qwen-Image-Edit-2511)
- Audio: [miniaudio](https://github.com/mackron/miniaudio) · [miniaudio manual](https://miniaud.io/docs/manual/index.html) · [SoLoud](https://solhsa.com/soloud/) · [Steam Audio](https://valvesoftware.github.io/steam-audio) · [Steam Audio Apache-2.0](https://fossforce.com/2024/02/valve-releases-steam-audio-as-open-source/) · [FFTConvolver](https://kandi.openweaver.com/c++/HiFi-LoFi/FFTConvolver) · [Loudness in game audio](https://designingsound.org/2013/02/loudness-in-game-audio/) · [Kokoro](https://huggingface.co/hexgrad/Kokoro-82M) · [Qwen3-TTS](https://gigazine.net/gsc_news/en/20260123-qwen3-tts-family-opensource) ([arXiv 2601.15621](https://arxiv.org/pdf/2601.15621v1)) · [F5-TTS](https://huggingface.co/SWivid/F5-TTS) · [XTTS-v2](https://huggingface.co/coqui/XTTS-v2) · [ACE-Step 1.5](https://github.com/ACE-Step/ACE-Step-1.5) ([arXiv 2602.00744](https://arxiv.org/html/2602.00744))

*Not verified:* the StyleTTS2 licence, the ACE-Step 1.5 weights licence, the MMPX code licence, and any −16 LUFS games standard. "Juice it or lose it" is cited from memory.
