# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keeps the tests' own skips out of the skip list of a local CI run around them."""

from __future__ import annotations

import pytest


@pytest.fixture(autouse=True)
def no_ci_skip_log(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("PLEGACY_SKIP_LOG", raising=False)
