# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every redrawn frame next to its original, numbered, for a frame-by-frame
review before a redraw goes into a pack.

  python review_pairs.py <original pack> <redraw-out>/0.80 <items.json> <out prefix>

Writes <out prefix>0.png, <out prefix>1.png, ... with 64 pairs each (8 per
row, original on the left). Frames not redrawn yet are skipped.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

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


def sheets(originals: Path, redraws: Path, files: list[str], out_prefix: str) -> list[Path]:
    files = [f for f in files if (redraws / f).exists()]
    per_sheet = PER_ROW * ROWS
    written: list[Path] = []
    for n in range(0, len(files), per_sheet):
        chunk = files[n : n + per_sheet]
        sheet = Image.new("RGB", (PER_ROW * (2 * CELL + 8), ROWS * (CELL + 16)), (40, 40, 46))
        draw = ImageDraw.Draw(sheet)
        for k, f in enumerate(chunk):
            x = (k % PER_ROW) * (2 * CELL + 8)
            y = (k // PER_ROW) * (CELL + 16)
            sheet.paste(thumb(originals / f), (x + 3, y + 3))
            sheet.paste(thumb(redraws / f), (x + CELL + 3, y + 3))
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
    files = [e["file"] for e in json.loads(items_json.read_text(encoding="utf-8"))]
    written = sheets(originals, redraws, files, out_prefix)
    print(f"{len(written)} sheets: {', '.join(str(p) for p in written)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
