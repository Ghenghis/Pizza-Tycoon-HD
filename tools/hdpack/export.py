#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the original Pizza Tycoon art without the engine.

Writes the same PNGs and manifest.json as ``plegacy --export-assets``, decoded
the same way (VGA run-length pictures, BOB sprite frames, PALETTE.E with the
palette the engine uses for each picture from palettes.json). Handy on a
machine where the game engine is not built, such as the GPU box that runs
ComfyUI. tests/test_export.py checks it pixel for pixel against the engine.

    python export.py <folder with PT.EXE> <output folder>
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
PALETTE_COUNT = 71
VGA_HEADER_SIZE = 8
MAX_VGA_SIDE = 320


class ExportError(Exception):
    """A problem the user has to fix (missing game files...)."""


@dataclass
class Report:
    vga_files: int = 0
    bob_files: int = 0
    bob_frames: int = 0
    skipped: int = 0


def find_ci(root: Path, relative: str) -> Path | None:
    """Case-insensitive lookup, like the engine on Linux."""
    current = root
    for part in Path(relative).parts:
        match = next((p for p in current.iterdir() if p.name.lower() == part.lower()), None)
        if match is None:
            return None
        current = match
    return current


def load_palettes(game_root: Path) -> list[list[tuple[int, int, int]]]:
    path = find_ci(game_root, "gfx/palette.e")
    if path is None:
        raise ExportError(f"{game_root} has no GFX/PALETTE.E; is this the folder with PT.EXE?")
    raw = path.read_bytes()
    if len(raw) != PALETTE_COUNT * 256 * 3:
        raise ExportError(f"{path} has the wrong size ({len(raw)} bytes)")
    # 6-bit VGA colours to 8 bits, exactly as the engine does (shift by 2).
    out = []
    for p in range(PALETTE_COUNT):
        base = p * 768
        out.append(
            [(raw[base + i * 3] << 2, raw[base + i * 3 + 1] << 2, raw[base + i * 3 + 2] << 2) for i in range(256)]
        )
    return out


def decode_vga(data: bytes) -> tuple[int, int, bytes] | None:
    """Run-length VGA picture. Returns None when the file is not one."""
    if len(data) <= VGA_HEADER_SIZE:
        return None
    width, height = struct.unpack_from("<HH", data, 0)
    if not (0 < width <= MAX_VGA_SIDE and 0 < height <= MAX_VGA_SIDE):
        return None
    length = len(data) - 1  # the last byte holds the threshold, not pixels
    threshold = data[length] - 1
    expected = width * height
    out = bytearray(expected)
    pos, idx = VGA_HEADER_SIZE, 0
    while pos < length:
        control = data[pos]
        pos += 1
        if control > threshold:
            if pos >= length:
                break
            count = control - threshold
            if idx + count > expected:
                break
            out[idx : idx + count] = bytes([data[pos]]) * count
            pos += 1
            idx += count
        else:
            count = control + 1
            if pos + count > length or idx + count > expected:
                break
            out[idx : idx + count] = data[pos : pos + count]
            pos += count
            idx += count
    return width, height, bytes(out)


def bob_frames(data: bytes) -> list[tuple[int, int, bytes]] | None:
    """Every frame of a BOB sprite file. None when the file is not one."""
    if len(data) < 6:
        return None
    count = struct.unpack_from("<H", data, 0)[0]
    header = 6 + count * 7
    if count == 0 or header > len(data):
        return None
    sizes = [(data[6 + i * 7], data[7 + i * 7]) for i in range(count)]
    if header + sum(w * h for w, h in sizes) > len(data):
        return None
    frames, offset = [], header
    for w, h in sizes:
        frames.append((w, h, data[offset : offset + w * h]))
        offset += w * h
    return frames


def to_rgba(width: int, height: int, pixels: bytes, palette: list[tuple[int, int, int]]) -> tuple[Image.Image, int]:
    """Index 0 is the transparent colour key."""
    lut = [(*palette[i], 255) for i in range(256)]
    lut[0] = (0, 0, 0, 0)
    flat = bytearray()
    opaque = 0
    for index in pixels:
        flat.extend(lut[index])
        opaque += index != 0
    return Image.frombytes("RGBA", (width, height), bytes(flat)), opaque


def normalize_key(relative: str) -> str:
    """Same as resources::HdPack::normalize_key."""
    key = relative.replace("\\", "/").lower()
    for ext in (".vga", ".bob"):
        if key.endswith(ext):
            key = key[: -len(ext)]
    while key.startswith("./"):
        key = key[2:]
    return key


def iter_assets(game_root: Path) -> Iterator[Path]:
    files = [p for p in game_root.rglob("*") if p.is_file() and p.suffix.lower() in (".vga", ".bob")]
    yield from sorted(files, key=lambda p: p.relative_to(game_root).as_posix())


def describe(
    kind: str, key: str, file: str, img: Image.Image, opaque: int, palettes: dict[str, int]
) -> dict[str, object]:
    return {
        "kind": kind,
        "key": key,
        "file": file,
        "width": img.width,
        "height": img.height,
        "palette": palettes.get(key, 0),
        "palette_known": key in palettes,
        "opaque_pixels": opaque,
        "transparent_pixels": img.width * img.height - opaque,
    }


def export(game_root: Path, out_dir: Path, palette_map: Path = HERE / "palettes.json") -> Report:
    if not game_root.is_dir():
        raise ExportError(f"{game_root} is not a folder")
    palettes = load_palettes(game_root)
    known: dict[str, int] = json.loads(palette_map.read_text(encoding="utf-8"))
    report = Report()
    entries: list[dict[str, object]] = []
    for path in iter_assets(game_root):
        rel = path.relative_to(game_root).as_posix()
        key = normalize_key(rel)
        data = path.read_bytes()
        if path.suffix.lower() == ".vga":
            pic = decode_vga(data)
            if pic is None:
                report.skipped += 1
                continue
            w, h, px = pic
            img, opaque = to_rgba(w, h, px, palettes[known.get(key, 0)])
            file = key + ".png"
            (out_dir / file).parent.mkdir(parents=True, exist_ok=True)
            img.save(out_dir / file)
            entries.append(describe("vga", key, file, img, opaque, known))
            report.vga_files += 1
        else:
            frames = bob_frames(data)
            if frames is None:
                report.skipped += 1
                continue
            for n, (w, h, px) in enumerate(frames):
                if w == 0 or h == 0:
                    continue
                img, opaque = to_rgba(w, h, px, palettes[known.get(key, 0)])
                file = f"{key}/{n:04d}.png"
                (out_dir / file).parent.mkdir(parents=True, exist_ok=True)
                img.save(out_dir / file)
                entry = describe("bob", key, file, img, opaque, known)
                entry["frame"] = n
                entries.append(entry)
                report.bob_frames += 1
            report.bob_files += 1
    manifest = {
        "format": 1,
        "generator": "Pizza Tycoon HD export.py",
        "engine_pixel_scale": 2,
        "note": "Each file is the original picture at its original size. Replace each PNG with a larger one "
        "of the same aspect ratio to make an HD pack.",
        "images": entries,
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("game", help="folder with PT.EXE")
    p.add_argument("out", help="output folder")
    args = p.parse_args(argv)
    try:
        r = export(Path(args.game), Path(args.out))
    except ExportError as exc:
        print("error:", exc, file=sys.stderr)
        return 2
    print(f"VGA pictures: {r.vga_files}\nBOB files: {r.bob_files}\nBOB frames: {r.bob_frames}\nSkipped: {r.skipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
