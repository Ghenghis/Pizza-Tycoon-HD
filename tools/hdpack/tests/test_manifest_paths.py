# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manifest file names are data: none may lead a read or write outside the
export or pack folder, by its spelling or through a symlink."""

from __future__ import annotations

import json
from collections.abc import Sequence
from pathlib import Path
from typing import cast

import pytest
from PIL import Image

import hdpack

HERE = Path(__file__).resolve().parents[1]

HOSTILE_NAMES = [
    "/etc/passwd",
    "../outside.png",
    "gfx/../../outside.png",
    "gfx/..",
    "..",
    "C:/Windows/win.ini",
    "C:outside.png",
    "c:\\outside.png",
    "\\\\server\\share\\outside.png",
    "//server/share/outside.png",
    "gfx\\..\\..\\outside.png",
    "\\outside.png",
    "gfx/./back1.png",
    "gfx//back1.png",
    "gfx/back1.png/",
    "",
    "gfx/back1.png:hidden",
    "gfx/back1\0.png",
    "gfx/con.png",
    "NUL",
    "bob/lpt1.txt/0000.png",
    "gfx/com\u00b9.png",
]


def _manifest(root: Path, files: Sequence[object]) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    images = [
        {"kind": "vga", "key": "x", "file": f, "width": 2, "height": 2, "opaque_pixels": 4, "transparent_pixels": 0}
        for f in files
    ]
    (root / "manifest.json").write_text(json.dumps({"format": 1, "images": images}))
    return root


def _symlink(link: Path, target: Path) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
    except OSError as exc:  # e.g. Windows without the symlink privilege
        pytest.skip(f"cannot make symlinks here: {exc}")


@pytest.mark.parametrize("name", HOSTILE_NAMES)
def test_hostile_manifest_names_are_rejected(tmp_path: Path, name: str) -> None:
    export = _manifest(tmp_path / "export", ["gfx/fine.png", name])
    with pytest.raises(hdpack.UnsafePathError, match="image 1"):
        hdpack.load_manifest(export)
    with pytest.raises(hdpack.UnsafePathError):
        hdpack.check_relative(name)
    # Every command stops before touching a file, with a plain error.
    assert hdpack.main(["upscale", str(export), str(tmp_path / "pack")]) == 2
    assert hdpack.main(["validate", str(export), str(tmp_path)]) == 2
    assert not (tmp_path / "pack").exists()
    assert not (tmp_path / "outside.png").exists()


@pytest.mark.parametrize("name", [None, 5, ["gfx/a.png"], {"file": "gfx/a.png"}])
def test_names_that_are_not_strings_are_rejected(tmp_path: Path, name: object) -> None:
    export = _manifest(tmp_path / "export", [name])
    with pytest.raises(hdpack.UnsafePathError, match="not a relative path"):
        hdpack.load_manifest(export)


def test_plain_names_pass(tmp_path: Path) -> None:
    names = [
        "gfx/back1.png",
        "bob/moebel/0000.png",
        "plegacy/game_location.png",
        "gfx/a.b.png",
        "gfx/..png",
        "gfx/console.png",
        "gfx/com10.png",
        "a",
    ]
    for name in names:
        assert hdpack.check_relative(name) == name
    root = tmp_path / "root"
    root.mkdir()
    assert hdpack.contained_path(root, "gfx/a.png") == (root / "gfx" / "a.png").resolve()
    assert [a.file for a in hdpack.load_manifest(_manifest(tmp_path / "export", names))] == names


def test_a_symlinked_pack_folder_cannot_redirect_writes(small_export: Path, tmp_path: Path) -> None:
    outside = tmp_path / "outside"
    outside.mkdir()
    pack = tmp_path / "pack"
    _symlink(pack / "gfx", outside)
    counts = hdpack.upscale_pack(small_export, pack, 2)
    # The two gfx pictures are refused; the sprite frames are written.
    assert (counts.written, counts.failed) == (2, 2)
    assert all("leads outside" in e for e in counts.errors)
    assert list(outside.iterdir()) == []
    report = hdpack.validate_pack(small_export, pack)
    assert sum("leads outside" in e for e in report.errors) == 2


def test_a_symlinked_pack_file_cannot_overwrite_outside(small_export: Path, tmp_path: Path) -> None:
    victim = tmp_path / "victim.png"
    victim.write_bytes(b"keep me")
    pack = tmp_path / "pack"
    _symlink(pack / "gfx" / "back1.png", victim)
    counts = hdpack.upscale_pack(small_export, pack, 2, force=True)
    assert counts.failed == 1 and "leads outside" in counts.errors[0]
    assert victim.read_bytes() == b"keep me"


def test_a_symlinked_export_file_cannot_be_read_outside(small_export: Path, tmp_path: Path) -> None:
    secret = tmp_path / "secret.png"
    Image.new("RGBA", (20, 10), (9, 9, 9, 255)).save(secret)
    (small_export / "gfx" / "back1.png").unlink()
    _symlink(small_export / "gfx" / "back1.png", secret)
    pack = tmp_path / "pack"
    counts = hdpack.upscale_pack(small_export, pack, 2)
    assert counts.failed == 1 and "leads outside" in counts.errors[0]
    assert not (pack / "gfx" / "back1.png").exists()


def test_symlinks_that_stay_inside_still_work(small_export: Path, tmp_path: Path) -> None:
    # A link inside the pack to another folder of the pack...
    pack = tmp_path / "pack"
    real = pack / "real_gfx"
    real.mkdir(parents=True)
    _symlink(pack / "gfx", real)
    counts = hdpack.upscale_pack(small_export, pack, 2)
    assert (counts.written, counts.failed) == (4, 0)
    assert (real / "back1.png").exists()
    # ...and a pack folder that is itself a link (to another drive, say).
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    _symlink(tmp_path / "linked_pack", elsewhere)
    counts = hdpack.upscale_pack(small_export, tmp_path / "linked_pack", 2)
    assert (counts.written, counts.failed) == (4, 0)
    assert hdpack.validate_pack(small_export, tmp_path / "linked_pack").errors == []


class _NoServer:
    """A ComfyUI client that must not be reached."""

    def upload_image(self, *_: object) -> str:
        raise AssertionError("uploaded a file that should have been refused")


def test_comfy_refuses_escaping_paths_before_uploading(small_export: Path, tmp_path: Path) -> None:
    outside = tmp_path / "outside"
    outside.mkdir()
    pack = tmp_path / "pack"
    _symlink(pack / "bob", outside)
    wf = json.loads((HERE / "workflows" / "upscale_only_api.json").read_text())
    settings = hdpack.ComfySettings(wf, hdpack.load_prompts(HERE / "prompts.json"), 6, "", "m", 1, 1.0, 5)
    client = cast(hdpack.ComfyClient, _NoServer())
    counts = hdpack.comfy_pack(client, small_export, pack, settings, ["bob/*"], log=lambda *_: None)
    assert counts.failed == 2 and all("leads outside" in e for e in counts.errors)
    assert list(outside.iterdir()) == []
