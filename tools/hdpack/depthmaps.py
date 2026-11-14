# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Depth and surface-normal maps for an HD pack, for 2.5D lighting.

For every picture in the pack this writes, under the output folder and with
the same relative path:

  depth/<file>.png    16-bit greyscale, white = near, transparent pixels 0
  normal/<file>.png   RGB normal map (tangent space, +Y up), from the depth

Depth comes from Depth Anything V2 (Hugging Face transformers) on the GPU.
The maps are inputs for the engine's lighting pass (lamps, oven glow,
shadows, day and night); the art itself is not changed.

  python depthmaps.py <pack> <out> [--device cuda:0] [--only 'gfx/*']
"""

from __future__ import annotations

import argparse
import fnmatch
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from numpy.typing import NDArray
from PIL import Image

# The sibling module, found even under `python -I` (which leaves the
# script's folder off sys.path).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpack  # noqa: E402

MODEL = "depth-anything/Depth-Anything-V2-Large-hf"
F32 = NDArray[np.float32]


def normals_from_depth(depth: F32, strength: float = 6.0) -> NDArray[np.uint8]:
    """Tangent-space normals from a 0..1 depth map (1 = near)."""
    dz_dx = np.zeros_like(depth)
    dz_dy = np.zeros_like(depth)
    dz_dx[:, 1:-1] = (depth[:, 2:] - depth[:, :-2]) * 0.5
    dz_dy[1:-1, :] = (depth[2:, :] - depth[:-2, :]) * 0.5
    nx = -dz_dx * strength * depth.shape[1] / 256.0
    ny = dz_dy * strength * depth.shape[0] / 256.0
    nz = np.ones_like(depth)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    n = np.stack([nx / length, ny / length, nz / length], axis=-1)
    return np.clip((n * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8)


def normalise(raw: F32, mask: NDArray[np.bool_]) -> F32:
    """Scales model output to 0..1 over the visible pixels only."""
    vis = raw[mask] if mask.any() else raw.ravel()
    lo, hi = float(np.percentile(vis, 1)), float(np.percentile(vis, 99))
    out: F32 = np.clip((raw - lo) / max(hi - lo, 1e-6), 0.0, 1.0).astype(np.float32)
    out[~mask] = 0.0
    return out


@dataclass(frozen=True)
class Job:
    rel: str  # the picture's path in the pack, e.g. "gfx/back1.png"
    source: Path
    depth: Path
    normal: Path


@dataclass
class Plan:
    total: int = 0  # pictures in the pack (after --only)
    jobs: list[Job] = field(default_factory=list)  # the ones still to do
    refused: list[str] = field(default_factory=list)  # names left out, with why


def plan(pack: Path, out: Path, only: Sequence[str] = (), force: bool = False) -> Plan:
    """The pictures to do, every path checked to stay inside its folder: a
    name that is not a plain relative path, or a symlink that leads outside
    the pack or the output folder, is refused."""
    files = sorted(f.relative_to(pack).as_posix() for f in pack.rglob("*.png"))
    if only:
        files = [f for f in files if any(fnmatch.fnmatch(f, pat) for pat in only)]
    result = Plan(total=len(files))
    for rel in files:
        try:
            job = Job(
                rel,
                hdpack.contained_path(pack, rel),
                hdpack.contained_path(out / "depth", rel),
                hdpack.contained_path(out / "normal", rel),
            )
        except hdpack.UnsafePathError as exc:
            result.refused.append(f"{rel}: {exc}")
            continue
        if force or not job.depth.exists():
            result.jobs.append(job)
    return result


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pack")
    p.add_argument("out")
    p.add_argument("--device", default="cuda:0")
    p.add_argument("--only", action="append", default=[], help="glob on pack paths, e.g. 'gfx/*'")
    p.add_argument("--force", action="store_true")
    p.add_argument("--max-side", type=int, default=1024, help="longest side fed to the model")
    args = p.parse_args(argv)

    work = plan(Path(args.pack), Path(args.out), args.only, args.force)
    for reason in work.refused:
        print("error:", reason, file=sys.stderr)
    todo = work.jobs
    print(f"{work.total} pictures, {len(todo)} to do, model {MODEL} on {args.device}", flush=True)
    if not todo:
        return 1 if work.refused else 0

    import torch
    from transformers import pipeline

    estimator = pipeline("depth-estimation", model=MODEL, device=args.device, torch_dtype=torch.float16)

    started = time.monotonic()
    for n, job in enumerate(todo, 1):
        rel = job.rel
        with Image.open(job.source) as im:
            rgba = im.convert("RGBA")
        w, h = rgba.size
        mask = np.asarray(rgba)[..., 3] > 8
        # The model sees the picture on neutral grey, at a sensible size.
        scale = min(1.0, args.max_side / max(w, h))
        small = rgba.resize((max(16, round(w * scale)), max(16, round(h * scale))), Image.Resampling.BOX)
        flat = Image.new("RGB", small.size, (128, 128, 128))
        flat.paste(small, mask=small.getchannel("A"))
        with torch.inference_mode():
            result = estimator(flat)
        raw = np.asarray(result["predicted_depth"].float().cpu().numpy(), dtype=np.float32)
        raw = np.squeeze(raw)
        depth_img = Image.fromarray(raw).resize((w, h), Image.Resampling.BICUBIC)
        depth = normalise(np.asarray(depth_img, dtype=np.float32), mask)
        job.depth.parent.mkdir(parents=True, exist_ok=True)
        job.normal.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray((depth * 65535.0 + 0.5).astype(np.uint16)).save(job.depth)
        normal = normals_from_depth(depth)
        normal[~mask] = (128, 128, 255)
        Image.fromarray(normal, "RGB").save(job.normal)
        if n % 25 == 0 or n == len(todo):
            rate = n / (time.monotonic() - started)
            print(f"[{n}/{len(todo)}] {rel} ({rate:.1f}/s)", flush=True)
    return 1 if work.refused else 0


if __name__ == "__main__":
    sys.exit(main())
