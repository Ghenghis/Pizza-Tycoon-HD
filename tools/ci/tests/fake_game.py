# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the game, for the tests of hd_gates.py and check_release.py.

It reads an --exec script and answers the commands the gates send:
screenshot (a synthetic picture), dump fingerprint, bench, --version; every
other command is accepted silently. Like the game, it names each failed
command on stderr ("error: command failed: <line>") and exits 1 after them.

FAKE_COVERAGE  JSON: the flat share each screen's screenshot has, keyed by
               the last nav command before it (default 0.25)
FAKE_FAIL      JSON list of commands that fail
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 640, 400


def picture(k: int, flat_share: float) -> Image.Image:
    """640k x 400k: the first flat_share of the engine pixels are flat k x k blocks, the rest are not."""
    y, x = np.mgrid[0 : H * k, 0 : W * k]
    # x changes inside every block, so none is flat until the mask makes it so.
    a = np.stack([x % 256, y % 256, (x + y) % 256], axis=-1).astype(np.uint8)
    mask = np.zeros(W * H, dtype=bool)
    mask[: round(flat_share * W * H)] = True
    texels = np.repeat(np.repeat(mask.reshape(H, W), k, axis=0), k, axis=1)
    a[texels] = (90, 140, 60)
    return Image.fromarray(a)


def main(argv: list[str]) -> int:
    if "--version" in argv:
        print("Version: Pizza Tycoon HD 9.9.9 (Pizza Legacy 1.0.0-beta.2)")
        return 0
    script = Path(argv[argv.index("--exec") + 1])
    k = int(argv[argv.index("--hd-scale") + 1]) if "--hd-scale" in argv else 1
    coverage: dict[str, float] = json.loads(os.environ.get("FAKE_COVERAGE", "{}"))
    fail: list[str] = json.loads(os.environ.get("FAKE_FAIL", "[]"))
    current = ""
    failures = 0
    for raw in script.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line in fail:
            print(f"error: command failed: {line}", file=sys.stderr)
            failures += 1
            continue
        words = line.split()
        if words[0] == "screenshot":
            picture(k, coverage.get(current, 0.25)).save(words[1])
            print(f"Wrote {words[1]}")
        elif words[:2] == ["dump", "fingerprint"]:
            Path(words[2]).write_text("x" * 2000, encoding="utf-8")
        elif words[0] == "bench":
            print("bench: 300 frames at 3840x2400 (direct3d11): avg 3.0 ms, p99 4.4 ms, first frame 14.4 ms")
        elif words[0] == "nav":
            current = line
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
