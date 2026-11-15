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
| `format`, `version` | yes | `pthd-sfx-overrides`, `1` or `2`. Anything else: the whole pack is refused. |
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
music (header first, then the decoder). Limits per file: 16 MiB, 120 s; per pack: 96 MiB decoded (a sound two scenes share counts twice),
256 entries, a 1 MiB manifest.

## Version 2: variants, priorities, ambience

Everything above holds; version 1 packs read exactly as before, and the fields below are read only
when `version` is `2`.

```json
{
  "format": "pthd-sfx-overrides",
  "version": 2,
  "effects": [
    {
      "event": "phone-pickup",
      "source_sha256": "<64 hex>",
      "gain_db": -2,
      "priority": "feedback",
      "variants": [
        { "file": "phone/pickup-a.wav", "sha256": "<64 hex>" },
        { "file": "phone/pickup-b.wav", "sha256": "<64 hex>", "gain_db": -3.5, "trim_end_ms": 40 }
      ]
    }
  ],
  "ambience": [
    {
      "with": "restaurant-noise-1",
      "layers": [
        {
          "name": "cutlery",
          "variants": [
            { "file": "room/cutlery-1.wav", "sha256": "<64 hex>" },
            { "file": "room/cutlery-2.wav", "sha256": "<64 hex>" }
          ],
          "gain_db": -9,
          "interval_ms": [2500, 7000],
          "pan": [-0.6, 0.6]
        }
      ]
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `variants` | Instead of `file`/`sha256`: 1 to 8 sounds, each with its own `file` and `sha256`, and optionally its own `gain_db`, `trim_start_ms`, `trim_end_ms`, `loop_start_ms`; the entry's values are the defaults. Each play picks one at random, never the one played last for that event, with the engine's own generator (never the game's random streams). A variant that is refused is left out with its reason (`variant 2: ...`) and the others play; with none left the event keeps its original. |
| `priority` | `alert`, `feedback` or `ambience`, for an entry that is replaced. Without it: `phone-ringing` and `police-siren` are alerts; `restaurant-noise-1..3`, `traffic-1..2` ambience; the rest feedback. Anything else refuses the entry. |
| `ambience` | Up to 8 scenes. A scene's `with` names its bed: an event (original or replaced) that the game loops. While the bed plays, each of its up to 8 layers plays one of its variants (rules as above) every `interval_ms` [least, most] (250 to 600000 ms, random in between), panned at random within `pan` [left, right] (-1 to 1, default [0, 0]). A layer starts its first interval when the bed starts, and stops when it stops. A layer that is refused (no name, unknown bed, bad interval or pan, no variant that loads) is reported in the outcomes with `layer` set, and the others play. |

### Pack events (version 2)

Seven events had no sound in 1994. Only a pack sounds them; without one (Original effects, or a
pack that does not list them) they are silent and nothing at all changes in the output. These slot
numbers are final (published 2026-11-14 with `door-close`, before any pack shipped).

| Event | Slot | Plays | Default priority | Call it when |
| --- | --- | --- | --- | --- |
| `guest-enter` | 57 | once | feedback | a guest comes in through the door (the door opens) |
| `guest-leave` | 58 | once | feedback | a guest goes out through the door (the door opens) |
| `door-close` | 59 | once | feedback | the door shuts behind a guest, a moment after either |
| `payment` | 60 | once | feedback | a guest pays |
| `plate-served` | 61 | once | feedback | a waiter puts a pizza on a table |
| `kitchen-busy` | 62 | loops | ambience | the kitchen view is open and the kitchen is busy |
| `kitchen-idle` | 63 | loops | ambience | the kitchen view is open and the kitchen is quiet |

A pack-event entry is a normal version 2 entry (a `file` or `variants`, and the fields below),
but without `source_sha256`: it replaces no original. In version 1, these ids are "no such event".
They can also be a scene's bed (`"with": "kitchen-busy"`).

**The game's API** (`engine/audio/AudioManager.h`, game thread; `context.audio`):

```cpp
// One-shots. Returns false, doing nothing, when the effects in use have no
// sound for it.
bool play_event(std::string_view event, audio::Placement place = {});
bool play_event(audio::PackEvent event, audio::Placement place = {});
// Beds: loop from start_event() until stop_event().
bool start_event(std::string_view event, audio::Placement place = {});
bool start_event(audio::PackEvent event, audio::Placement place = {});
void stop_event(std::string_view event);
void stop_event(audio::PackEvent event);
bool has_event(std::string_view event) const;  // the pack in use has it
bool event_playing(audio::PackEvent event) const;
```

- Call these from the views at the moments in the table. Pass
  `audio::screen_placement(x, y, 640, 400)` for a door or a table that has a place on the screen.
- The calls are cheap and safe to make every time, with or without a pack. No `has_event()` check
  is needed first.
- For the kitchen, keep exactly one bed running while the kitchen view is open:
  - start `kitchen-busy` when it opens with the restaurant open and utilization over 21 %, and
    `kitchen-idle` otherwise;
  - switch beds (stop one, start the other) when that changes;
  - stop the bed when the view closes.
  - The restaurant crowd (`restaurant-noise-*`) is off in the kitchen view, as now.

### Pool rules (version 2)

Allowed on any version 2 entry and on any ambience layer:

| Field | Meaning |
| --- | --- |
| `gain_jitter_db` | 0 to 6. Each play gets a random gain within +/- this (the engine's own generator). Default 0. |
| `min_spacing_ms` | 0 to 60000. A play sooner than this after the last start is skipped (a layer waits instead). Default 0. |
| `duck_db` | 0 to 24. While an alert plays, the sound dips this much, with the same timing as the music (0.08 s down, 0.6 s back), and only if ducking is on. Default 0. |
| `max_voices` | Layers only: 0 to 16 of its one-shots at once; one beyond that is left out. 0: only the voice limit. Default 0. |

An event plays one sound at a time: playing it again starts it over, unless its spacing skips the
play.

How it plays:

- **Voices.** At most 16 sounds at once by default. The game's own events always play; ambience
  one-shots take the voices left. One that finds none is left out, and one that is playing is cut
  (oldest first) when feedback or an alert starts while the voices are full. So alerts and feedback
  are never lost to ambience.
- **Placement.** `AudioManager::play_sound_at(id, screen_placement(x, y, 640, 400))` pans by the
  screen x (at most 0.8 to a side) and, off the screen, lowers the gain with the distance, to 0.3 a
  screen width away. Panning only lowers the far channel, so a placed sound is never louder than in
  the middle. Ambience layers are panned the same way.
- **Ducking.** While an alert plays, the music goes down 5 dB in 0.08 s and comes back over 0.6 s,
  in straight lines with no step. Sounds with a `duck_db` dip by that amount, with the same timing.
  On by default; **Music Ducking** in the audio options (config `duck_music`) turns all ducking
  off.
- Switching packs ends the old pack's ambience one-shots at once; the next scene starts from a
  full interval.

## Loudness

`python tools/audio/loudness_check.py <pack folder>` measures what the game plays (after `gain_db`
and trims) per sound: integrated loudness (ITU-R BS.1770-4, gated), true peak (4x oversampled), and
sample peak. Ambience beds (ambience-priority events, the kitchen beds included) must be -23 LUFS
integrated, within 2 LU. Every sound must stay at or under -1 dBTP, measured at the top of its
`gain_jitter_db`. `--json` writes the report as JSON; it exits 1 when a
sound fails. WAV is read directly; MP3 needs `ffmpeg` on the PATH.

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
