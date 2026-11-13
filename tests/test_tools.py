"""Smoke tests for the shell tools in tools/testing/ and tools/ghidra/ (read-only; nothing here writes to the game dirs)."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
BASH = shutil.which("bash")
pytestmark = pytest.mark.skipif(BASH is None, reason="bash not on PATH")


def run(*args, timeout=120):
    return subprocess.run([BASH, *args], cwd=REPO, capture_output=True, text=True, timeout=timeout)


@pytest.mark.parametrize("script", ["testing/check_pristine.sh", "testing/restore_testgame.sh", "ghidra/ghidra.sh"])
def test_help(script):
    r = run(f"tools/{script}", "--help")
    assert r.returncode == 0, r.stderr
    assert "Usage:" in r.stdout


def test_check_pristine_bad_arg():
    assert run("tools/testing/check_pristine.sh", "nonsense").returncode == 2


def test_check_pristine_clean(game_dir):
    if not (game_dir / "PRISTINE.sha256").is_file():
        pytest.skip("no PRISTINE.sha256")
    r = run("tools/testing/check_pristine.sh", "-q", "pristine", "install")
    assert r.returncode == 0, r.stdout + r.stderr
    n = sum(1 for line in (game_dir / "PRISTINE.sha256").read_text().splitlines() if line.strip())
    assert r.stdout.count(f"OK: {n} files match") == 2


def test_ghidra_installed():
    r = run("tools/ghidra/ghidra.sh", "version")
    if r.returncode == 2:
        pytest.skip("Ghidra not installed (tools/ghidra/ghidra.sh install)")
    assert r.returncode == 0 and r.stdout.strip(), r.stderr
