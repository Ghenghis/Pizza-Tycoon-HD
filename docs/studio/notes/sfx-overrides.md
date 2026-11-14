# Sound effect overrides: pack schema (engine side)

2026-11-14. What `audio::load_effect_pack()` (`src/engine/audio/EffectPack.cpp`) reads. The audio
engine owns this file's meaning; the effects curator fills the manifest in its own folder. Field
names are open for agreement through the integrator until the first pack ships; after that they
change only with `version`.

## Where it lives

- The shipped pack: `data/sfx/enhanced/manifest.json`, with its sound files below that folder.
- The player picks **Sound Effects: Original / Enhanced** (audio options; saved as
  `enhanced_effects` in the config file). Restore Original Audio sets it back to Original.

## manifest.json

```json
{
  "format": "pthd-sfx-overrides",
  "version": 1,
  "pack": { "id": "pthd-enhanced", "revision": 1, "name": "Pizza Tycoon HD effects" },
  "effects": [
    {
      "event": "phone-ringing",
      "slot": 15,
      "file": "phone/ringing.wav",
      "sha256": "<64 hex: the file's bytes>",
      "source_sha256": "<64 hex: the original VOC data it replaces>",
      "gain_db": -1.5,
      "trim_start_ms": 0,
      "trim_end_ms": 0,
      "loop_start_ms": 0,
      "loop": false,
      "fallback": "original",
      "origin": "authored",
      "provenance": "...",
      "license": "..."
    }
  ]
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `format`, `version` | yes | `pthd-sfx-overrides`, `1`. Anything else: the whole pack is refused. |
| `pack.id`, `pack.revision` | no | Shown and logged; the pack's identity. |
| `event` | yes | The stable event id (below). Entries are keyed by event, never by position. |
| `slot` | no | When given, it must equal the event's slot number, as a check. |
| `file` | yes | A relative path inside the pack folder; `..`, absolute paths and links leading out are refused. |
| `sha256` | yes | Of the file's bytes; a mismatch is refused. |
| `source_sha256` | yes | Of the original effect's VOC data as the game loads it from its `m` container (`AudioManager::effect_source_sha256()`); a mismatch means the pack was made for other originals and is refused. |
| `gain_db` | no | Applied once at load, clamped to -60 to +12 dB, with saturation. Default 0. |
| `trim_start_ms`, `trim_end_ms` | no | Cut from the decoded sound. Default 0. |
| `loop_start_ms` | no | Where a looping play starts and loops back, after the trims. Default 0. This replaces the original chopper's built-in 500-sample skip, which is never applied to a replacement. |
| `loop` | no | What the asset was made for; the game's call still decides looping. |
| `fallback` | no | Only `original` (the default). |
| `origin`, `provenance`, `license` | no | Kept for the asset matrix; not read by the engine. |

Files: WAV (PCM 8/16/24/32-bit, float) or MP3, decoded to 44100 Hz stereo with the same checks as
music (header first, then the decoder). Limits per file: 16 MiB, 120 s; per pack: 64 MiB decoded,
256 entries, a 1 MiB manifest.

## Event ids (slot numbers never change)

| Slot | Event | Slot | Event |
| --- | --- | --- | --- |
| 0 | `restaurant-noise-1` | 12 | `dial-tone-7` |
| 1 | `restaurant-noise-2` | 13 | `dial-tone-8` |
| 2 | `restaurant-noise-3` | 14 | `dial-tone-9` |
| 3 | `traffic-1` | 15 | `phone-ringing` |
| 4 | `traffic-2` | 16 | `phone-pickup` |
| 5 to 11 | `dial-tone-0` to `dial-tone-6` | 17 | `chopper` |
| 18 to 21 | `weapon-1` to `weapon-4` | 22 | `police-siren` |

## What happens

- The whole pack is read, checked and decoded on the game thread before anything changes; the
  audio callback only ever switches to a finished set, at the start of a block, all slots at once.
- An entry that is missing, damaged, too long, too large, outside the folder, for an unknown event
  or for other originals leaves its event on the original, with the reason in
  `EffectsChange::outcomes`. No other slot moves.
- A pack whose manifest cannot be used (absent, damaged, other format or version) is refused as a
  whole and the previous selection stays.
- Sounds that are playing carry on in the new set; past a shorter sound's end a loop starts again
  and a one-shot ends.
