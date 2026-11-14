# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Seeds and provenance for generated pack art.

Seeds. An asset's seed is a hash of the run's global seed, the workflow it
goes through and its group (the picture's key; every frame of one sprite
shares it, so its views match). It never depends on the asset's place in
the queue, so --only, --limit and resuming give an asset the same seed.

Provenance. Every generated file gets a sidecar under
<root>/_provenance/<asset id>/provenance.json (asset id = the file's path
without .png, e.g. gfx/back1), written atomically after the file itself. It
names the input, the outputs and their SHA-256, the workflow (hash, and a
content-addressed copy under _provenance/_workflows/), the models (name,
SHA-256 when known, licence), the seed, the prompts, the tool and when.
check() verifies that every picture under a root has a sidecar that names
it with its current hash.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import platform
import uuid
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any

TOOL_NAME = "plegacy-hdpack"
TOOL_VERSION = "1.1.0"  # keep in step with pyproject.toml
SCHEMA = "plegacy.hd-provenance/1"
FOLDER = "_provenance"
WORKFLOWS = "_workflows"
SEED_SCHEME = "int(sha256(f'{global_seed}:{workflow_id}:{group}')[:8], 16) & 0x7fffffff"


class ProvenanceError(Exception):
    """A sidecar that cannot be written or read."""


# --------------------------------------------------------------- hashing


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def canonical_json(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def workflow_id(workflow: Mapping[str, Any]) -> str:
    """The workflow's identity: SHA-256 of its canonical JSON (key order and
    whitespace do not matter)."""
    return sha256_bytes(canonical_json(workflow))


def asset_seed(global_seed: int, workflow: str, group: str) -> int:
    """Seed for one asset group: stable across runs, filters and resumes."""
    digest = sha256_bytes(f"{global_seed}:{workflow}:{group}".encode())
    return int(digest[:8], 16) & 0x7FFFFFFF


# ----------------------------------------------------------------- files


def write_bytes_atomic(path: Path, data: bytes) -> None:
    """Write via a temporary file in the same folder, flushed, then renamed
    over the target: a reader sees the old file or the whole new one."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with tmp.open("wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except OSError as exc:
        tmp.unlink(missing_ok=True)
        raise ProvenanceError(f"cannot write {path}: {exc}") from exc


def write_json_atomic(path: Path, obj: Any) -> None:
    write_bytes_atomic(path, (json.dumps(obj, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def asset_id(rel: str) -> str:
    """'gfx/back1.png' -> 'gfx/back1'."""
    p = PurePosixPath(rel)
    return str(p.with_suffix("")) if p.suffix.lower() == ".png" else str(p)


def sidecar_path(root: Path, rel: str) -> Path:
    return root / FOLDER / asset_id(rel) / "provenance.json"


def store_workflow(root: Path, workflow: Mapping[str, Any]) -> str:
    """Keep one copy of each workflow used, named by its hash."""
    wid = workflow_id(workflow)
    path = root / FOLDER / WORKFLOWS / f"{wid}.json"
    if not path.exists():
        write_bytes_atomic(path, canonical_json(workflow))
    return wid


# ----------------------------------------------------------------- record


@dataclass(frozen=True)
class Model:
    role: str  # "checkpoint", "upscale", "depth", ...
    name: str
    sha256: str | None = None
    license: str = "unknown"
    revision: str | None = None

    def to_json(self) -> dict[str, Any]:
        return {
            "role": self.role,
            "name": self.name,
            "sha256": self.sha256,
            "license": self.license,
            "revision": self.revision,
        }


def load_model_index(path: Path | None) -> dict[str, dict[str, str]]:
    """Optional {"<model file name>": {"sha256": ..., "license": ...}} so a
    run can record what it cannot ask ComfyUI for."""
    if path is None:
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ProvenanceError(f"{path}: cannot read the model index ({exc})") from exc
    if not isinstance(data, dict):
        raise ProvenanceError(f"{path}: the model index must be an object of model names")
    out: dict[str, dict[str, str]] = {}
    for name, info in data.items():
        if not isinstance(info, dict):
            raise ProvenanceError(f"{path}: {name}: expected an object")
        out[str(name)] = {str(k): str(v) for k, v in info.items()}
    return out


def model(role: str, name: str, index: Mapping[str, Mapping[str, str]]) -> Model:
    info = index.get(name, {})
    return Model(role, name, info.get("sha256"), info.get("license", "unknown"), info.get("revision"))


@dataclass
class Record:
    stage: str  # "comfy", "upscale", "depth+normal"
    asset: dict[str, Any]
    input_file: str
    input_sha256: str
    outputs: dict[str, str]  # path relative to the root -> sha256
    workflow: dict[str, Any] | None = None
    models: list[Model] = field(default_factory=list)
    seed: int | None = None
    seed_inputs: dict[str, Any] | None = None
    parameters: dict[str, Any] = field(default_factory=dict)

    def to_json(self, now: dt.datetime | None = None) -> dict[str, Any]:
        when = (now or dt.datetime.now(dt.timezone.utc)).replace(microsecond=0).isoformat()
        return {
            "schema": SCHEMA,
            "stage": self.stage,
            "asset": self.asset,
            "input": {"file": self.input_file, "sha256": self.input_sha256},
            "outputs": [{"file": f, "sha256": s} for f, s in sorted(self.outputs.items())],
            "workflow": self.workflow,
            "models": [m.to_json() for m in self.models],
            "seed": self.seed,
            "seed_scheme": SEED_SCHEME if self.seed is not None else None,
            "seed_inputs": self.seed_inputs,
            "parameters": self.parameters,
            "tool": {"name": TOOL_NAME, "version": TOOL_VERSION, "python": platform.python_version()},
            "created_utc": when,
            "review": "pending",
        }


def write(root: Path, rel: str, record: Record) -> Path:
    path = sidecar_path(root, rel)
    write_json_atomic(path, record.to_json())
    return path


def matches(root: Path, rel: str, files: Iterable[str]) -> bool:
    """True when rel's sidecar exists and names every one of files with its
    current hash (a stale or missing sidecar is False)."""
    path = sidecar_path(root, rel)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        recorded = {o["file"]: o["sha256"] for o in data["outputs"]}
        return all(recorded.get(f) == sha256_file(root / f) for f in files)
    except (OSError, ValueError, KeyError, TypeError):
        return False


# ------------------------------------------------------------------ check


@dataclass
class CheckReport:
    pictures: int = 0
    recorded: int = 0
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {
            "pictures": self.pictures,
            "recorded": self.recorded,
            "errors": self.errors,
            "warnings": self.warnings,
        }


def check(root: Path) -> CheckReport:
    """Every PNG under root (outside _provenance) must be named, with its
    current SHA-256, by a readable sidecar; every output a sidecar names must
    exist. Models recorded without a hash or licence are warnings."""
    report = CheckReport()
    recorded: dict[str, tuple[str, Path]] = {}
    side_root = root / FOLDER
    for side in sorted(side_root.rglob("provenance.json")) if side_root.is_dir() else []:
        try:
            data = json.loads(side.read_text(encoding="utf-8"))
            if data.get("schema") != SCHEMA:
                report.errors.append(f"{side.relative_to(root)}: unknown schema {data.get('schema')!r}")
                continue
            outputs = [(str(o["file"]), str(o["sha256"])) for o in data["outputs"]]
            models = data.get("models", [])
        except (OSError, ValueError, KeyError, TypeError) as exc:
            report.errors.append(f"{side.relative_to(root)}: unreadable ({exc})")
            continue
        for m in models:
            if not m.get("sha256"):
                report.warnings.append(f"{side.relative_to(root)}: model {m.get('name')!r} has no SHA-256")
            if m.get("license", "unknown") == "unknown":
                report.warnings.append(f"{side.relative_to(root)}: model {m.get('name')!r} has no licence")
        for rel, digest in outputs:
            recorded[rel] = (digest, side)
            if not (root / rel).is_file():
                report.errors.append(f"{rel}: named by {side.relative_to(root)} but missing")
    for png in sorted(root.rglob("*.png")):
        rel = png.relative_to(root).as_posix()
        if rel.split("/", 1)[0] == FOLDER:
            continue
        report.pictures += 1
        entry = recorded.get(rel)
        if entry is None:
            report.errors.append(f"{rel}: no provenance sidecar")
            continue
        if sha256_file(png) != entry[0]:
            report.errors.append(f"{rel}: does not match its sidecar ({entry[1].relative_to(root)})")
            continue
        report.recorded += 1
    return report
