# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stable per-asset seeds, provenance sidecars and the pack-level check,
the depth model licence gate, and the depth/normals stage (planned and run
with a stand-in model; the real one needs a GPU and a broker grant)."""

from __future__ import annotations

import datetime as dt
import json
import threading
from collections.abc import Sequence
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from PIL import Image
from test_hdpack import _FakeComfy

import depthmaps
import hdpack
import maps
import provenance

HERE = Path(__file__).resolve().parents[1]


def _wf(name: str) -> dict[str, Any]:
    data: dict[str, Any] = json.loads((HERE / "workflows" / name).read_text())
    return data


def _settings(seed: int | None = 7) -> hdpack.ComfySettings:
    return hdpack.ComfySettings(
        _wf("sdxl_refine_api.json"),
        hdpack.load_prompts(HERE / "prompts.json"),
        6,
        "ckpt.safetensors",
        "4x.pth",
        seed,
        0.0,
        30,
        small_workflow=_wf("upscale_only_api.json"),
        small_area=100,
        models={"4x.pth": {"sha256": "ab" * 32, "license": "MIT"}},
    )


def _seeds(export: Path, pack: Path, patterns: Sequence[str] = (), limit: int | None = None) -> dict[str, int]:
    counts = hdpack.comfy_pack(
        hdpack.ComfyClient("http://127.0.0.1:9"),
        export,
        pack,
        _settings(),
        patterns,
        limit,
        dry_run=True,
        log=lambda *_: None,
    )
    return counts.seeds


# ------------------------------------------------------------------ seeds


def test_seed_is_a_hash_of_global_seed_workflow_and_group() -> None:
    a = provenance.asset_seed(1994, "wf1", "gfx/back1")
    assert a == provenance.asset_seed(1994, "wf1", "gfx/back1")
    assert 0 <= a < 2**31
    assert a != provenance.asset_seed(1995, "wf1", "gfx/back1")
    assert a != provenance.asset_seed(1994, "wf2", "gfx/back1")
    assert a != provenance.asset_seed(1994, "wf1", "gfx/back2")
    # Key order and whitespace do not change a workflow's identity.
    assert provenance.workflow_id({"a": 1, "b": [1, 2]}) == provenance.workflow_id({"b": [1, 2], "a": 1})


def test_seeds_do_not_depend_on_filter_limit_or_resume(small_export: Path, tmp_path: Path) -> None:
    full = _seeds(small_export, tmp_path / "p1")
    assert set(full) == {"gfx/back1.png", "gfx/k0.png", "bob/moebel/0000.png", "bob/moebel/0001.png"}
    # A pattern filter.
    only_bob = _seeds(small_export, tmp_path / "p2", patterns=["bob/*"])
    assert only_bob == {k: full[k] for k in only_bob} and len(only_bob) == 2
    # A limit, from the other end of the queue.
    one = _seeds(small_export, tmp_path / "p3", patterns=["gfx/k0.png"], limit=1)
    assert one == {"gfx/k0.png": full["gfx/k0.png"]}
    # A resume: the first picture is already done (with its sidecar).
    pack = tmp_path / "p4"
    hdpack.upscale_pack(small_export, pack, 6, patterns=["gfx/back1.png"])
    resumed = _seeds(small_export, pack)
    assert "gfx/back1.png" not in resumed
    assert resumed == {k: full[k] for k in resumed} and len(resumed) == 3


def test_frames_of_one_sprite_share_a_seed_and_workflows_differ(small_export: Path, tmp_path: Path) -> None:
    seeds = _seeds(small_export, tmp_path / "p")
    assert seeds["bob/moebel/0000.png"] == seeds["bob/moebel/0001.png"]  # one group: views match
    settings = _settings()
    assets = {a.file: a for a in hdpack.load_manifest(small_export)}
    seed_small, wid_small = settings.seed_for(assets["gfx/k0.png"])  # small picture: upscale workflow
    seed_big, wid_big = settings.seed_for(assets["gfx/back1.png"])  # diffusion workflow
    assert wid_small != wid_big
    assert wid_big == provenance.workflow_id(_wf("sdxl_refine_api.json"))
    assert seed_big == provenance.asset_seed(7, wid_big, "gfx/back1")


def test_cli_seed_has_a_fixed_default() -> None:
    args = hdpack.build_parser().parse_args(["comfy", "e", "p"])
    assert args.seed == 1994 and args.models is None


# ------------------------------------------------------------- provenance


def _fake_server() -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", 0), _FakeComfy)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    _FakeComfy.queued = []
    return server


def test_comfy_writes_a_sidecar_per_picture(small_export: Path, tmp_path: Path) -> None:
    server = _fake_server()
    try:
        pack = tmp_path / "pack"
        settings = _settings()
        settings.workflow = _wf("upscale_only_api.json")  # the fake server answers ImageScale sizes
        client = hdpack.ComfyClient(f"http://127.0.0.1:{server.server_port}")
        counts = hdpack.comfy_pack(client, small_export, pack, settings, log=lambda *_: None)
        assert (counts.written, counts.failed) == (4, 0)
    finally:
        server.shutdown()
        server.server_close()
    side = json.loads((pack / "_provenance/gfx/back1/provenance.json").read_text())
    assert side["schema"] == provenance.SCHEMA and side["stage"] == "comfy"
    assert side["input"]["sha256"] == provenance.sha256_file(small_export / "gfx/back1.png")
    assert side["outputs"] == [{"file": "gfx/back1.png", "sha256": provenance.sha256_file(pack / "gfx/back1.png")}]
    assert side["seed"] == counts.seeds["gfx/back1.png"]
    assert side["seed_inputs"]["group"] == "gfx/back1" and side["seed_inputs"]["global_seed"] == 7
    assert side["workflow"]["id"] == side["seed_inputs"]["workflow_id"]
    assert (pack / "_provenance/_workflows" / f"{side['workflow']['id']}.json").is_file()
    assert {m["name"]: m["license"] for m in side["models"]} == {"4x.pth": "MIT"}
    assert side["tool"]["version"] == provenance.TOOL_VERSION and side["review"] == "pending"
    assert side["created_utc"].endswith("+00:00")
    # The engine-facing check still passes, the provenance check too.
    assert hdpack.validate_pack(small_export, pack).errors == []
    report = provenance.check(pack)
    assert report.errors == [] and report.recorded == report.pictures == 4
    assert not list(pack.rglob("*.tmp"))  # every write was whole


def test_check_catches_missing_changed_and_orphaned(small_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    assert hdpack.upscale_pack(small_export, pack, 6).written == 4
    assert provenance.check(pack).errors == []
    assert hdpack.main(["validate", str(small_export), str(pack), "--provenance"]) == 0
    Image.new("RGBA", (120, 60), (1, 2, 3, 255)).save(pack / "gfx/back1.png")  # edited by hand
    (pack / "_provenance/gfx/k0/provenance.json").unlink()  # sidecar lost
    (pack / "bob/moebel/0001.png").unlink()  # picture lost
    errors = provenance.check(pack).errors
    assert any("gfx/back1.png: does not match" in e for e in errors)
    assert any("gfx/k0.png: no provenance sidecar" in e for e in errors)
    assert any("bob/moebel/0001.png: named by" in e and "missing" in e for e in errors)
    assert hdpack.main(["validate", str(small_export), str(pack), "--provenance"]) == 1
    assert hdpack.main(["provenance", str(pack)]) == 1


def test_stale_pictures_are_redone_and_unrecorded_ones_kept(small_export: Path, tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    hdpack.upscale_pack(small_export, pack, 6)
    Image.new("RGBA", (120, 60), (1, 2, 3, 255)).save(pack / "gfx/back1.png")  # sidecar now stale
    (pack / "_provenance/gfx/k0/provenance.json").unlink()  # older pack, no sidecar
    counts = hdpack.upscale_pack(small_export, pack, 6)
    assert counts.written == 1 and counts.unrecorded == 1 and counts.skipped == 3
    errors = provenance.check(pack).errors
    assert errors == ["gfx/k0.png: no provenance sidecar"]  # kept, and reported


def test_model_index_and_unknown_models(tmp_path: Path) -> None:
    index = tmp_path / "models.json"
    index.write_text(json.dumps({"x.safetensors": {"sha256": "cd" * 32, "license": "CreativeML-OpenRAIL-M"}}))
    loaded = provenance.load_model_index(index)
    assert provenance.model("checkpoint", "x.safetensors", loaded).license == "CreativeML-OpenRAIL-M"
    unknown = provenance.model("upscale", "y.pth", loaded)
    assert unknown.sha256 is None and unknown.license == "unknown"
    index.write_text("[1, 2]")
    with pytest.raises(provenance.ProvenanceError):
        provenance.load_model_index(index)


def test_atomic_write_replaces_whole(tmp_path: Path) -> None:
    target = tmp_path / "a" / "b.json"
    provenance.write_json_atomic(target, {"v": 1})
    provenance.write_json_atomic(target, {"v": 2})
    assert json.loads(target.read_text()) == {"v": 2}
    assert [p.name for p in target.parent.iterdir()] == ["b.json"]


# ------------------------------------------------------- depth licence gate


def test_depth_defaults_to_the_apache_model() -> None:
    m = depthmaps.choose_model(depthmaps.DEFAULT_MODEL)
    assert m.repo == "depth-anything/Depth-Anything-V2-Small-hf" and m.license == "Apache-2.0"
    assert m.comfy_ckpt == "depth_anything_v2_vits.pth"
    for key in ("base", "large"):
        with pytest.raises(depthmaps.LicenceError, match="CC-BY-NC-4.0"):
            depthmaps.choose_model(key)
        assert depthmaps.choose_model(key, allow_noncommercial=True).license == "CC-BY-NC-4.0"


def test_depthmaps_refuses_large_before_loading_anything(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    (tmp_path / "pack").mkdir()
    assert depthmaps.main([str(tmp_path / "pack"), str(tmp_path / "out"), "--model", "large"]) == 2
    assert "non-commercial" in capsys.readouterr().err
    # Accepted with the flag (an empty pack: nothing to load).
    assert (
        depthmaps.main([str(tmp_path / "pack"), str(tmp_path / "out"), "--model", "large", "--allow-noncommercial"])
        == 0
    )


def _ramp_estimator(img: Image.Image) -> np.ndarray:
    w, h = img.size
    return np.tile(np.linspace(0.0, 10.0, w, dtype=np.float32), (h, 1))


def _pack(root: Path) -> Path:
    for rel in ("gfx/back1.png", "bob/moebel/0000.png"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        img = Image.new("RGBA", (64, 48), (180, 120, 60, 255))
        if rel.startswith("bob"):
            img.putpixel((0, 0), (0, 0, 0, 0))
        img.save(path)
    return root


def test_depth_outputs_carry_model_and_licence(tmp_path: Path) -> None:
    pack, out = _pack(tmp_path / "pack"), tmp_path / "out"
    model = depthmaps.choose_model("small")
    for job in depthmaps.plan(pack, out).jobs:
        with Image.open(job.source) as im:
            depth, mask = depthmaps.estimate(_ramp_estimator, im.convert("RGBA"), 1024)
        depthmaps.write_outputs(
            out,
            job.rel,
            job.source,
            f"depth/{job.rel}",
            f"normal/{job.rel}",
            depthmaps.encode_maps(depth, mask, 3, 6.0),
            model,
            "abc123",
            {"device": "test"},
        )
    with Image.open(out / "depth/gfx/back1.png") as d:
        assert d.mode.startswith("I") and d.size == (64, 48)
    side = json.loads((out / "_provenance/gfx/back1/provenance.json").read_text())
    assert side["models"] == [
        {"role": "depth", "name": model.repo, "sha256": None, "license": "Apache-2.0", "revision": "abc123"}
    ]
    assert provenance.check(out).errors == []
    assert depthmaps.plan(pack, out).jobs == []  # done, recorded


def test_bilateral_smoothing_keeps_edges() -> None:
    depth = np.zeros((16, 16), dtype=np.float32)
    depth[:, 8:] = 1.0
    noisy = depth + np.random.default_rng(1).normal(0, 0.01, depth.shape).astype(np.float32)
    mask = np.ones_like(depth, dtype=bool)
    out = depthmaps.smooth_depth(noisy, mask, 3)
    assert np.std(out[:, :6]) < np.std(noisy[:, :6])  # noise smoothed
    assert out[8, 7] < 0.1 and out[8, 8] > 0.9  # step kept
    assert np.array_equal(depthmaps.smooth_depth(noisy, mask, 0), noisy)


# ------------------------------------------------------ depth/normals stage


def _grant(path: Path, **over: object) -> Path:
    data = {
        "grant_id": "test-1",
        "scope": "hdpack-maps",
        "gpu": 0,
        "not_after_utc": (dt.datetime.now(dt.timezone.utc) + dt.timedelta(hours=1)).isoformat(),
    } | over
    path.write_text(json.dumps(data))
    return path


def test_maps_layout_plan(tmp_path: Path) -> None:
    pack = _pack(tmp_path / "pack")
    work = maps.plan(pack, tmp_path / "maps")
    assert [b.rel for b in work.bundles] == ["bob/moebel/0000.png", "gfx/back1.png"]
    assert work.bundles[1].depth_rel == "gfx/back1/depth_16.png"
    assert work.bundles[1].normal_rel == "gfx/back1/normal.png"
    assert maps.main(["plan", str(pack), str(tmp_path / "maps"), "--json"]) == 0


def test_maps_run_refuses_without_a_valid_grant(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv(maps.GRANT_ENV, raising=False)
    pack = _pack(tmp_path / "pack")
    base = ["run", str(pack), str(tmp_path / "maps")]
    assert maps.main(base) == 2
    assert "broker grant" in capsys.readouterr().err
    expired = _grant(tmp_path / "old.json", not_after_utc="2020-01-01T00:00:00+00:00")
    assert maps.main([*base, "--grant-file", str(expired)]) == 2
    assert "ended" in capsys.readouterr().err
    other = _grant(tmp_path / "other.json", scope="comfy-redraw")
    assert maps.main([*base, "--grant-file", str(other)]) == 2
    naive = _grant(tmp_path / "naive.json", not_after_utc="2999-01-01T00:00:00")
    assert maps.main([*base, "--grant-file", str(naive)]) == 2
    # A valid grant still does not let the NC model through.
    good = _grant(tmp_path / "good.json")
    assert maps.main([*base, "--grant-file", str(good), "--model", "large"]) == 2
    assert "non-commercial" in capsys.readouterr().err
    assert not (tmp_path / "maps").exists()  # nothing was written


def test_maps_bundles_with_a_stand_in_model(tmp_path: Path) -> None:
    pack, out = _pack(tmp_path / "pack"), tmp_path / "maps"
    work = maps.plan(pack, out)
    failed = maps.run_bundles(
        out, work, _ramp_estimator, depthmaps.choose_model("small"), None, {"route": "test"}, log=lambda *_: None
    )
    assert failed == 0
    with Image.open(out / "gfx/back1/normal.png") as n:
        assert n.mode == "RGB"
    report = provenance.check(out)
    assert report.errors == [] and report.recorded == 4
    assert maps.plan(pack, out).bundles == []
    Image.new("RGB", (64, 48)).save(out / "gfx/back1/normal.png")  # changed: stale, redo
    assert [b.rel for b in maps.plan(pack, out).bundles] == ["gfx/back1.png"]


def test_maps_comfy_workflow_fills_with_the_small_model() -> None:
    wf = _wf("depth_anything_v2_api.json")
    values = {
        "input_image": "in.png",
        "depth_model": depthmaps.choose_model("small").comfy_ckpt,
        "resolution": 1024,
        "prefix": "plegacy_maps/depth",
    }
    filled = hdpack.fill_template(wf, values)
    assert "{{" not in json.dumps(filled)
    pre = next(n for n in filled.values() if n["class_type"] == "DepthAnythingV2Preprocessor")
    assert pre["inputs"]["ckpt_name"] == "depth_anything_v2_vits.pth" and pre["inputs"]["resolution"] == 1024
    assert hdpack.placeholders(wf) == set(values)
