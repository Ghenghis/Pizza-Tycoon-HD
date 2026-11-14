# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Redraw sprites as new HD art with SDXL, guided by their depth maps.

For each sprite: its HD pack picture (the pixel-art upscale) is the starting
image, its depth map (depthmaps.py) drives a depth ControlNet so the shape
and perspective stay, and a prompt describes the object. The result keeps
the original sprite's exact outline (its alpha), so placement, click areas
and gameplay are unchanged; only the paint inside is new.

  python redraw.py <pack> <maps> <out> --server http://127.0.0.1:8189 \\
      --item bob/moebel/0004.png "ornate red velvet dining chair" ...

Writes <out>/<denoise>/<file> for every --denoise value, ready to copy into a
pack, and <out>/sheet.png comparing them with the original.
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpack  # noqa: E402

STYLE = (
    "seen from above at a slight angle, 1990s pizza restaurant game asset, "
    "hand-painted cartoon art, rich warm colours, soft studio lighting, "
    "clean shapes, crisp details, isolated object on a plain grey background"
)
NEGATIVE = (
    "photo, photorealistic, text, letters, watermark, signature, frame, border, "
    "blurry, noisy, jpeg artifacts, deformed, extra objects, people, cropped"
)


def workflow(values: dict[str, Any]) -> dict[str, Any]:
    return {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": values["checkpoint"]}},
        "2": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["1", 1], "text": values["positive"]}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["1", 1], "text": values["negative"]}},
        "4": {"class_type": "LoadImage", "inputs": {"image": values["input_image"]}},
        "5": {"class_type": "LoadImage", "inputs": {"image": values["depth_image"]}},
        "6": {"class_type": "VAEEncode", "inputs": {"pixels": ["4", 0], "vae": ["1", 2]}},
        "7": {"class_type": "ControlNetLoader", "inputs": {"control_net_name": values["controlnet"]}},
        "8": {
            "class_type": "ControlNetApplyAdvanced",
            "inputs": {
                "positive": ["2", 0],
                "negative": ["3", 0],
                "control_net": ["7", 0],
                "image": ["5", 0],
                "strength": values["control_strength"],
                "start_percent": 0.0,
                "end_percent": 0.9,
                "vae": ["1", 2],
            },
        },
        "9": {
            "class_type": "KSampler",
            "inputs": {
                "model": ["1", 0],
                "positive": ["8", 0],
                "negative": ["8", 1],
                "latent_image": ["6", 0],
                "seed": values["seed"],
                "steps": 30,
                "cfg": 6.0,
                "sampler_name": "dpmpp_2m",
                "scheduler": "karras",
                "denoise": values["denoise"],
            },
        },
        "10": {"class_type": "VAEDecode", "inputs": {"samples": ["9", 0], "vae": ["1", 2]}},
        "11": {"class_type": "SaveImage", "inputs": {"images": ["10", 0], "filename_prefix": "pthd_redraw"}},
    }


def model_size(w: int, h: int, long_side: int = 1024) -> tuple[int, int]:
    """SDXL-friendly size with the sprite's aspect, multiples of 8."""
    s = long_side / max(w, h)
    return max(64, round(w * s / 8) * 8), max(64, round(h * s / 8) * 8)


def prepare(sprite: Image.Image, depth: Image.Image, size: tuple[int, int]) -> tuple[Image.Image, Image.Image]:
    flat = Image.new("RGB", sprite.size, (128, 128, 128))
    flat.paste(sprite, mask=sprite.getchannel("A"))
    d = np.asarray(depth, dtype=np.float32)
    d8 = (d / max(float(d.max()), 1.0) * 255.0).astype(np.uint8)
    depth_rgb = Image.fromarray(d8).convert("RGB")
    return flat.resize(size, Image.Resampling.LANCZOS), depth_rgb.resize(size, Image.Resampling.BICUBIC)


def shadow_mask(sprite: Image.Image) -> NDArray[np.float32]:
    """1 where the sprite shows its baked-in drop shadow (dark, grey), softened
    over a couple of pixels; those pixels are kept as they are."""
    a = np.asarray(sprite.convert("RGBA")).astype(np.int16)
    rgb = a[..., :3]
    grey = (rgb.max(axis=-1) - rgb.min(axis=-1)) < 14
    dark = rgb.max(axis=-1) < 80
    m = Image.fromarray(((grey & dark & (a[..., 3] > 0)) * 255).astype(np.uint8))
    radius = max(1, round(min(sprite.size) / 150))
    m = m.filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(m, dtype=np.float32) / 255.0


def finish(generated: Image.Image, sprite: Image.Image) -> Image.Image:
    """Back to the sprite's size, inside the sprite's own outline, with the
    sprite's own drop shadow kept."""
    out = generated.convert("RGB").resize(sprite.size, Image.Resampling.LANCZOS)
    m = shadow_mask(sprite)[..., None]
    mixed = np.asarray(out, dtype=np.float32) * (1 - m) + np.asarray(sprite.convert("RGB"), dtype=np.float32) * m
    result = Image.fromarray(np.clip(mixed + 0.5, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    result.putalpha(sprite.getchannel("A"))
    return result


def sheet(rows: list[tuple[str, list[Image.Image]]], labels: list[str], path: Path) -> None:
    cell = 300
    pad = 10
    w = pad + len(labels) * (cell + pad)
    h = 30 + len(rows) * (cell + pad + 20)
    img = Image.new("RGB", (w, h), (32, 32, 32))
    draw = ImageDraw.Draw(img)
    for c, label in enumerate(labels):
        draw.text((pad + c * (cell + pad), 8), label, fill=(230, 230, 230))
    for r, (name, pics) in enumerate(rows):
        y = 30 + r * (cell + pad + 20)
        draw.text((pad, y), name, fill=(200, 200, 200))
        for c, pic in enumerate(pics):
            s = min(cell / pic.width, cell / pic.height)
            p = pic.resize((max(1, round(pic.width * s)), max(1, round(pic.height * s))), Image.Resampling.LANCZOS)
            bg = Image.new("RGBA", p.size, (210, 190, 160, 255))
            bg.alpha_composite(p)
            img.paste(bg.convert("RGB"), (pad + c * (cell + pad), y + 16))
    img.save(path)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pack")
    p.add_argument("maps", help="folder from depthmaps.py (has depth/)")
    p.add_argument("out")
    p.add_argument("--server", default="http://127.0.0.1:8188")
    p.add_argument("--item", nargs=2, action="append", default=[], metavar=("FILE", "DESCRIPTION"))
    p.add_argument(
        "--items-json",
        help='list of {"file", "description", "seed"}; frames of one object share a seed so its views match',
    )
    p.add_argument("--sheet-limit", type=int, default=12, help="rows on the comparison sheet")
    p.add_argument("--skip-done", action="store_true", help="skip files already in <out>/<denoise>/")
    p.add_argument("--denoise", type=float, nargs="+", default=[0.55, 0.7])
    p.add_argument("--control-strength", type=float, default=0.85)
    p.add_argument("--checkpoint", default="juggernautXL_v9.safetensors")
    p.add_argument("--controlnet", default="diffusers_xl_depth_full.safetensors")
    p.add_argument("--seed", type=int, default=1994)
    args = p.parse_args(argv)

    pack, maps, out = Path(args.pack), Path(args.maps), Path(args.out)
    items: list[tuple[str, str, int]] = [(f, d, args.seed + n) for n, (f, d) in enumerate(args.item)]
    if args.items_json:
        for e in json.loads(Path(args.items_json).read_text(encoding="utf-8")):
            items.append((e["file"], e["description"], int(e.get("seed", args.seed))))
    if not items:
        p.error("give --item or --items-json")
    client = hdpack.ComfyClient(args.server, timeout=60)
    rows: list[tuple[str, list[Image.Image]]] = []
    with tempfile.TemporaryDirectory() as tmp:
        for n, (rel, description, seed) in enumerate(items):
            if args.skip_done and all((out / f"{d:.2f}" / rel).exists() for d in args.denoise):
                continue
            with Image.open(pack / rel) as im:
                sprite = im.convert("RGBA")
            with Image.open(maps / "depth" / rel) as dm:
                depth = dm.copy()
            size = model_size(*sprite.size)
            flat, depth_rgb = prepare(sprite, depth, size)
            stem = rel.replace("/", "_")[:-4]
            flat.save(Path(tmp) / f"{stem}_in.png")
            depth_rgb.save(Path(tmp) / f"{stem}_depth.png")
            in_name = client.upload_image(Path(tmp) / f"{stem}_in.png", f"pthd_{stem}_in.png")
            depth_name = client.upload_image(Path(tmp) / f"{stem}_depth.png", f"pthd_{stem}_depth.png")
            pics = [sprite]
            for denoise in args.denoise:
                started = time.monotonic()
                values = {
                    "checkpoint": args.checkpoint,
                    "controlnet": args.controlnet,
                    "positive": f"{description}, {STYLE}",
                    "negative": NEGATIVE,
                    "input_image": in_name,
                    "depth_image": depth_name,
                    "seed": seed,
                    "denoise": denoise,
                    "control_strength": args.control_strength,
                }
                entry = client.wait(client.queue(workflow(values)), timeout=600)
                generated = Image.open(io.BytesIO(client.first_image(entry)))
                result = finish(generated, sprite)
                target = out / f"{denoise:.2f}" / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                result.save(target)
                pics.append(result)
                print(
                    f"[{n + 1}/{len(items)}] {rel} denoise {denoise} in {time.monotonic() - started:.1f}s",
                    flush=True,
                )
            if len(rows) < args.sheet_limit:
                rows.append((f"{rel}: {description}", pics))
    sheet(rows, ["current (pixel-art scaler)"] + [f"redraw, denoise {d}" for d in args.denoise], out / "sheet.png")
    print(f"sheet: {out / 'sheet.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
