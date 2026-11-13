"""Build the Blender add-on zip (DM-022): out/io_dmbmx.zip

Usage (repo root or worktree):  tools/testing/py.sh tools/blender/build_blender_addon.py [--out out/io_dmbmx.zip]

The zip is a Blender extension (blender_manifest.toml at the zip root) that also works as a legacy
add-on (bl_info in __init__.py). It contains tools/blender/io_dmbmx/*.py + the manifest, and the
``dmbmx`` package vendored under ``vendor/dmbmx`` (only .py files; the CLI and integrity/patch modules
are included too, they are small and pure stdlib). Install it in Blender with
Edit > Preferences > Get Extensions > (v menu) Install from Disk.
"""
import argparse
import ast
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ADDON = REPO / "tools" / "blender" / "io_dmbmx"
LIB = REPO / "tools" / "dmbmx"
STDLIB_OK = set(sys.stdlib_module_names) | {"dmbmx", "numpy"}


def third_party_imports(path):
    """Top-level imports of a module that are neither stdlib nor dmbmx (must be empty, AD-2)."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    bad = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            bad |= {a.name.split(".")[0] for a in n.names}
        elif isinstance(n, ast.ImportFrom) and n.level == 0 and n.module:
            bad.add(n.module.split(".")[0])
    return sorted(b for b in bad if b not in STDLIB_OK)


def files():
    out = [(p, p.name) for p in sorted(ADDON.glob("*.py"))] + [(ADDON / "blender_manifest.toml", "blender_manifest.toml")]
    for p in sorted(LIB.rglob("*.py")):
        if "__pycache__" in p.parts:
            continue
        out.append((p, "vendor/dmbmx/" + p.relative_to(LIB).as_posix()))
    return out


def build(out_path):
    problems = []
    for p, arc in files():
        if arc.startswith("vendor/"):
            bad = third_party_imports(p)
            if bad:
                problems.append(f"{arc}: imports {bad}")
    if problems:
        raise SystemExit("dmbmx must stay pure stdlib for the add-on:\n  " + "\n  ".join(problems))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for p, arc in files():
            # fixed timestamp -> reproducible zip
            info = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, p.read_bytes())
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO / "out" / "io_dmbmx.zip"))
    a = ap.parse_args()
    p = build(Path(a.out))
    n = len(zipfile.ZipFile(p).namelist())
    print(f"wrote {p} ({p.stat().st_size} bytes, {n} files)")


if __name__ == "__main__":
    main()
