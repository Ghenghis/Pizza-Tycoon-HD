# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every redrawn frame next to its original, numbered, for a frame-by-frame
review before a redraw goes into a pack.

  python review_pairs.py <original pack> <redraw-out>/0.80 <items.json> <out prefix>

Writes <out prefix>0.png, <out prefix>1.png, ... with 64 pairs each (8 per
row, original on the left). Frames not redrawn yet are skipped. File names
from items.json must stay inside both folders.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

# The sibling module, found even under `python -I` (which leaves the
# script's folder off sys.path).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpack  # noqa: E402

CELL = 140
PER_ROW = 8
ROWS = 8


def thumb(path: Path) -> Image.Image:
    with Image.open(path) as src:
        im = src.convert("RGBA")
    inner = CELL - 6
    s = min(inner / im.width, inner / im.height)
    im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.Resampling.LANCZOS)
    bg = Image.new("RGBA", (inner, inner), (200, 185, 160, 255))
    bg.alpha_composite(im, ((inner - im.width) // 2, (inner - im.height) // 2))
    return bg.convert("RGB")


def load_files(items_json: Path) -> list[str]:
    """The file names in items.json, each checked to be a plain relative path."""
    try:
        data = json.loads(items_json.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise hdpack.HdPackError(f"{items_json}: {exc}") from exc
    if not isinstance(data, list):
        raise hdpack.HdPackError(f"{items_json}: expected a list of items")
    files: list[str] = []
    for n, e in enumerate(data):
        try:
            files.append(hdpack.check_relative(e.get("file") if isinstance(e, dict) else None))
        except hdpack.UnsafePathError as exc:
            raise hdpack.UnsafePathError(f"{items_json}: item {n}: {exc}") from exc
    return files


def sheets(
    originals: Path, redraws: Path, files: list[str], out_prefix: str, errors: list[str] | None = None
) -> list[Path]:
    """Writes the sheets. A file a symlink would take outside its folder is
    left out, with the reason added to errors."""
    pairs: list[tuple[str, Path, Path]] = []
    for f in files:
        try:
            original, redrawn = hdpack.contained_path(originals, f), hdpack.contained_path(redraws, f)
        except hdpack.UnsafePathError as exc:
            if errors is not None:
                errors.append(f"{f}: {exc}")
            continue
        if redrawn.exists():
            pairs.append((f, original, redrawn))
    per_sheet = PER_ROW * ROWS
    written: list[Path] = []
    for n in range(0, len(pairs), per_sheet):
        chunk = pairs[n : n + per_sheet]
        sheet = Image.new("RGB", (PER_ROW * (2 * CELL + 8), ROWS * (CELL + 16)), (40, 40, 46))
        draw = ImageDraw.Draw(sheet)
        for k, (f, original, redrawn) in enumerate(chunk):
            x = (k % PER_ROW) * (2 * CELL + 8)
            y = (k // PER_ROW) * (CELL + 16)
            sheet.paste(thumb(original), (x + 3, y + 3))
            sheet.paste(thumb(redrawn), (x + CELL + 3, y + 3))
            draw.text((x + 3, y + CELL), f"{n + k}: {Path(f).parent.name}/{Path(f).stem}", fill=(255, 220, 120))
        path = Path(f"{out_prefix}{n // per_sheet}.png")
        sheet.save(path)
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    originals, redraws, items_json, out_prefix = Path(args[0]), Path(args[1]), Path(args[2]), args[3]
    try:
        files = load_files(items_json)
    except hdpack.HdPackError as exc:
        print("error:", exc, file=sys.stderr)
        return 2
    errors: list[str] = []
    written = sheets(originals, redraws, files, out_prefix, errors)
    for e in errors:
        print("error:", e, file=sys.stderr)
    print(f"{len(written)} sheets: {', '.join(str(p) for p in written)}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
