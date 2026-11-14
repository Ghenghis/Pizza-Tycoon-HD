# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import hdpack

HERE = Path(__file__).resolve().parents[1]


# ---------------------------------------------------------------- manifest


def test_manifest_loads_real_format(small_export: Path) -> None:
    assets = hdpack.load_manifest(small_export)
    assert [a.file for a in assets] == ["gfx/back1.png", "gfx/k0.png", "bob/moebel/0000.png", "bob/moebel/0001.png"]
    sprite = assets[2]
    assert sprite.has_transparency and sprite.frame == 0
    assert sprite.target_size(6) == (36, 48)


def test_manifest_errors_are_helpful(tmp_path: Path) -> None:
    with pytest.raises(hdpack.HdPackError, match="--export-assets"):
        hdpack.load_manifest(tmp_path)
    (tmp_path / "manifest.json").write_text('{"format": 99, "images": []}')
    with pytest.raises(hdpack.HdPackError, match="unsupported"):
        hdpack.load_manifest(tmp_path)


def test_select_globs(small_export: Path) -> None:
    assets = hdpack.load_manifest(small_export)
    assert len(hdpack.select(assets, ["bob/*"])) == 2
    assert len(hdpack.select(assets, [])) == 4


# ------------------------------------------------------------------ images


def test_premultiplied_resize_has_no_fringe() -> None:
    img = Image.new("RGBA", (4, 4), (255, 0, 0, 0))  # invisible red
    img.putpixel((1, 1), (255, 255, 255, 255))
    out = np.asarray(hdpack.premultiplied_resize(img, (16, 16)))
    visible = out[..., 3] > 0
    # Nothing visible may be tinted by the invisible red.
    assert np.all(out[visible][:, 1] >= out[visible][:, 0] - 2)


def test_reference_alpha_restores_sprite_outline() -> None:
    ref = Image.new("RGBA", (4, 4), (0, 0, 0, 0))
    ref.putpixel((1, 1), (9, 9, 9, 255))
    generated = Image.new("RGB", (40, 40), (50, 60, 70))
    out = hdpack.apply_reference_alpha(generated, ref, feather=0)
    a = np.asarray(out.getchannel("A"))
    assert a[15, 15] == 255 and a[0, 0] == 0
    opaque_ref = Image.new("RGBA", (4, 4), (1, 2, 3, 255))
    assert hdpack.apply_reference_alpha(generated, opaque_ref, 2).getchannel("A").getextrema() == (255, 255)


def test_generation_size_floor_and_ceiling() -> None:
    assert hdpack.generation_size(126, 138) == (936, 1024)
    assert max(hdpack.generation_size(3840, 2400)) == 2048
    w, h = hdpack.generation_size(1152, 1056)
    assert (w, h) == (1152, 1056) and w % 8 == 0


# ---------------------------------------------------------- upscale+validate


def test_upscale_then_validate_is_clean(small_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    counts = hdpack.upscale_pack(small_export, pack, 4)
    assert (counts.written, counts.failed) == (4, 0)
    with Image.open(pack / "gfx/back1.png") as img:
        assert img.size == (80, 40)
    report = hdpack.validate_pack(small_export, pack)
    assert report.errors == [] and report.coverage == 1.0
    # Second run keeps existing files.
    assert hdpack.upscale_pack(small_export, pack, 4).skipped == 4


def test_validate_catches_what_the_engine_rejects(small_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    (pack / "gfx").mkdir(parents=True)
    (pack / "bob/moebel").mkdir(parents=True)
    Image.new("RGBA", (100, 100), (1, 1, 1, 255)).save(pack / "gfx/back1.png")  # wrong aspect
    Image.new("RGBA", (24, 32), (0, 0, 0, 0)).save(pack / "bob/moebel/0000.png")  # all holes
    Image.new("RGBA", (6, 8), (0, 0, 0, 255)).save(pack / "bob/moebel/0001.png")  # 1x: warning
    Image.new("RGBA", (5, 5)).save(pack / "gfx/stray.png")
    report = hdpack.validate_pack(small_export, pack)
    joined = "\n".join(report.errors)
    assert "aspect ratio" in joined and "transparent in the HD art" in joined
    assert any("only 1.0x" in w for w in report.warnings)
    assert any("stray.png" in w for w in report.warnings)
    assert hdpack.main(["validate", str(small_export), str(pack)]) == 1


def test_real_export_baseline_pack(real_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    counts = hdpack.upscale_pack(real_export, pack, 2, "nearest", ["gfx/back*", "bob/moebel/*"])
    assert counts.failed == 0 and counts.written > 300
    report = hdpack.validate_pack(real_export, pack)
    assert report.errors == []
    assert 0 < report.coverage < 1


# ------------------------------------------------------------------ comfy


def test_fill_template_types_and_typos() -> None:
    wf = {"1": {"inputs": {"seed": "{{seed}}", "text": "a {{positive}} b", "n": 3}}}
    out = hdpack.fill_template(wf, {"seed": 7, "positive": "cat"})
    assert out["1"]["inputs"] == {"seed": 7, "text": "a cat b", "n": 3}
    with pytest.raises(hdpack.HdPackError):
        hdpack.fill_template({"x": "{{sede}}"}, {"seed": 1})


def test_shipped_workflows_and_prompts_fill(small_export: Path) -> None:
    prompts = hdpack.load_prompts(HERE / "prompts.json")
    assets = hdpack.load_manifest(small_export)
    assert prompts.for_asset(assets[0]).match == "gfx/back*"
    assert prompts.for_asset(assets[2]).match == "bob/moebel/*"
    for name in ("sdxl_refine_api.json", "upscale_only_api.json"):
        wf = json.loads((HERE / "workflows" / name).read_text())
        settings = hdpack.ComfySettings(wf, prompts, 6, "x.safetensors", "4x.pth", 1, 1.5, 60)
        filled = hdpack.fill_template(wf, hdpack.comfy_values(assets[2], settings, "up.png", 5))
        assert "{{" not in json.dumps(filled)


class _FakeComfy(BaseHTTPRequestHandler):
    """Stand-in for a GPU ComfyUI server: it answers the same four endpoints
    and 'generates' by returning a flat image at the requested size. The real
    server is exercised by `hdpack.py doctor` and `comfy --limit 1` on a
    machine with a GPU (see docs/HD_REMASTER.md)."""

    queued: list[dict] = []

    def log_message(self, *args: object) -> None:  # keep test output quiet
        pass

    def _json(self, obj: object) -> None:
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers["Content-Length"])
        body = self.rfile.read(length)
        if self.path == "/upload/image":
            assert b'name="image"' in body
            self._json({"name": "uploaded.png", "subfolder": ""})
        elif self.path == "/prompt":
            self.queued.append(json.loads(body)["prompt"])
            self._json({"prompt_id": f"p{len(self.queued)}"})

    def do_GET(self) -> None:  # noqa: N802
        if self.path.startswith("/history/"):
            pid = self.path.rsplit("/", 1)[1]
            self._json(
                {pid: {"status": {"completed": True}, "outputs": {"9": {"images": [{"filename": pid + ".png"}]}}}}
            )
        elif self.path.startswith("/view"):
            prompt = self.queued[-1]
            size = next(
                (n["inputs"]["width"], n["inputs"]["height"])
                for n in prompt.values()
                if n["class_type"] == "ImageScale"
            )
            buf = BytesIO()
            Image.new("RGB", size, (30, 160, 90)).save(buf, "PNG")
            data = buf.getvalue()
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.end_headers()
            self.wfile.write(data)


def test_comfy_batch_round_trip(small_export: Path, tmp_path: Path) -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), _FakeComfy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        _FakeComfy.queued = []
        wf = json.loads((HERE / "workflows" / "upscale_only_api.json").read_text())
        settings = hdpack.ComfySettings(wf, hdpack.load_prompts(HERE / "prompts.json"), 6, "", "4x.pth", 3, 0.0, 30)
        client = hdpack.ComfyClient(f"http://127.0.0.1:{server.server_port}")
        pack = tmp_path / "pack"
        counts = hdpack.comfy_pack(client, small_export, pack, settings, ["bob/*"], log=lambda *_: None)
        assert (counts.written, counts.failed) == (2, 0)
        with Image.open(pack / "bob/moebel/0000.png") as out:
            assert out.size == (36, 48)
            assert out.getpixel((0, 0))[3] == 0  # outline from the original
            assert out.getpixel((18, 24))[3] == 255
        assert hdpack.validate_pack(small_export, pack).errors == []
    finally:
        server.shutdown()
        server.server_close()


def test_comfy_unreachable_server_stops_cleanly(small_export: Path, tmp_path: Path) -> None:
    wf = json.loads((HERE / "workflows" / "upscale_only_api.json").read_text())
    settings = hdpack.ComfySettings(wf, hdpack.load_prompts(HERE / "prompts.json"), 6, "", "m", 1, 1.0, 5)
    client = hdpack.ComfyClient("http://127.0.0.1:9", timeout=2)
    counts = hdpack.comfy_pack(client, small_export, tmp_path / "p", settings, log=lambda *_: None)
    assert counts.failed == 1 and "cannot reach ComfyUI" in counts.errors[0]


def _wf(name: str) -> dict:
    return json.loads((HERE / "workflows" / name).read_text())


def test_shipped_workflows_finish_with_a_model_pass() -> None:
    # 4K targets are far above what the diffusion step samples at, so the
    # last resize must start from a model upscale, never straight from the
    # VAE output; tiny sprites get two model passes (16x) for 12x targets.
    for name, passes in (("sdxl_refine_api.json", 2), ("upscale_only_api.json", 2)):
        wf = _wf(name)
        ups = {k for k, n in wf.items() if n["class_type"] == "ImageUpscaleWithModel"}
        assert len(ups) == passes, name
        save = next(n for n in wf.values() if n["class_type"] == "SaveImage")
        final_scale = wf[save["inputs"]["images"][0]]
        assert final_scale["class_type"] == "ImageScale"
        assert final_scale["inputs"]["image"][0] in ups, name


def test_small_pictures_skip_the_diffusion_model(small_export: Path) -> None:
    prompts = hdpack.load_prompts(HERE / "prompts.json")
    big, small = _wf("sdxl_refine_api.json"), _wf("upscale_only_api.json")
    assets = {a.file: a for a in hdpack.load_manifest(small_export)}
    settings = hdpack.ComfySettings(big, prompts, 12, "c", "u", 1, 1.0, 5, small_workflow=small, small_area=100)
    assert settings.workflow_for(assets["gfx/back1.png"]) is big  # 20x10 = 200
    assert settings.workflow_for(assets["gfx/k0.png"]) is small  # 12x4 = 48
    assert settings.workflow_for(assets["bob/moebel/0000.png"]) is small  # 6x8 = 48
    off = hdpack.ComfySettings(big, prompts, 12, "c", "u", 1, 1.0, 5, small_workflow=None)
    assert off.workflow_for(assets["bob/moebel/0000.png"]) is big


def test_comfy_routes_each_picture_to_its_workflow(small_export: Path, tmp_path: Path) -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), _FakeComfy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        _FakeComfy.queued = []
        settings = hdpack.ComfySettings(
            _wf("upscale_only_api.json"),
            hdpack.load_prompts(HERE / "prompts.json"),
            12,
            "",
            "4x.pth",
            3,
            0.0,
            30,
            small_workflow=_wf("upscale_only_api.json"),
            small_area=100,
        )
        client = hdpack.ComfyClient(f"http://127.0.0.1:{server.server_port}")
        pack = tmp_path / "pack"
        counts = hdpack.comfy_pack(client, small_export, pack, settings, log=lambda *_: None)
        assert (counts.written, counts.failed) == (4, 0)
        with Image.open(pack / "gfx/back1.png") as out:
            assert out.size == (240, 120)  # 12x for 4K
        assert hdpack.validate_pack(small_export, pack).errors == []
    finally:
        server.shutdown()
        server.server_close()


def test_cli_parses_small_workflow_options() -> None:
    args = hdpack.build_parser().parse_args(["comfy", "e", "p", "--small-area", "0", "--scale", "12"])
    assert (args.small_area, args.scale) == (0, 12)
    assert args.small_workflow.endswith("upscale_only_api.json")


def test_mmpx_pack_is_valid(small_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    counts = hdpack.upscale_pack(small_export, pack, 12, "mmpx")
    assert (counts.written, counts.failed) == (4, 0)
    with Image.open(pack / "bob/moebel/0000.png") as out:
        assert out.size == (72, 96)
        assert out.getpixel((0, 0))[3] == 0
    assert hdpack.validate_pack(small_export, pack).errors == []
