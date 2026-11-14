#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds data/audio/sfx-matrix.json: the original sound effects, one row per
audio::SoundId, joined to the game's own M containers, decoded here.

  python tools/audio/sfx_matrix.py --game <folder with SOUND/M0..M11>
  python tools/audio/sfx_matrix.py --game <...> --check   # exit 1 if stale

What it reads, the way the engine does (src/resources/MFileLoader.cpp,
src/engine/audio/VocDecoder.cpp, AudioManager::load_m_container):
  * an M container: u32 count, then per entry u32 type (1 CMF, 2 VOC; others
    are skipped), u32 offset, u32 length;
  * every VOC entry, in load order m0, m1 ... m11, takes the next effect slot,
    and slot n is SoundId n (CMF entries go to music, not to effect slots);
  * a VOC: "Creative Voice File\\x1A", u16 first-block offset at 20, then
    blocks (u8 type, u24 size). Type 1 is 8-bit unsigned mono PCM after a
    rate byte (rate = 1e6 / (256 - code)) and a codec byte (0 = PCM). The
    engine ignores every other block type; repeat blocks (6/7) are reported.

Call sites are found in the committed source (git HEAD, not the working
tree), not copied by hand. Everything else per row (trigger, scene, loop, priority,
reachability, candidates) is curated in EVENTS below.

Status columns (asset_check, engine_trigger_check, audible_review) are
written as "open". This tool never marks them done.

Standard library only; no audio is played or written.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "data" / "audio" / "sfx-matrix.json"
SCHEMA = "plegacy.sfx-matrix/1"

# The engine's load order (AudioManager::load_default_assets).
CONTAINERS = ["M0", "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8", "M9", "M10", "M11"]
ENGINE_RATE = 44100  # AudioManager::SAMPLE_RATE; mixer output is s16 stereo
CHOPPER_SKIP_INTERLEAVED = 500  # AudioManager.cpp: chopper start/loop offset

LOOP, ONE_SHOT = "loop", "one-shot"

# Curated per event. Order is the SoundId enum order (AudioManager.h).
EVENTS: list[dict] = []


def _event(
    sound_id,
    family,
    trigger,
    scene,
    role,
    priority,
    reachability,
    notes="",
    candidates=None,
):
    EVENTS.append(
        {
            "id": sound_id,
            "family": family,
            "trigger": trigger,
            "scene": scene,
            "role": role,
            "priority": priority,
            "reachability": reachability,
            "notes": notes,
            "candidates": candidates or [],
        }
    )


CROWD_NOTE = (
    "RestaurantView::update_restaurant_noise: seating utilization >21/>51/>81 % picks 1/2/3; "
    "off when closed, in the kitchen view, while the jukebox plays, or with options().restaurant_fx off. "
    "Stopped and restarted on each update."
)
for n, (lo, cand) in enumerate(
    [
        (">21 %", "pc3:GuestRoom_lowactivity"),
        (">51 %", "pc3:GuestRoom_midactivity"),
        (">81 %", "pc3:GuestRoom_highactivity"),
    ],
    start=1,
):
    _event(
        f"RESTAURANT_NOISE_{n}",
        "restaurant-crowd",
        f"restaurant open, dining room shown, seating utilization {lo}",
        "restaurant (dining room)",
        LOOP,
        "ambience",
        "visited",
        CROWD_NOTE,
        [cand],
    )

TRAFFIC_NOTE = "CityView::update_city_sound; needs options().map_fx; FAR zoom stops it."
_event(
    "TRAFFIC_1",
    "city-traffic",
    "city map at CLOSE zoom",
    "city view",
    LOOP,
    "ambience",
    "visited",
    TRAFFIC_NOTE,
    [
        "pc3:Blocks_Urban_Traffic_Day (staged, decoded: 120.8 s stereo, -35.7 LUFS; not in pack-v0)"
    ],
)
_event(
    "TRAFFIC_2",
    "city-traffic",
    "city map at MEDIUM zoom",
    "city view",
    LOOP,
    "ambience",
    "visited",
    TRAFFIC_NOTE,
    [
        "pc3:Blocks_Urban_Traffic_Night (staged, decoded: 155.5 s stereo, -36.9 LUFS; not in pack-v0)"
    ],
)
for d in range(10):
    _event(
        f"DIAL_TONE_{d}",
        "phone",
        f"digit {d} keyed on the terminal keypad",
        "terminal (phone)",
        ONE_SHOT,
        "ui",
        "visited",
        "Terminal: SoundId::DIAL_TONE_0 + digit; one voice per key press.",
        ["pack-v0: ITU-T Q.23 DTMF, authored (synthesized standard signal)"],
    )
_event(
    "PHONE_RINGING",
    "phone",
    "call placed (waiting for connection) and the error phase",
    "terminal (phone)",
    LOOP,
    "alert",
    "visited",
    "Looped in WAITING_FOR_CONNECTION and ERROR; stopped in IDLE, PHONE_PICKUP and the destructor.",
    [
        "pack-v0: North American ringback 440+480 Hz, 2 s on / 4 s off, authored (synthesized standard signal)"
    ],
)
_event(
    "PHONE_PICKUP",
    "phone",
    "the other side picks up",
    "terminal (phone)",
    ONE_SHOT,
    "alert",
    "visited",
    "Played after PHONE_RINGING is stopped.",
    [
        "original kept in pack-v0; an authored click is a variant for the audible review only"
    ],
)
_event(
    "CHOPPER",
    "kitchen-prep",
    "a choppable topping is chopped in the pizza designer",
    "pizza designer",
    ONE_SHOT,
    "feedback",
    "visited",
    "PizzaDesign::start_chop; the blade animation runs NUMBER_OF_CHOP_MS = 600 ms flipping every 55 ms. "
    "Loader starts playback 500 interleaved samples (250 frames at 44.1 kHz) in to skip a static lead-in; "
    "a clean replacement must carry its own start offset instead.",
    ["pc3:PizzaCreator/Ingredients/cut.ogg"],
)
_event(
    "WEAPON_1",
    "attack",
    "flame weapon active (and no police, no explosion)",
    "restaurant attack scene",
    LOOP,
    "feedback",
    "visited",
    "AttackScene: play_sound_if_changed priority chain: siren > explosion > flame > bazooka.",
)
_event(
    "WEAPON_2",
    "attack",
    "none found",
    "-",
    "unknown",
    "-",
    "unverified",
    "No call site at HEAD (only the enum). Reachability stays unverified until proven; do not count it.",
)
_event(
    "WEAPON_3",
    "attack",
    "bazooka projectile in flight",
    "restaurant attack scene",
    ONE_SHOT,
    "feedback",
    "visited",
    "AttackScene priority chain (see WEAPON_1).",
)
_event(
    "WEAPON_4",
    "attack",
    "explosion active",
    "restaurant attack scene",
    ONE_SHOT,
    "feedback",
    "visited",
    "AttackScene priority chain (see WEAPON_1).",
)
_event(
    "POLICE_SIREN",
    "attack",
    "police window after an attack",
    "restaurant attack scene",
    LOOP,
    "alert",
    "visited",
    "AttackScene priority chain: highest.",
    [
        "pc3:Policecar_Siren_1 / Policecar_Siren_2 (staged, decoded: 1.05 s / 0.69 s chirps, not a continuous siren loop; not in pack-v0)"
    ],
)

# Things the game really does that make no sound now. Not coverage.
NEW_EVENTS = [
    {
        "id": "GUEST_ENTERS",
        "family": "doors",
        "emitted_by": "GuestWalkScene::spawn_guest",
        "trigger": "a guest is spawned at the customer door",
        "role": ONE_SHOT,
        "status": "needs new trigger",
        "candidates": [
            "pc3:Scooter_DoorBell (lead, 2.08 s)",
            "kenney: Foley Sounds (door-like, unreviewed)",
        ],
    },
    {
        "id": "GUEST_LEAVES",
        "family": "doors",
        "emitted_by": "GuestWalkScene::spawn_guest_departure",
        "trigger": "the oldest seated guest leaves through the customer door",
        "role": ONE_SHOT,
        "status": "needs new trigger",
        "candidates": [],
    },
    {
        "id": "PIZZA_SERVED",
        "family": "service",
        "emitted_by": "GuestWalkScene::waiter_delivered",
        "trigger": "a waiter reaches the table and the guest starts eating",
        "role": ONE_SHOT,
        "status": "needs new trigger",
        "candidates": ["pc3:PizzaCreator/Ingredients/placeSmall.ogg (lead)"],
    },
    {
        "id": "KITCHEN_AMBIENCE",
        "family": "kitchen",
        "emitted_by": "RestaurantView kitchen_view_ (crowd noise is off there)",
        "trigger": "kitchen view shown while open",
        "role": LOOP,
        "status": "needs new trigger",
        "candidates": ["pc3:Kitchen_activity / Kitchen_noactivity (lead)"],
    },
]
# What the private pack pack-v0 (outside the repo) puts in place of the
# original. Synthesized files are never called restored.
RESTORED = "restored (owned PC3 recording, level-matched/edited)"
SYNTH = "authored (synthesized standard signal)"
PACK_V0 = {
    "RESTAURANT_NOISE_1": RESTORED,
    "RESTAURANT_NOISE_2": RESTORED,
    "RESTAURANT_NOISE_3": RESTORED,
    **{f"DIAL_TONE_{d}": SYNTH for d in range(10)},
    "PHONE_RINGING": SYNTH,
    "CHOPPER": RESTORED,
}

NOT_FOUND_FAMILIES = {
    "ovens": "no discrete oven event in the views (ovens are bought/listed only)",
    "payment": "no per-guest payment event; takings are shown as reports",
}


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=True
    ).stdout.strip()


def call_sites(sound_id: str) -> list[dict]:
    """Every use of SoundId::<id> in src, outside the audio engine itself."""
    out = subprocess.run(
        ["git", "grep", "-n", "-w", f"SoundId::{sound_id}", "HEAD", "--", "src"],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    sites = []
    for line in out.splitlines():
        # HEAD:path:line:code (the commit, not the working tree, so the
        # lines match the commit stamped in the matrix)
        _rev, path, lineno, text = line.split(":", 3)
        if path.startswith("src/engine/audio/"):
            continue
        sites.append({"site": f"{path}:{lineno}", "code": text.strip()})
    if sound_id.startswith("DIAL_TONE_") and sound_id != "DIAL_TONE_0":
        sites = [
            dict(s, indexed="DIAL_TONE_0 + digit") for s in call_sites("DIAL_TONE_0")
        ]
    return sites


def read_containers(game: Path) -> list[dict]:
    """VOC entries in effect-slot order, decoded."""
    slots = []
    sound = game / "SOUND"
    for name in CONTAINERS:
        path = sound / name
        data = path.read_bytes()
        (count,) = struct.unpack_from("<I", data, 0)
        voc_seen = 0
        for index in range(count):
            kind, offset, length = struct.unpack_from("<III", data, 4 + index * 12)
            if kind != 2:
                continue
            blob = data[offset : offset + length]
            slots.append(
                decode_voc(blob, f"sound/{name.lower()}", index, voc_seen, path, data)
            )
            voc_seen += 1
    return slots


def decode_voc(
    blob: bytes, container: str, index: int, voc_index: int, path: Path, whole: bytes
) -> dict:
    if blob[:20] != b"Creative Voice File\x1a":
        raise ValueError(f"{container}[{index}]: not a VOC")
    (pos,) = struct.unpack_from("<H", blob, 20)
    pcm = bytearray()
    rates = set()
    blocks = []
    repeat = None
    while pos < len(blob):
        kind = blob[pos]
        pos += 1
        if kind == 0:
            blocks.append(0)
            break
        size = blob[pos] | blob[pos + 1] << 8 | blob[pos + 2] << 16
        pos += 3
        body = blob[pos : pos + size]
        if kind == 1:
            code, codec = body[0], body[1]
            if codec != 0:
                raise ValueError(f"{container}[{index}]: codec {codec}")
            rates.add(1_000_000 // (256 - code))
            pcm += body[2:]
        elif kind == 6:
            (repeat,) = struct.unpack_from("<H", body, 0)
        blocks.append(kind)
        pos += size
    if len(rates) != 1:
        raise ValueError(f"{container}[{index}]: rates {rates}")
    rate = rates.pop()
    n = len(pcm)
    peak = max(abs(b - 128) for b in pcm)
    dc = sum(pcm) / n - 128.0
    quiet = 2  # within +-2 of 128 counts as silence
    lead = next((i for i, b in enumerate(pcm) if abs(b - 128) > quiet), n)
    tail = next((i for i, b in enumerate(reversed(pcm)) if abs(b - 128) > quiet), n)
    engine_frames = n * ENGINE_RATE // rate  # VocDecoder::resample_linear
    return {
        "container": container,
        "container_sha256": hashlib.sha256(whole).hexdigest(),
        "index": index,
        "voc_index": voc_index,
        "format": "VOC block 1, 8-bit unsigned PCM",
        "channels": 1,
        "rate_hz": rate,
        "samples": n,
        "duration_s": round(n / rate, 4),
        "voc_sha256": hashlib.sha256(blob).hexdigest(),
        "pcm_sha256": hashlib.sha256(bytes(pcm)).hexdigest(),
        "voc_blocks": blocks,
        "voc_repeat_count": repeat,
        "peak_u8_from_128": peak,
        "clipped_samples": sum(1 for b in pcm if b in (0, 255)),
        "dc_offset_u8": round(dc, 3),
        "leading_quiet_ms": round(1000 * lead / rate, 1),
        "trailing_quiet_ms": round(1000 * tail / rate, 1),
        "engine_decoded": {
            "rate_hz": ENGINE_RATE,
            "channels": 2,
            "frames": engine_frames,
            "bytes_s16": engine_frames * 4,
            "note": "linear resample, mono duplicated; VOC repeat blocks are not honoured",
        },
    }


def build(game: Path) -> dict:
    slots = read_containers(game)
    if len(slots) != len(EVENTS):
        raise SystemExit(
            f"{len(slots)} VOC slots in the game, {len(EVENTS)} SoundIds curated"
        )
    rows = []
    for slot, (event, voc) in enumerate(zip(EVENTS, slots)):
        row = {
            "slot": slot,
            **event,
            "call_sites": call_sites(event["id"]),
            "original": voc,
        }
        if event["id"] == "CHOPPER":
            row["original"]["engine_start_offset_frames"] = (
                CHOPPER_SKIP_INTERLEAVED // 2
            )
        if not row["call_sites"] and event["reachability"] == "visited":
            raise SystemExit(f"{event['id']}: marked visited but no call site at HEAD")
        if event["id"] in PACK_V0:
            row["replacement"] = {"pack": "pack-v0", "origin": PACK_V0[event["id"]]}
        row["fallback"] = "original"
        row["selector"] = (
            "effects: Original / Enhanced; no selector in the engine yet (see docs/studio/notes/sfx-matrix.md)"
        )
        row["asset_check"] = "open"
        row["engine_trigger_check"] = "open"
        row["audible_review"] = "open"
        rows.append(row)
    return {
        "schema": SCHEMA,
        "commit": git("rev-parse", "HEAD"),
        "generator": "tools/audio/sfx_matrix.py",
        "game_sound_dir": "SOUND (M0..M11 of the 1994 release; path not recorded)",
        "reachability_terms": {
            "visited": "a call site on a normal gameplay path exists at this commit (static; not observed at runtime)",
            "unverified": "no gameplay call site found; not counted",
            "unused": "proven never played",
        },
        "rows": rows,
        "candidate_new_events": NEW_EVENTS,
        "families_not_backed_by_events": NOT_FOUND_FAMILIES,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--game", type=Path, required=True, help="folder holding SOUND/M0..M11"
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="fail if the committed matrix differs (commit aside)",
    )
    p.add_argument("--out", type=Path, default=OUT)
    a = p.parse_args(argv)
    matrix = build(a.game)
    text = json.dumps(matrix, indent=2) + "\n"
    if a.check:
        old = json.loads(a.out.read_text()) if a.out.exists() else {}
        old.pop("commit", None)
        new = dict(matrix)
        new.pop("commit")
        if old != json.loads(json.dumps(new)):
            print(
                f"{a.out} is stale: run tools/audio/sfx_matrix.py --game ...",
                file=sys.stderr,
            )
            return 1
        print(f"{a.out}: up to date ({len(matrix['rows'])} rows)")
        return 0
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(text)
    print(
        f"wrote {a.out.relative_to(REPO)}: {len(matrix['rows'])} rows, all decoded from the game"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
