# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The depth and normals stage: per-picture maps from the HD art.

Layout (docs/studio/notes/hdpack-maps-layout.md), one bundle per pack picture,
named by its asset id (the pack path without .png):

  <maps>/<asset id>/depth_16.png   16-bit greyscale, near = bright, visible pixels 0..65535
  <maps>/<asset id>/normal.png     RGB tangent-space normals, +X right, +Y up, +Z out
  <maps>/_provenance/<asset id>/provenance.json

  python maps.py plan <pack> <maps> [--only 'bob/*'] [--json]       (no GPU)
  python maps.py run  <pack> <maps> --grant-file <grant.json>
        [--route transformers|comfy] [--model small]

The run uses a GPU, so it needs a broker grant: a JSON file with
"grant_id", "scope": "hdpack-maps", "gpu" (the device index) and
"not_after_utc" (ISO time). Without one, or past its time, or for another
scope, the run refuses before loading a model or contacting ComfyUI.
Depth is Depth Anything V2 Small (Apache-2.0) unless --model base/large
and --allow-noncommercial are both given. Normals come from the depth,
smoothed (bilateral) so the big planes show, not the model's noise.
"""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import fnmatch
import json
import os
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from io import BytesIO
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import depthmaps  # noqa: E402
import hdpack  # noqa: E402
import provenance  # noqa: E402

GRANT_SCOPE = "hdpack-maps"
GRANT_ENV = "PLEGACY_GPU_GRANT_FILE"
HERE = Path(__file__).resolve().parent
DEFAULT_WORKFLOW = HERE / "workflows" / "depth_anything_v2_api.json"
DEPTH_NAME = "depth_16.png"
NORMAL_NAME = "normal.png"


class GrantError(hdpack.HdPackError):
    """No usable GPU grant for this run."""


@dataclass(frozen=True)
class Grant:
    grant_id: str
    scope: str
    gpu: int
    not_after: dt.datetime


def load_grant(path: Path | None, now: dt.datetime | None = None) -> Grant:
    if path is None:
        raise GrantError(f"this stage runs on a GPU and needs a broker grant: pass --grant-file or set {GRANT_ENV}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        grant = Grant(
            str(data["grant_id"]),
            str(data["scope"]),
            int(data["gpu"]),
            dt.datetime.fromisoformat(str(data["not_after_utc"]).replace("Z", "+00:00")),
        )
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise GrantError(f"{path}: not a usable grant ({exc})") from exc
    if grant.not_after.tzinfo is None:
        raise GrantError(f"{path}: not_after_utc must carry a time zone")
    if grant.scope != GRANT_SCOPE:
        raise GrantError(f"{path}: grant {grant.grant_id} is for {grant.scope!r}, not {GRANT_SCOPE!r}")
    if (now or dt.datetime.now(dt.timezone.utc)) > grant.not_after:
        raise GrantError(f"{path}: grant {grant.grant_id} ended at {grant.not_after.isoformat()}")
    return grant


@dataclass(frozen=True)
class Bundle:
    rel: str  # pack path, e.g. "gfx/back1.png"
    source: Path
    depth_rel: str  # relative to the maps folder
    normal_rel: str


@dataclass
class MapsPlan:
    total: int = 0
    bundles: list[Bundle] = field(default_factory=list)
    refused: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {
            "total": self.total,
            "todo": [{"file": b.rel, "depth": b.depth_rel, "normal": b.normal_rel} for b in self.bundles],
            "refused": self.refused,
        }


def plan(pack: Path, maps: Path, only: Sequence[str] = (), force: bool = False) -> MapsPlan:
    """Bundles still to make, every path checked to stay inside its folder.
    A bundle is done when both maps exist and its sidecar (if any) still
    names them as they are."""
    files = sorted(f.relative_to(pack).as_posix() for f in pack.rglob("*.png"))
    files = [f for f in files if f.split("/", 1)[0] != provenance.FOLDER]
    if only:
        files = [f for f in files if any(fnmatch.fnmatch(f, pat) for pat in only)]
    result = MapsPlan(total=len(files))
    for rel in files:
        aid = provenance.asset_id(rel)
        try:
            source = hdpack.contained_path(pack, rel)
            depth = hdpack.contained_path(maps, f"{aid}/{DEPTH_NAME}")
            normal = hdpack.contained_path(maps, f"{aid}/{NORMAL_NAME}")
        except hdpack.UnsafePathError as exc:
            result.refused.append(f"{rel}: {exc}")
            continue
        bundle = Bundle(rel, source, f"{aid}/{DEPTH_NAME}", f"{aid}/{NORMAL_NAME}")
        exists = depth.exists() and normal.exists()
        has_side = provenance.sidecar_path(maps, rel).exists()
        stale = has_side and not provenance.matches(maps, rel, [bundle.depth_rel, bundle.normal_rel])
        if force or not exists or stale:
            result.bundles.append(bundle)
    return result


def run_bundles(
    maps: Path,
    work: MapsPlan,
    estimator: depthmaps.Estimator,
    model: depthmaps.DepthModel,
    revision: str | None,
    parameters: dict[str, Any],
    max_side: int = 1024,
    smooth: int = 3,
    strength: float = 6.0,
    log: Any = print,
) -> int:
    """Make every planned bundle with estimator; returns how many failed."""
    failed = 0
    for n, b in enumerate(work.bundles, 1):
        try:
            with Image.open(b.source) as im:
                rgba = im.convert("RGBA")
            depth, mask = depthmaps.estimate(estimator, rgba, max_side)
            depthmaps.write_outputs(
                maps,
                b.rel,
                b.source,
                b.depth_rel,
                b.normal_rel,
                depthmaps.encode_maps(depth, mask, smooth, strength),
                model,
                revision,
                parameters | {"max_side": max_side, "smooth": smooth, "strength": strength},
            )
            log(f"[{n}/{len(work.bundles)}] {b.rel}")
        except (OSError, ValueError, hdpack.HdPackError, provenance.ProvenanceError) as exc:
            failed += 1
            log(f"[{n}/{len(work.bundles)}] {b.rel} FAILED: {exc}")
    return failed


def comfy_estimator(
    client: hdpack.ComfyClient, workflow: dict[str, Any], model: depthmaps.DepthModel, resolution: int, timeout: float
) -> depthmaps.Estimator:
    """Depth through ComfyUI (comfyui_controlnet_aux's DepthAnythingV2Preprocessor).
    Its SaveImage output is 8-bit, so this route's depth has 256 levels; the
    sidecar says so. Check the server first with: hdpack.py doctor --workflow."""

    def estimate(img: Image.Image) -> depthmaps.F32:
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d) / "input.png"
            img.save(tmp, "PNG")
            uploaded = client.upload_image(tmp, "plegacy_maps_input.png")
        values = {
            "input_image": uploaded,
            "depth_model": model.comfy_ckpt,
            "resolution": resolution,
            "prefix": "plegacy_maps/depth",
        }
        prompt = hdpack.fill_template(copy.deepcopy(workflow), values)
        data = client.first_image(client.wait(client.queue(prompt), timeout))
        with Image.open(BytesIO(data)) as out:
            grey = np.asarray(out.convert("L"), dtype=np.float32) / 255.0
        return grey

    return estimate


def cmd_plan(args: argparse.Namespace) -> int:
    work = plan(Path(args.pack), Path(args.maps), args.only, args.force)
    if args.json:
        print(json.dumps(work.to_json(), indent=1))
    else:
        for r in work.refused:
            print("error:", r)
        print(f"{work.total} pictures, {len(work.bundles)} bundles to make, {len(work.refused)} refused")
    return 1 if work.refused else 0


def cmd_run(args: argparse.Namespace) -> int:
    grant_file = args.grant_file or os.environ.get(GRANT_ENV)
    grant = load_grant(Path(grant_file) if grant_file else None)
    model = depthmaps.choose_model(args.model, args.allow_noncommercial)
    work = plan(Path(args.pack), Path(args.maps), args.only, args.force)
    for r in work.refused:
        print("error:", r, file=sys.stderr)
    print(f"{len(work.bundles)} bundles, {model.repo} ({model.license}), route {args.route}, grant {grant.grant_id}")
    if not work.bundles:
        return 1 if work.refused else 0
    params: dict[str, Any] = {"route": args.route, "grant_id": grant.grant_id, "gpu": grant.gpu}
    if args.route == "comfy":
        workflow = json.loads(Path(args.workflow).read_text(encoding="utf-8"))
        client = hdpack.ComfyClient(args.server)
        estimator = comfy_estimator(client, workflow, model, args.max_side, args.job_timeout)
        params |= {
            "workflow_id": provenance.store_workflow(Path(args.maps), workflow),
            "depth_bits": 8,
            "server": client.server,
        }
        revision = None
    else:
        import torch
        from transformers import pipeline

        pipe = pipeline(
            "depth-estimation",
            model=model.repo,
            revision=args.revision,
            device=f"cuda:{grant.gpu}",
            torch_dtype=torch.float16,
        )
        revision = args.revision or getattr(pipe.model.config, "_commit_hash", None)

        def estimator(img: Image.Image) -> depthmaps.F32:
            with torch.inference_mode():
                result = pipe(img)
            return np.asarray(result["predicted_depth"].float().cpu().numpy(), dtype=np.float32)

        params |= {"depth_bits": 16}
    failed = run_bundles(
        Path(args.maps), work, estimator, model, revision, params, args.max_side, args.smooth, args.strength
    )
    return 1 if failed or work.refused else 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="maps", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    def common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("pack", help="HD pack folder (the art the maps are made from)")
        sp.add_argument("maps", help="output folder for the map bundles")
        sp.add_argument("--only", action="append", default=[], help="glob on pack paths, e.g. 'bob/*'")
        sp.add_argument("--force", action="store_true")

    pl = sub.add_parser("plan", help="list the bundles still to make (no GPU)")
    common(pl)
    pl.add_argument("--json", action="store_true")
    pl.set_defaults(func=cmd_plan)

    ru = sub.add_parser("run", help="make the bundles (needs a GPU broker grant)")
    common(ru)
    ru.add_argument("--grant-file", default=None, help=f"broker grant JSON (or set {GRANT_ENV})")
    ru.add_argument("--route", choices=("transformers", "comfy"), default="transformers")
    ru.add_argument("--model", choices=sorted(depthmaps.MODELS), default=depthmaps.DEFAULT_MODEL)
    ru.add_argument("--allow-noncommercial", action="store_true")
    ru.add_argument("--revision", default=None, help="Hugging Face commit to pin (transformers route)")
    ru.add_argument("--server", default="http://127.0.0.1:8188")
    ru.add_argument("--workflow", default=str(DEFAULT_WORKFLOW))
    ru.add_argument("--job-timeout", type=float, default=300.0)
    ru.add_argument("--max-side", type=int, default=1024)
    ru.add_argument("--smooth", type=int, default=3)
    ru.add_argument("--strength", type=float, default=6.0)
    ru.set_defaults(func=cmd_run)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except hdpack.HdPackError as exc:
        print("error:", exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
