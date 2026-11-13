"""Shared fixtures. The corpus is the user's game install (never committed, AD-9)."""
import os
import sys
from pathlib import Path

import pytest

# The dmbmx package and the other Python tools live under tools/. Put that on sys.path for the tests
# and on PYTHONPATH for the subprocesses they start (`python -m dmbmx ...`).
TOOLS = str(Path(__file__).resolve().parents[1] / "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
if TOOLS not in os.environ.get("PYTHONPATH", "").split(os.pathsep):
    os.environ["PYTHONPATH"] = os.pathsep.join(p for p in (TOOLS, os.environ.get("PYTHONPATH")) if p)

GAME_DIR = Path(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
_EXCLUDE = {"_pristine", "_testgame", "wt", "dave-mirra-bmx", "dave-mirra-freestyle-bmx"}


def corpus_files(ext):
    """All install files with extension `ext` (case-insensitive, e.g. '.zmd'), excluding work folders."""
    if not GAME_DIR.is_dir():
        return []
    ext = ext.lower()
    out = []
    for root, dirs, files in os.walk(GAME_DIR):
        if Path(root) == GAME_DIR:
            dirs[:] = [d for d in dirs if d not in _EXCLUDE]
        out += [Path(root) / f for f in files if f.lower().endswith(ext)]
    return sorted(out)


def corpus_params(ext):
    """Use as: @pytest.mark.parametrize('path', corpus_params('.zmd'))"""
    files = corpus_files(ext)
    if not files:
        return [pytest.param(None, marks=pytest.mark.skip(reason=f"no {ext} corpus at {GAME_DIR}"))]
    return [pytest.param(p, id=str(p.relative_to(GAME_DIR))) for p in files]


@pytest.fixture
def game_dir():
    if not GAME_DIR.is_dir():
        pytest.skip(f"game dir missing: {GAME_DIR}")
    return GAME_DIR
