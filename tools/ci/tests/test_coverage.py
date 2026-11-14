# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The screen-coverage gate, on synthetic screenshots from fake_game.py."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from fake_game import picture
from PIL import Image

import hd_gates
from hd_gates import COVERAGE_BUDGET, GateError, ScreenCoverage, flat_blocks, judge_coverage, load_coverage_budget

FAKE = str(Path(__file__).with_name("fake_game.py"))
SCREENS = [
    {"name": "main-menu", "open": ["nav main"]},
    {"name": "city-view", "open": ["nav cityview @"]},
    {"name": "depot", "open": ["depot give 0 0", "nav depot"]},
]


def test_flat_share_of_a_synthetic_screenshot() -> None:
    flat = flat_blocks(picture(3, 0.4), 3)
    assert flat.shape == (400, 640)
    assert flat.sum() == round(0.4 * 640 * 400)


def test_a_block_with_one_different_texel_is_not_flat() -> None:
    a = np.full((1200, 1920, 3), 50, dtype=np.uint8)
    a[2, 5] = (51, 50, 50)  # inside the block of engine pixel (0, 1)
    flat = flat_blocks(Image.fromarray(a), 3)
    assert not flat[0, 1]
    assert flat.sum() == 640 * 400 - 1


def test_a_screenshot_of_the_wrong_size_is_refused() -> None:
    with pytest.raises(GateError, match="not 1920x1200"):
        flat_blocks(Image.new("RGB", (1280, 800)), 3)


def test_judging_against_the_budget() -> None:
    def screen(name: str, flat: int | None, error: str = "") -> ScreenCoverage:
        return ScreenCoverage(name, ["nav x"], flat=flat, blocks=None if flat is None else 1000, error=error)

    screens = [
        screen("same", 400),
        screen("within", 401),
        screen("worse", 410),
        screen("better", 380),
        screen("new", 500),
        screen("gone", None, "'nav gone' failed"),
    ]
    judge_coverage(screens, {"same": 0.4, "within": 0.4, "worse": 0.4, "better": 0.4, "gone": 0.1}, 0.002)
    assert [sc.status for sc in screens] == ["pass", "pass", "worse", "better", "unbudgeted", "unreachable"]


def test_the_shipped_screen_list_is_valid() -> None:
    data = load_coverage_budget(COVERAGE_BUDGET)
    assert len(data["screens"]) >= 25
    assert data["hd_scale"] == 3


@pytest.mark.parametrize(
    "screens, message",
    [
        ([{"name": "a", "open": ["nav main"]}, {"name": "a", "open": ["nav ng"]}], "listed twice"),
        ([{"name": "Main Menu", "open": ["nav main"]}], "lower-case"),
        ([{"name": "main", "open": []}], "non-empty list"),
        ([], "no screens"),
    ],
)
def test_a_bad_screen_list_is_refused(tmp_path: Path, screens: list[dict[str, object]], message: str) -> None:
    budget = tmp_path / "budget.json"
    budget.write_text(json.dumps({"screens": screens}), encoding="utf-8")
    with pytest.raises(GateError, match=message):
        load_coverage_budget(budget)


def run_gate(tmp_path: Path, *extra: str) -> int:
    return hd_gates.main(
        [
            "--game",
            FAKE,
            "--game-path",
            str(tmp_path),
            "--checks",
            "coverage",
            "--artifacts",
            str(tmp_path / "artifacts"),
            "--coverage-budget",
            str(tmp_path / "budget.json"),
            "--coverage-report",
            str(tmp_path / "report"),
            *extra,
        ]
    )


@pytest.fixture
def budget(tmp_path: Path) -> Path:
    path = tmp_path / "budget.json"
    path.write_text(
        json.dumps({"hd_scale": 3, "tolerance": 0.002, "setup": ["restaurant qs 0 0"], "screens": SCREENS}),
        encoding="utf-8",
    )
    return path


def report(tmp_path: Path) -> dict[str, dict[str, object]]:
    doc = json.loads((tmp_path / "report" / "coverage.json").read_text(encoding="utf-8"))
    return {s["name"]: s for s in doc["screens"]}


def test_record_then_hold_the_budget(
    tmp_path: Path, budget: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setenv("FAKE_COVERAGE", json.dumps({"nav main": 0.6, "nav cityview @": 0.3, "nav depot": 0.1}))
    monkeypatch.delenv("FAKE_FAIL", raising=False)

    # No budget yet: skipped, which fails where skips must not happen.
    assert run_gate(tmp_path) == 0
    assert "SKIP coverage: 3 of 3 screens have no budget for no-pack@gpu" in capsys.readouterr().out
    assert run_gate(tmp_path, "--fail-on-skip") == 1

    # Record it.
    assert run_gate(tmp_path, "--coverage-update") == 0
    recorded = json.loads(budget.read_text(encoding="utf-8"))["budgets"]["no-pack@gpu"]["screens"]
    assert recorded == {"main-menu": 0.6, "city-view": 0.3, "depot": 0.1}
    assert json.loads(budget.read_text(encoding="utf-8"))["screens"] == SCREENS  # the list is kept

    # Same picture: passes. The report and the masks are there.
    capsys.readouterr()
    assert run_gate(tmp_path, "--fail-on-skip") == 0
    assert "PASS coverage: 3 screens, profile no-pack@gpu, mean flat 33.3%" in capsys.readouterr().out
    assert {s["status"] for s in report(tmp_path).values()} == {"pass"}
    with Image.open(tmp_path / "report" / "masks" / "main-menu.png") as mask:
        assert mask.size == (640, 400)
        assert int((np.asarray(mask) == 255).sum()) == round(0.6 * 640 * 400)
    assert "| main-menu | `nav main` | 60.0% | 60.0% | pass |" in (tmp_path / "report" / "coverage.md").read_text(
        encoding="utf-8"
    )

    # One screen loses HD art: fails, and says which.
    monkeypatch.setenv("FAKE_COVERAGE", json.dumps({"nav main": 0.6, "nav cityview @": 0.35, "nav depot": 0.05}))
    capsys.readouterr()
    assert run_gate(tmp_path, "--fail-on-skip") == 1
    out = capsys.readouterr().out
    assert "1 screen(s) got worse: city-view 35.0% > budget 30.0%" in out
    statuses = {name: s["status"] for name, s in report(tmp_path).items()}
    assert statuses == {"main-menu": "pass", "city-view": "worse", "depot": "better"}
    assert "evidence kept in" in out  # screenshots and the game log stay for a look


def test_a_screen_that_cannot_be_opened_fails(
    tmp_path: Path, budget: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setenv("FAKE_FAIL", json.dumps(["nav depot"]))
    assert run_gate(tmp_path, "--coverage-update") == 1
    assert "could not be opened: depot ('nav depot' failed)" in capsys.readouterr().out
    assert report(tmp_path)["depot"]["status"] == "unreachable"
    # The screens that did open are recorded anyway.
    recorded = json.loads(budget.read_text(encoding="utf-8"))["budgets"]["no-pack@gpu"]["screens"]
    assert set(recorded) == {"main-menu", "city-view"}


def test_a_failed_setup_stops_the_gate(
    tmp_path: Path, budget: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setenv("FAKE_FAIL", json.dumps(["restaurant qs 0 0"]))
    assert run_gate(tmp_path) == 1
    assert "coverage setup failed: restaurant qs 0 0" in capsys.readouterr().out


def test_budgets_are_kept_per_pack_and_renderer(
    tmp_path: Path, budget: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.delenv("FAKE_FAIL", raising=False)
    pack = tmp_path / "pack12"
    pack.mkdir()
    assert run_gate(tmp_path, "--pack", str(pack), "--coverage-update") == 0
    assert run_gate(tmp_path, "--pack", str(pack), "--software", "--coverage-update") == 0
    assert set(json.loads(budget.read_text(encoding="utf-8"))["budgets"]) == {"pack12@gpu", "pack12@software"}
