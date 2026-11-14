# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Depth and surface-normal maps for an HD pack, for 2.5D lighting.

For every picture in the pack this writes, under the output folder and with
the same relative path:

  depth/<file>.png    16-bit greyscale, white = near, transparent pixels 0
  normal/<file>.png   RGB normal map (tangent space, +Y up), from the depth
  _provenance/<file without .png>/provenance.json   model, licence, hashes

Depth comes from Depth Anything V2 (Hugging Face transformers) on the GPU.
The default is the Small model, the only Apache-2.0 one; Base and Large are
CC-BY-NC-4.0 and are refused unless --allow-noncommercial is given (maps
made with them must not ship in a commercial release). The maps are inputs
for the engine's lighting pass (lamps, oven glow, shadows, day and night);
the art itself is not changed.

  python depthmaps.py <pack> <out> [--model small] [--device cuda:0] [--only 'gfx/*']
"""

from __future__ import annotations

import argparse
import fnmatch
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray
from PIL import Image

# The sibling modules, found even under `python -I` (which leaves the
# script's folder off sys.path).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpack  # noqa: E402
import provenance  # noqa: E402

F32 = NDArray[np.float32]


@dataclass(frozen=True)
class DepthModel:
    key: str
    repo: str  # Hugging Face id (transformers route)
    comfy_ckpt: str  # comfyui_controlnet_aux file name (ComfyUI route)
    license: str
    commercial: bool


MODELS = {
    "small": DepthModel(
        "small", "depth-anything/Depth-Anything-V2-Small-hf", "depth_anything_v2_vits.pth", "Apache-2.0", True
    ),
    "base": DepthModel(
        "base", "depth-anything/Depth-Anything-V2-Base-hf", "depth_anything_v2_vitb.pth", "CC-BY-NC-4.0", False
    ),
    "large": DepthModel(
        "large", "depth-anything/Depth-Anything-V2-Large-hf", "depth_anything_v2_vitl.pth", "CC-BY-NC-4.0", False
    ),
}
DEFAULT_MODEL = "small"


class LicenceError(hdpack.HdPackError):
    """A model whose licence the user has not accepted for this run."""


def choose_model(key: str, allow_noncommercial: bool = False) -> DepthModel:
    if key not in MODELS:
        raise hdpack.HdPackError(f"unknown depth model {key!r}; use {', '.join(MODELS)}")
    m = MODELS[key]
    if not m.commercial and not allow_noncommercial:
        raise LicenceError(
            f"{m.repo} is {m.license} (non-commercial). Use --model small (Apache-2.0), or pass "
            "--allow-noncommercial for maps that will not ship commercially"
        )
    return m


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


def smooth_depth(
    depth: F32, mask: NDArray[np.bool_], radius: int = 3, sigma_space: float = 2.0, sigma_range: float = 0.08
) -> F32:
    """Edge-keeping (bilateral) smoothing of the visible pixels, so derived
    normals show the big planes and not the model's fine noise. radius 0 is
    no smoothing."""
    if radius <= 0:
        return depth
    h, w = depth.shape
    pad = np.pad(depth, radius, mode="edge")
    mpad = np.pad(mask.astype(np.float32), radius, mode="constant")
    acc = np.zeros_like(depth)
    wsum = np.zeros_like(depth)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            ws = np.exp(-(dx * dx + dy * dy) / (2.0 * sigma_space * sigma_space))
            nb = pad[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            nm = mpad[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            wr = np.exp(-((nb - depth) ** 2) / (2.0 * sigma_range * sigma_range)) * nm * ws
            acc += wr * nb
            wsum += wr
    out: F32 = np.where(wsum > 0, acc / np.maximum(wsum, 1e-12), depth).astype(np.float32)
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
    the pack or the output folder, is refused. A picture whose maps exist is
    skipped, unless its provenance sidecar says they have changed since."""
    files = sorted(f.relative_to(pack).as_posix() for f in pack.rglob("*.png"))
    files = [f for f in files if f.split("/", 1)[0] != provenance.FOLDER]
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
        stale = provenance.sidecar_path(out, rel).exists() and not provenance.matches(
            out, rel, [f"depth/{rel}", f"normal/{rel}"]
        )
        if force or stale or not job.depth.exists():
            result.jobs.append(job)
    return result


# A depth estimator: an RGB picture in, raw depth (any scale, larger = near) out.
Estimator = Callable[[Image.Image], F32]


def estimate(estimator: Estimator, rgba: Image.Image, max_side: int) -> tuple[F32, NDArray[np.bool_]]:
    """0..1 depth at the picture's size, and its visible-pixel mask."""
    w, h = rgba.size
    mask = np.asarray(rgba)[..., 3] > 8
    # The model sees the picture on neutral grey, at a sensible size.
    scale = min(1.0, max_side / max(w, h))
    small = rgba.resize((max(16, round(w * scale)), max(16, round(h * scale))), Image.Resampling.BOX)
    flat = Image.new("RGB", small.size, (128, 128, 128))
    flat.paste(small, mask=small.getchannel("A"))
    raw = np.squeeze(np.asarray(estimator(flat), dtype=np.float32))
    depth_img = Image.fromarray(raw).resize((w, h), Image.Resampling.BICUBIC)
    return normalise(np.asarray(depth_img, dtype=np.float32), mask), mask


def encode_maps(depth: F32, mask: NDArray[np.bool_], smooth: int, strength: float) -> tuple[bytes, bytes]:
    """PNG bytes of the 16-bit depth map and the RGB normal map."""
    depth16 = Image.fromarray((depth * 65535.0 + 0.5).astype(np.uint16))
    normal = normals_from_depth(smooth_depth(depth, mask, smooth), strength)
    normal[~mask] = (128, 128, 255)
    return hdpack.encode_png(depth16), hdpack.encode_png(Image.fromarray(normal, "RGB"))


def write_outputs(
    out: Path,
    rel: str,
    source: Path,
    depth_rel: str,
    normal_rel: str,
    data: tuple[bytes, bytes],
    model: DepthModel,
    revision: str | None,
    parameters: dict[str, Any],
) -> None:
    """Both maps, then their sidecar, each written whole."""
    for target, payload in ((depth_rel, data[0]), (normal_rel, data[1])):
        provenance.write_bytes_atomic(hdpack.contained_path(out, target), payload)
    provenance.write(
        out,
        rel,
        provenance.Record(
            stage="depth+normal",
            asset={"file": rel, "id": provenance.asset_id(rel)},
            input_file=rel,
            input_sha256=provenance.sha256_file(source),
            outputs={t: provenance.sha256_file(hdpack.contained_path(out, t)) for t in (depth_rel, normal_rel)},
            models=[provenance.Model("depth", model.repo, None, model.license, revision)],
            parameters=parameters,
        ),
    )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pack")
    p.add_argument("out")
    p.add_argument("--model", default=DEFAULT_MODEL, choices=sorted(MODELS), help="default small (Apache-2.0)")
    p.add_argument("--allow-noncommercial", action="store_true", help="accept the CC-BY-NC-4.0 base/large models")
    p.add_argument("--revision", default=None, help="Hugging Face commit of the model, recorded and pinned")
    p.add_argument("--device", default="cuda:0")
    p.add_argument("--only", action="append", default=[], help="glob on pack paths, e.g. 'gfx/*'")
    p.add_argument("--force", action="store_true")
    p.add_argument("--max-side", type=int, default=1024, help="longest side fed to the model")
    p.add_argument("--smooth", type=int, default=3, help="bilateral smoothing radius before normals (0 = off)")
    p.add_argument("--strength", type=float, default=6.0, help="normal map strength")
    args = p.parse_args(argv)

    try:
        model = choose_model(args.model, args.allow_noncommercial)
    except hdpack.HdPackError as exc:
        print("error:", exc, file=sys.stderr)
        return 2
    work = plan(Path(args.pack), Path(args.out), args.only, args.force)
    for reason in work.refused:
        print("error:", reason, file=sys.stderr)
    pending = work.jobs
    print(
        f"{work.total} pictures, {len(pending)} to do, model {model.repo} ({model.license}) on {args.device}",
        flush=True,
    )
    if not pending:
        return 1 if work.refused else 0

    import torch
    from transformers import pipeline

    pipe = pipeline(
        "depth-estimation", model=model.repo, revision=args.revision, device=args.device, torch_dtype=torch.float16
    )
    revision = args.revision or getattr(pipe.model.config, "_commit_hash", None)

    def run_model(img: Image.Image) -> F32:
        with torch.inference_mode():
            result = pipe(img)
        return np.asarray(result["predicted_depth"].float().cpu().numpy(), dtype=np.float32)

    out = Path(args.out)
    started = time.monotonic()
    for n, job in enumerate(pending, 1):
        with Image.open(job.source) as im:
            rgba = im.convert("RGBA")
        depth, mask = estimate(run_model, rgba, args.max_side)
        write_outputs(
            out,
            job.rel,
            job.source,
            f"depth/{job.rel}",
            f"normal/{job.rel}",
            encode_maps(depth, mask, args.smooth, args.strength),
            model,
            revision,
            {"device": args.device, "max_side": args.max_side, "smooth": args.smooth, "strength": args.strength},
        )
        if n % 25 == 0 or n == len(pending):
            rate = n / (time.monotonic() - started)
            print(f"[{n}/{len(pending)}] {job.rel} ({rate:.1f}/s)", flush=True)
    return 1 if work.refused else 0


if __name__ == "__main__":
    sys.exit(main())
