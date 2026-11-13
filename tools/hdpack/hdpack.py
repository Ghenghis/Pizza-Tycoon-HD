#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and check HD art packs for Pizza Legacy.

Workflow:

  1. Export the original art (the engine does this, with the right palettes):
       plegacy --game-path <folder with PT.EXE> --export-assets export/
  2. Make a pack, either
       * baseline, no AI:   hdpack.py upscale export/ pack/ --scale 6
       * with ComfyUI:      hdpack.py comfy export/ pack/ --workflow ...
  3. Check it:            hdpack.py validate export/ pack/
  4. Play it:             plegacy --hd-scale 3 --hd-pack pack/
                          (or put the pack in a folder called "hd" next to
                          the program and set hd_scale in the settings)

The pack layout mirrors the export layout: export/gfx/back1.png becomes
pack/gfx/back1.png, only bigger. Anything missing from the pack keeps the
original art, so a pack can be built up a few pictures at a time.
"""

from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import random
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageFilter

# Must match resources::HdPack::aspect_tolerance in the engine.
ASPECT_TOLERANCE = 0.02
MANIFEST_FORMAT = 1


class HdPackError(Exception):
    """A problem the user has to fix (bad input, unreachable server...)."""


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Asset:
    kind: str  # "vga" or "bob"
    key: str  # "gfx/back1", "bob/moebel"
    file: str  # path relative to the export / pack root
    width: int  # original pixels
    height: int
    frame: int | None
    opaque_pixels: int
    transparent_pixels: int

    @property
    def has_transparency(self) -> bool:
        return self.transparent_pixels > 0

    def target_size(self, scale: int) -> tuple[int, int]:
        return self.width * scale, self.height * scale


def load_manifest(export_dir: Path) -> list[Asset]:
    path = export_dir / "manifest.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise HdPackError(
            f"{path} not found. Export the original art first: " "plegacy --game-path <folder> --export-assets <dir>"
        ) from exc
    except json.JSONDecodeError as exc:
        raise HdPackError(f"{path} is not valid JSON: {exc}") from exc
    if data.get("format") != MANIFEST_FORMAT:
        raise HdPackError(f"{path}: unsupported manifest format {data.get('format')!r}")
    assets = []
    for item in data["images"]:
        assets.append(
            Asset(
                kind=item["kind"],
                key=item["key"],
                file=item["file"],
                width=int(item["width"]),
                height=int(item["height"]),
                frame=item.get("frame"),
                opaque_pixels=int(item["opaque_pixels"]),
                transparent_pixels=int(item["transparent_pixels"]),
            )
        )
    return assets


def select(assets: Iterable[Asset], patterns: Sequence[str]) -> list[Asset]:
    """Assets whose file path matches any glob (all when there are none)."""
    if not patterns:
        return list(assets)
    return [a for a in assets if any(fnmatch.fnmatch(a.file, p) for p in patterns)]


# --------------------------------------------------------------------------
# Image helpers
# --------------------------------------------------------------------------


def to_rgba(img: Image.Image) -> Image.Image:
    return img if img.mode == "RGBA" else img.convert("RGBA")


def premultiplied_resize(
    img: Image.Image, size: tuple[int, int], resample: int = Image.Resampling.LANCZOS
) -> Image.Image:
    """Resize RGBA without dark/colour fringes from transparent texels."""
    rgba = np.asarray(to_rgba(img), dtype=np.float32) / 255.0
    alpha = rgba[..., 3:4]
    premul = np.concatenate([rgba[..., :3] * alpha, alpha], axis=-1)
    channels = []
    for c in range(4):
        plane = Image.fromarray(premul[..., c], mode="F").resize(size, resample)
        channels.append(np.asarray(plane, dtype=np.float32))
    out = np.clip(np.stack(channels, axis=-1), 0.0, 1.0)
    a = out[..., 3:4]
    rgb = np.where(a > 1e-6, out[..., :3] / np.maximum(a, 1e-6), 0.0)
    result = np.concatenate([np.clip(rgb, 0, 1), a], axis=-1)
    return Image.fromarray((result * 255.0 + 0.5).astype(np.uint8), mode="RGBA")


def reference_mask(reference: Image.Image, size: tuple[int, int], feather: float) -> Image.Image:
    """The original's transparency, scaled up with soft edges."""
    alpha = to_rgba(reference).getchannel("A").resize(size, Image.Resampling.NEAREST)
    if feather > 0:
        alpha = alpha.filter(ImageFilter.GaussianBlur(feather))
    return alpha


def apply_reference_alpha(generated: Image.Image, reference: Image.Image, feather: float) -> Image.Image:
    """Give generated art the original sprite outline.

    Image models return opaque pictures; the game needs the original
    transparent areas back. Fully opaque originals stay fully opaque.
    """
    out = to_rgba(generated).copy()
    ref = to_rgba(reference)
    if ref.getchannel("A").getextrema()[0] == 255:
        out.putalpha(255)
        return out
    out.putalpha(reference_mask(ref, out.size, feather))
    return out


def fit_to_target(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Exact target size. Small aspect drift from a model is resized away."""
    if img.size == size:
        return to_rgba(img)
    return premultiplied_resize(img, size, Image.Resampling.LANCZOS)


# --------------------------------------------------------------------------
# Baseline pack (no AI)
# --------------------------------------------------------------------------


UPSCALE_METHODS = {
    "nearest": Image.Resampling.NEAREST,
    "lanczos": Image.Resampling.LANCZOS,
    "bicubic": Image.Resampling.BICUBIC,
}


@dataclass
class Counts:
    written: int = 0
    skipped: int = 0
    failed: int = 0
    errors: list[str] = field(default_factory=list)


def upscale_pack(
    export_dir: Path,
    pack_dir: Path,
    scale: int,
    method: str = "lanczos",
    patterns: Sequence[str] = (),
    force: bool = False,
) -> Counts:
    """Fill a pack with plainly upscaled originals.

    Useful as a smooth, artefact-free starting point and for checking the
    engine's HD path. Real remastered art comes from the comfy command.
    """
    if scale < 1:
        raise HdPackError("--scale must be at least 1")
    if method not in UPSCALE_METHODS:
        raise HdPackError(f"unknown method {method!r}; use {', '.join(UPSCALE_METHODS)}")
    counts = Counts()
    for asset in select(load_manifest(export_dir), patterns):
        dst = pack_dir / asset.file
        if dst.exists() and not force:
            counts.skipped += 1
            continue
        try:
            with Image.open(export_dir / asset.file) as src:
                size = asset.target_size(scale)
                if method == "nearest":
                    out = to_rgba(src).resize(size, Image.Resampling.NEAREST)
                else:
                    out = premultiplied_resize(src, size, UPSCALE_METHODS[method])
            dst.parent.mkdir(parents=True, exist_ok=True)
            out.save(dst)
            counts.written += 1
        except (OSError, ValueError) as exc:
            counts.failed += 1
            counts.errors.append(f"{asset.file}: {exc}")
    return counts


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------


@dataclass
class Report:
    total: int = 0
    present: int = 0
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def coverage(self) -> float:
        return self.present / self.total if self.total else 0.0

    def to_json(self) -> dict[str, Any]:
        return {
            "total": self.total,
            "present": self.present,
            "coverage": round(self.coverage, 4),
            "errors": self.errors,
            "warnings": self.warnings,
        }


def hole_fraction(asset_png: Image.Image, hd_png: Image.Image) -> float:
    """Share of the original's opaque pixels the HD art leaves (nearly) empty."""
    ref_alpha = np.asarray(to_rgba(asset_png).getchannel("A"))
    hd_alpha = to_rgba(hd_png).getchannel("A").resize((ref_alpha.shape[1], ref_alpha.shape[0]), Image.Resampling.BOX)
    hd_a = np.asarray(hd_alpha)
    opaque = ref_alpha > 0
    if not opaque.any():
        return 0.0
    return float(np.count_nonzero(hd_a[opaque] < 64)) / float(np.count_nonzero(opaque))


def validate_pack(export_dir: Path, pack_dir: Path, min_scale: int = 2, max_holes: float = 0.05) -> Report:
    """Check a pack against the export. Errors make the engine ignore a file."""
    if not pack_dir.is_dir():
        raise HdPackError(f"pack folder {pack_dir} does not exist")
    report = Report()
    assets = load_manifest(export_dir)
    known = {a.file for a in assets}
    report.total = len(assets)
    for asset in assets:
        hd_path = pack_dir / asset.file
        if not hd_path.exists():
            continue
        report.present += 1
        try:
            with Image.open(hd_path) as hd, Image.open(export_dir / asset.file) as ref:
                hd.load()
                want = asset.width / asset.height
                have = hd.width / hd.height
                if abs(have / want - 1.0) > ASPECT_TOLERANCE:
                    report.errors.append(
                        f"{asset.file}: {hd.width}x{hd.height} does not keep the "
                        f"original {asset.width}x{asset.height} aspect ratio"
                    )
                    continue
                scale = hd.width / asset.width
                if scale < min_scale:
                    report.warnings.append(f"{asset.file}: only {scale:.1f}x the original (want {min_scale}x+)")
                if asset.has_transparency or "A" in hd.getbands():
                    holes = hole_fraction(ref, hd)
                    if holes > max_holes:
                        report.errors.append(
                            f"{asset.file}: {holes:.0%} of the original's visible "
                            "pixels are transparent in the HD art"
                        )
        except OSError as exc:
            report.errors.append(f"{asset.file}: cannot read ({exc})")
    for extra in sorted(pack_dir.rglob("*.png")):
        rel = extra.relative_to(pack_dir).as_posix()
        if rel not in known:
            report.warnings.append(f"{rel}: not an original picture, the engine ignores it")
    return report


# --------------------------------------------------------------------------
# ComfyUI
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class PromptRule:
    match: str
    positive: str
    negative: str
    denoise: float


@dataclass(frozen=True)
class PromptConfig:
    style: str
    negative: str
    default_positive: str
    default_denoise: float
    rules: tuple[PromptRule, ...]

    def for_asset(self, asset: Asset) -> PromptRule:
        for rule in self.rules:
            if fnmatch.fnmatch(asset.file, rule.match):
                return rule
        return PromptRule("*", self.default_positive, "", self.default_denoise)

    def positive(self, asset: Asset) -> str:
        rule = self.for_asset(asset)
        return ", ".join(p for p in (rule.positive, self.style) if p)

    def negative_for(self, asset: Asset) -> str:
        rule = self.for_asset(asset)
        return ", ".join(p for p in (rule.negative, self.negative) if p)


def load_prompts(path: Path) -> PromptConfig:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HdPackError(f"cannot read prompts {path}: {exc}") from exc
    default = data.get("default", {})
    rules = tuple(
        PromptRule(
            match=r["match"],
            positive=r.get("positive", ""),
            negative=r.get("negative", ""),
            denoise=float(r.get("denoise", default.get("denoise", 0.35))),
        )
        for r in data.get("rules", [])
    )
    return PromptConfig(
        style=data.get("style", ""),
        negative=data.get("negative", ""),
        default_positive=default.get("positive", ""),
        default_denoise=float(default.get("denoise", 0.35)),
        rules=rules,
    )


def fill_template(template: Any, values: dict[str, Any]) -> Any:
    """Replace "{{name}}" placeholders in a ComfyUI API-format workflow.

    A string that is exactly one placeholder takes the value's own type, so
    "{{seed}}" becomes an int and "{{denoise}}" a float. Unknown names are an
    error, so a typo cannot silently send a literal "{{...}}" to ComfyUI.
    """
    if isinstance(template, dict):
        return {k: fill_template(v, values) for k, v in template.items()}
    if isinstance(template, list):
        return [fill_template(v, values) for v in template]
    if isinstance(template, str) and "{{" in template:
        stripped = template.strip()
        if stripped.startswith("{{") and stripped.endswith("}}") and stripped.count("{{") == 1:
            name = stripped[2:-2].strip()
            if name not in values:
                raise HdPackError(f"workflow uses unknown placeholder {{{{{name}}}}}")
            return values[name]
        out = template
        for name, value in values.items():
            out = out.replace("{{" + name + "}}", str(value))
        if "{{" in out:
            raise HdPackError(f"workflow has an unknown placeholder in {template!r}")
        return out
    return template


class ComfyClient:
    """The few ComfyUI HTTP endpoints the batch needs."""

    def __init__(self, server: str, timeout: float = 30.0) -> None:
        if urllib.parse.urlsplit(server).scheme not in ("http", "https"):
            raise HdPackError(f"ComfyUI server must be an http(s) URL, not {server!r}")
        self.server = server.rstrip("/")
        self.timeout = timeout
        self.client_id = uuid.uuid4().hex

    def _request(self, path: str, data: bytes | None = None, headers: dict[str, str] | None = None) -> bytes:
        # The scheme was checked in __init__.
        req = urllib.request.Request(self.server + path, data=data, headers=headers or {})  # noqa: S310
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:  # noqa: S310
                return bytes(resp.read())
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", "replace")[:500]
            raise HdPackError(f"ComfyUI {path} failed: HTTP {exc.code}: {body}") from exc
        except (urllib.error.URLError, TimeoutError) as exc:
            raise HdPackError(f"cannot reach ComfyUI at {self.server} ({exc}). Is it running?") from exc

    def _json(self, path: str, data: bytes | None = None, headers: dict[str, str] | None = None) -> dict[str, Any]:
        result = json.loads(self._request(path, data, headers))
        if not isinstance(result, dict):
            raise HdPackError(f"ComfyUI {path} returned unexpected data")
        return result

    def system_stats(self) -> dict[str, Any]:
        return self._json("/system_stats")

    def node_info(self, node: str) -> dict[str, Any]:
        return self._json("/object_info/" + urllib.parse.quote(node))

    def upload_image(self, path: Path, name: str) -> str:
        boundary = uuid.uuid4().hex
        payload = path.read_bytes()
        body = (
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="image"; filename="{name}"\r\n'
                "Content-Type: image/png\r\n\r\n"
            ).encode()
            + payload
            + (
                f"\r\n--{boundary}\r\n"
                'Content-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n'
                f"--{boundary}--\r\n"
            ).encode()
        )
        result = self._json(
            "/upload/image",
            body,
            {"Content-Type": f"multipart/form-data; boundary={boundary}"},
        )
        sub = result.get("subfolder", "")
        return f"{sub}/{result['name']}" if sub else result["name"]

    def queue(self, prompt: dict[str, Any]) -> str:
        body = json.dumps({"prompt": prompt, "client_id": self.client_id}).encode()
        result = self._json("/prompt", body, {"Content-Type": "application/json"})
        if "prompt_id" not in result:
            raise HdPackError(f"ComfyUI refused the workflow: {result}")
        return str(result["prompt_id"])

    def wait(self, prompt_id: str, timeout: float, poll: float = 1.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            entry = self._json("/history/" + prompt_id).get(prompt_id)
            if isinstance(entry, dict):
                status = entry.get("status", {})
                if status.get("status_str") == "error":
                    raise HdPackError(f"ComfyUI job {prompt_id} failed: {status.get('messages')}")
                if status.get("completed", True) and entry.get("outputs"):
                    return entry
            time.sleep(poll)
        raise HdPackError(f"ComfyUI job {prompt_id} did not finish in {timeout:.0f}s")

    def first_image(self, entry: dict[str, Any]) -> bytes:
        for output in entry.get("outputs", {}).values():
            for img in output.get("images", []):
                query = urllib.parse.urlencode(
                    {
                        "filename": img["filename"],
                        "subfolder": img.get("subfolder", ""),
                        "type": img.get("type", "output"),
                    }
                )
                return self._request("/view?" + query)
        raise HdPackError("ComfyUI job finished without an image output")


@dataclass
class ComfySettings:
    workflow: dict[str, Any]
    prompts: PromptConfig
    scale: int
    checkpoint: str
    upscale_model: str
    seed: int | None
    feather: float
    job_timeout: float


# Diffusion models make poor pictures far below their training size, so small
# sprites are generated big and scaled down to the target afterwards.
MIN_GENERATION_SIDE = 1024
MAX_GENERATION_SIDE = 2048


def generation_size(width: int, height: int) -> tuple[int, int]:
    """Size to sample at: long side within the model's range, multiples of 8."""
    long_side = max(width, height)
    factor = 1.0
    if long_side < MIN_GENERATION_SIDE:
        factor = MIN_GENERATION_SIDE / long_side
    elif long_side > MAX_GENERATION_SIDE:
        factor = MAX_GENERATION_SIDE / long_side

    def round8(v: float) -> int:
        return max(64, int(round(v / 8.0)) * 8)

    return round8(width * factor), round8(height * factor)


def comfy_values(asset: Asset, settings: ComfySettings, uploaded: str, seed: int) -> dict[str, Any]:
    width, height = asset.target_size(settings.scale)
    gen_width, gen_height = generation_size(width, height)
    rule = settings.prompts.for_asset(asset)
    return {
        "gen_width": gen_width,
        "gen_height": gen_height,
        "input_image": uploaded,
        "positive": settings.prompts.positive(asset),
        "negative": settings.prompts.negative_for(asset),
        "denoise": rule.denoise,
        "seed": seed,
        "width": width,
        "height": height,
        "checkpoint": settings.checkpoint,
        "upscale_model": settings.upscale_model,
        "prefix": "plegacy_hd/" + asset.file.removesuffix(".png").replace("/", "_"),
    }


def comfy_pack(
    client: ComfyClient,
    export_dir: Path,
    pack_dir: Path,
    settings: ComfySettings,
    patterns: Sequence[str] = (),
    limit: int | None = None,
    force: bool = False,
    dry_run: bool = False,
    log: Any = print,
) -> Counts:
    counts = Counts()
    assets = select(load_manifest(export_dir), patterns)
    todo = [a for a in assets if force or not (pack_dir / a.file).exists()]
    counts.skipped = len(assets) - len(todo)
    if limit is not None:
        todo = todo[:limit]
    rng = random.Random(settings.seed)
    for n, asset in enumerate(todo, 1):
        seed = rng.randrange(2**31)
        if dry_run:
            values = comfy_values(asset, settings, "<upload>", seed)
            fill_template(settings.workflow, values)  # catches placeholder errors
            log(f"[{n}/{len(todo)}] would generate {asset.file} at {values['width']}x{values['height']}")
            continue
        try:
            started = time.monotonic()
            uploaded = client.upload_image(export_dir / asset.file, asset.file.replace("/", "_"))
            prompt = fill_template(copy.deepcopy(settings.workflow), comfy_values(asset, settings, uploaded, seed))
            entry = client.wait(client.queue(prompt), settings.job_timeout)
            data = client.first_image(entry)
            from io import BytesIO

            with Image.open(BytesIO(data)) as generated, Image.open(export_dir / asset.file) as ref:
                out = fit_to_target(generated, asset.target_size(settings.scale))
                out = apply_reference_alpha(out, ref, settings.feather)
            dst = pack_dir / asset.file
            dst.parent.mkdir(parents=True, exist_ok=True)
            out.save(dst)
            counts.written += 1
            log(f"[{n}/{len(todo)}] {asset.file} done in {time.monotonic() - started:.1f}s")
        except HdPackError as exc:
            counts.failed += 1
            counts.errors.append(f"{asset.file}: {exc}")
            log(f"[{n}/{len(todo)}] {asset.file} FAILED: {exc}")
            if "cannot reach ComfyUI" in str(exc):
                break
    return counts


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

HERE = Path(__file__).resolve().parent


def _print_counts(counts: Counts) -> int:
    print(f"written {counts.written}, skipped {counts.skipped} (already in pack), failed {counts.failed}")
    for e in counts.errors:
        print("error:", e, file=sys.stderr)
    return 1 if counts.failed else 0


def cmd_upscale(args: argparse.Namespace) -> int:
    return _print_counts(
        upscale_pack(Path(args.export), Path(args.pack), args.scale, args.method, args.only, args.force)
    )


def cmd_validate(args: argparse.Namespace) -> int:
    report = validate_pack(Path(args.export), Path(args.pack), args.min_scale, args.max_holes)
    if args.json:
        print(json.dumps(report.to_json(), indent=1))
    else:
        print(f"{report.present}/{report.total} pictures replaced ({report.coverage:.1%})")
        for w in report.warnings:
            print("warning:", w)
        for e in report.errors:
            print("error:", e)
        print(f"{len(report.errors)} errors, {len(report.warnings)} warnings")
    return 1 if report.errors else 0


def cmd_doctor(args: argparse.Namespace) -> int:
    client = ComfyClient(args.server, timeout=10)
    ok = True
    try:
        stats = client.system_stats()
    except HdPackError as exc:
        print("FAIL", exc)
        return 1
    devices = stats.get("devices", [])
    print("PASS ComfyUI", stats.get("system", {}).get("comfyui_version", "?"), "at", args.server)
    for d in devices:
        vram = d.get("vram_total", 0) / 2**30
        print(f"PASS device {d.get('name')} ({vram:.1f} GiB VRAM)")
    if not any(d.get("type") == "cuda" for d in devices):
        print("WARN no CUDA device reported; generation will be very slow")
    workflow = json.loads(Path(args.workflow).read_text(encoding="utf-8"))
    for node in sorted({n["class_type"] for n in workflow.values()}):
        try:
            client.node_info(node)
            print("PASS node", node)
        except HdPackError:
            print("FAIL node", node, "is not installed in ComfyUI")
            ok = False
    for loader, field_name, wanted in (
        ("CheckpointLoaderSimple", "ckpt_name", args.checkpoint),
        ("UpscaleModelLoader", "model_name", args.upscale_model),
    ):
        if not wanted or loader not in {n["class_type"] for n in workflow.values()}:
            continue
        info = client.node_info(loader).get(loader, {})
        choices = info.get("input", {}).get("required", {}).get(field_name, [[]])[0]
        if wanted in choices:
            print(f"PASS {field_name} {wanted}")
        else:
            print(f"FAIL {field_name} {wanted!r} not found; ComfyUI has: {', '.join(choices) or 'none'}")
            ok = False
    return 0 if ok else 1


def cmd_comfy(args: argparse.Namespace) -> int:
    settings = ComfySettings(
        workflow=json.loads(Path(args.workflow).read_text(encoding="utf-8")),
        prompts=load_prompts(Path(args.prompts)),
        scale=args.scale,
        checkpoint=args.checkpoint,
        upscale_model=args.upscale_model,
        seed=args.seed,
        feather=args.feather,
        job_timeout=args.job_timeout,
    )
    client = ComfyClient(args.server)
    return _print_counts(
        comfy_pack(
            client,
            Path(args.export),
            Path(args.pack),
            settings,
            args.only,
            args.limit,
            args.force,
            args.dry_run,
        )
    )


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="hdpack", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)

    def common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("export", help="folder written by plegacy --export-assets")
        sp.add_argument("pack", help="HD pack folder (created if needed)")

    up = sub.add_parser("upscale", help="baseline pack: plain upscaling, no AI")
    common(up)
    up.add_argument("--scale", type=int, default=6, help="times the original size (default 6)")
    up.add_argument("--method", default="lanczos", choices=sorted(UPSCALE_METHODS))
    up.add_argument("--only", action="append", default=[], help="glob on file paths, e.g. 'gfx/back*'")
    up.add_argument("--force", action="store_true", help="overwrite files already in the pack")
    up.set_defaults(func=cmd_upscale)

    va = sub.add_parser("validate", help="check a pack the way the engine will")
    common(va)
    va.add_argument("--min-scale", type=int, default=2)
    va.add_argument("--max-holes", type=float, default=0.05, help="allowed share of see-through sprite pixels")
    va.add_argument("--json", action="store_true")
    va.set_defaults(func=cmd_validate)

    defaults = {
        "server": "http://127.0.0.1:8188",
        "workflow": str(HERE / "workflows" / "sdxl_refine_api.json"),
        "prompts": str(HERE / "prompts.json"),
    }
    do = sub.add_parser("doctor", help="check ComfyUI, its GPU, and the workflow's nodes and models")
    do.add_argument("--server", default=defaults["server"])
    do.add_argument("--workflow", default=defaults["workflow"])
    do.add_argument("--checkpoint", default="")
    do.add_argument("--upscale-model", default="")
    do.set_defaults(func=cmd_doctor)

    co = sub.add_parser("comfy", help="generate HD art with a local ComfyUI")
    common(co)
    co.add_argument("--server", default=defaults["server"])
    co.add_argument(
        "--workflow", default=defaults["workflow"], help="ComfyUI workflow in API format with {{placeholders}}"
    )
    co.add_argument("--prompts", default=defaults["prompts"])
    co.add_argument("--checkpoint", default="", help="checkpoint file name as ComfyUI lists it")
    co.add_argument("--upscale-model", default="", help="upscale model file name as ComfyUI lists it")
    co.add_argument("--scale", type=int, default=6, help="output size, times the original (default 6)")
    co.add_argument("--seed", type=int, default=None)
    co.add_argument("--feather", type=float, default=1.5, help="softness of sprite edges in output pixels")
    co.add_argument("--job-timeout", type=float, default=900.0)
    co.add_argument("--only", action="append", default=[])
    co.add_argument("--limit", type=int, default=None)
    co.add_argument("--force", action="store_true")
    co.add_argument("--dry-run", action="store_true")
    co.set_defaults(func=cmd_comfy)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except HdPackError as exc:
        print("error:", exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
