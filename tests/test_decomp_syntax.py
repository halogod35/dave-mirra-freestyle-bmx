"""Syntax-check every C file of the decompilation with clang (through zig).

There is no MSVC on the build PC, but zig ships a complete clang plus the MinGW headers, so
`zig clang -fsyntax-only` parses and type-checks each translation unit for 32-bit Windows (the
decomp includes <windows.h>, <mmsystem.h>, <math.h>, ...). Warnings are allowed; errors fail.

zig is optional. The test looks for it, in this order:
  1. env DMBMX_ZIG = path to zig(.exe)
  2. `zig` on PATH
  3. the `ziglang` pip package importable from this Python (`pip install ziglang`)
and skips when none is found. Install notes: docs/tooling.md, "C syntax check (zig)".

`zig cc -fsyntax-only` would also work but reports a bogus "failed to rename ... .obj" error from its
cache on Windows, so the test calls `zig clang` directly with the include paths and defines that
`zig cc -target x86-windows-gnu` passes (`zig cc -### ...` prints them).
"""
import os
import re
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
DECOMP = REPO / "decomp"
C_FILES = sorted(p for p in (DECOMP / "src").rglob("*.c"))


def find_zig():
    env = os.environ.get("DMBMX_ZIG")
    if env:
        return env if Path(env).is_file() else None
    z = shutil.which("zig")
    if z:
        return z
    try:
        import ziglang  # noqa: F401  (pip package: the binary sits next to __init__.py)
    except ImportError:
        return None
    for name in ("zig.exe", "zig"):
        p = Path(ziglang.__file__).parent / name
        if p.is_file():
            return str(p)
    return None


def zig_lib_dir(zig):
    out = subprocess.run([zig, "env"], capture_output=True, text=True, timeout=60).stdout
    # 0.14+: ZON (`.lib_dir = "..."`); older: JSON (`"lib_dir": "..."`)
    m = re.search(r'\.?"?lib_dir"?\s*[=:]\s*"((?:[^"\\]|\\.)*)"', out)
    if m:
        return m.group(1).replace("\\\\", "\\")
    return str(Path(zig).parent / "lib")


def clang_args(zig):
    lib = Path(zig_lib_dir(zig))
    inc = lib / "libc" / "include"
    args = [zig, "clang", "-fsyntax-only", "-target", "i386-pc-windows-gnu", "-nostdinc",
            "-fno-caret-diagnostics", "-ferror-limit=0",
            "-isystem", str(lib / "include")]
    for d in ("x86-windows-gnu", "generic-mingw", "x86-windows-any", "any-windows-any"):
        args += ["-isystem", str(inc / d)]
    args += ["-D__MSVCRT_VERSION__=0xE00", "-D_WIN32_WINNT=0x0a00", "-D__CRT__NO_INLINE",
             "-I", str(DECOMP / "include")]
    return args


ZIG = find_zig()


@pytest.fixture(scope="module")
def results():
    """{file: (returncode, stderr)} for every decomp C file, checked in parallel once."""
    if ZIG is None:
        pytest.skip("zig not found (set DMBMX_ZIG or install the ziglang pip package; docs/tooling.md)")
    base = clang_args(ZIG)

    def check(p):
        r = subprocess.run(base + [str(p)], capture_output=True, text=True, timeout=120)
        return p, (r.returncode, r.stderr)

    with ThreadPoolExecutor(max_workers=max(2, min(8, os.cpu_count() or 2))) as ex:
        return dict(ex.map(check, C_FILES))


def test_found_c_files():
    assert len(C_FILES) >= 80, "decomp/src lost its C files?"


@pytest.mark.parametrize("path", C_FILES, ids=[str(p.relative_to(DECOMP / "src")).replace("\\", "/") for p in C_FILES])
def test_syntax(results, path):
    rc, err = results[path]
    errors = [l for l in err.splitlines() if re.search(r"\b(error|fatal error):", l)]
    assert rc == 0 and not errors, "\n".join(errors[:40]) or err[-2000:]


def test_zig_lookup_env(monkeypatch, tmp_path):
    """DMBMX_ZIG pointing to a missing file means 'no zig', not a crash."""
    monkeypatch.setenv("DMBMX_ZIG", str(tmp_path / "nope.exe"))
    assert find_zig() is None
