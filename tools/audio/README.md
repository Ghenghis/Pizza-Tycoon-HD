# Audio tools

- `loudness_check.py`: measures a pack as the game plays it (BS.1770-4
  loudness, true peak). See `docs/studio/notes/sfx-overrides.md`.
- `check_recording.py`: checks a recording made with the recorder. See
  `docs/studio/notes/recording.md`.
- `sfx_matrix.py`: builds `data/audio/sfx-matrix.json`.

## Building the private effect packs

The packs hold sounds that may not be shared: Pizza Connection 3 clips
(owner-approved private reuse) and the owner's own 1994 effects. They are
never in this repo. The builders live next to the packs, in the private
candidates folder. Below, `<cloud-work>` is the folder that holds
`sfx-candidates` and `sfx-masters`, and `<repo>` is this checkout.

You need Python 3 with numpy, and ffmpeg on the PATH. Every step gives the
same bytes from the same inputs: the dither and the generators are seeded.

Inputs, which must already be there (no step extracts from game archives):

- `<cloud-work>/sfx-candidates/sources-20261114`: the staged sources. That
  is the Kenney CC0 sets (with their licence files) and the PC3 clips, plus a
  receipt listing them.
- `<cloud-work>/sfx-masters`: those sources decoded to WAV, plus
  `original/NN-ID.wav`, the 23 effects of the owner's game as the engine
  plays them.

The first three builders have their folders written at the top of the
script. Change those lines if `<cloud-work>` is not where they say.

Steps, in order:

1. Export the originals (only if `sfx-masters/original` is missing). Point
   `base` in the script at the game's `SOUND` folder first.

       python -I <cloud-work>/sfx-candidates/pack-v0/builder/orig_export.py

2. Build pack-v0, the replacements for 17 originals:

       python -I <cloud-work>/sfx-candidates/pack-v0/builder/build_pack.py

3. Build pools-v0, the variants and beds for the pack events and ambience:

       python -I <cloud-work>/sfx-candidates/pools-v0/builder/build_pools.py

4. Build the limited restaurant-noise-1 used by pack-v2:

       python -I <cloud-work>/sfx-candidates/pack-v0-derivatives/derive_noise1.py

5. Build pack-v1, which is pack-v0 plus the pools as manifest version 2. It
   finds its inputs beside itself.

       python -I <cloud-work>/sfx-candidates/pack-v1/builder/build_pack_v1.py

6. Build pack-v2. This is pack-v1 with the limited noise-1, plus derivatives
   of the six originals that had no replacement. It reads the matrix and the
   loudness code from `<repo>`.

       python -I <cloud-work>/sfx-candidates/pack-v2/builder/build_pack_v2.py <repo>

7. Check it. Expect two failures: kitchen-busy and kitchen-idle are below
   the bed window (see `docs/studio/notes/sfx-coverage.md`).

       python -I <repo>/tools/audio/loudness_check.py <cloud-work>/sfx-candidates/pack-v2

8. Optional: build the A/B listening page at `pack-v2/listen/index.html`.
   Run step 7 with `--json /tmp/chk/pack-v2.json` first, because the page
   reads that file.

       python -I <cloud-work>/sfx-candidates/pack-v2/builder/build_listen.py

To use a pack, copy the contents of its folder to `data/sfx/enhanced` next
to the game executable, then turn on the enhanced effects in the options. To
hear all 30 events through the real mixer, run the hidden test:

    PLEGACY_TEST_GAME_PATH=<game> PLEGACY_SFX_PACK=<cloud-work>/sfx-candidates/pack-v2 test_plegacy "[sfx-pack-v2]"
