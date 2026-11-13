"""Blender add-on packaging + the denormal-safe frame path. The Blender run itself
(tools/blender/scripts/test_addon.py) is opt-in: set DMBMX_RUN_BLENDER=1 (it takes a few minutes)."""
import ast
import os
import struct
import subprocess
import sys
import tomllib
import zipfile
from pathlib import Path

import pytest
from conftest import corpus_files

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "blender"))
import build_blender_addon  # noqa: E402

from dmbmx.convert import gltf  # noqa: E402
from dmbmx.convert.gltf_import import _patch_frames, import_glb  # noqa: E402
from dmbmx.formats import zmd  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
BLENDER = Path(r"C:\Program Files\Blender Foundation\Blender 5.3\blender.exe")


def test_build_zip(tmp_path):
    z = build_blender_addon.build(tmp_path / "io_dmbmx.zip")
    names = zipfile.ZipFile(z).namelist()
    assert {"__init__.py", "zmd_import.py", "zmd_export.py", "blender_manifest.toml"} <= set(names)
    assert "vendor/dmbmx/convert/gltf_import.py" in names and "vendor/dmbmx/formats/zmd.py" in names
    assert not any("__pycache__" in n or n.endswith(".pyc") for n in names)
    man = tomllib.loads(zipfile.ZipFile(z).read("blender_manifest.toml").decode())
    assert man["id"] == "io_dmbmx" and man["type"] == "add-on" and man["schema_version"] == "1.0.0"
    # reproducible
    z2 = build_blender_addon.build(tmp_path / "again.zip")
    assert z.read_bytes() == z2.read_bytes()


def test_vendored_dmbmx_is_pure_stdlib():
    for p in (REPO / "tools" / "dmbmx").rglob("*.py"):
        assert build_blender_addon.third_party_imports(p) == [], p


def test_addon_sources_parse_and_declare_bl_info():
    for p in (REPO / "tools" / "blender" / "io_dmbmx").glob("*.py"):
        ast.parse(p.read_text(encoding="utf-8"))
    src = (REPO / "tools" / "blender" / "io_dmbmx" / "__init__.py").read_text(encoding="utf-8")
    assert "bl_info" in src and "import_scene.dmbmx_zmd" in src and "export_scene.dmbmx_zmd" in src


def _find(stem):
    for f in corpus_files(".zmd"):
        if f.stem.lower() == stem.lower():
            return f
    pytest.skip(f"{stem} not in corpus")


def test_raw_frame_records_match_file():
    data = _find("bike_hires").read_bytes()
    raw = gltf._raw_frame_records(data)
    m = zmd.read(data)
    assert len(raw) == len(m.lods[0].clump.frames)
    # the two denormal floats of bike_hires are in these raw records
    assert any(0 < abs(v) < 1.2e-38 for r in raw.values() for v in struct.unpack("<12f", r[:48]))


def test_patch_frames_restores_flushed_denormals():
    """Simulate a host that flushes denormals (Blender): the writer produced -0.0 / 0.0 where the
    original record had a denormal; _patch_frames puts the exact bytes back."""
    data = _find("bike_hires").read_bytes()
    raw = gltf._raw_frame_records(data)
    flushed = bytearray(data)
    k = next(k for k, r in raw.items() if any(0 < abs(v) < 1.2e-38 for v in struct.unpack("<12f", r[:48])))
    # locate the record in the file and flush its denormals
    rec = raw[k]
    o = data.index(rec)
    vals = [0.0 * (1 if v >= 0 else -1) if 0 < abs(v) < 1.2e-38 else v for v in struct.unpack("<12f", rec[:48])]
    flushed[o:o + 48] = struct.pack("<12f", *vals)
    assert bytes(flushed) != data
    assert _patch_frames(bytes(flushed), raw) == data


def test_import_keeps_raw_records(tmp_path):
    data = _find("bike_hires").read_bytes()
    out, rep = import_glb(gltf.export_glb(zmd.read(data), source=data))
    assert out == data and rep.raw_frames


@pytest.mark.skipif(os.environ.get("DMBMX_RUN_BLENDER") != "1" or not BLENDER.exists(),
                    reason="set DMBMX_RUN_BLENDER=1 to run the Blender add-on tests (needs Blender 5.3)")
def test_blender_addon_headless(tmp_path):
    z = build_blender_addon.build(tmp_path / "io_dmbmx.zip")
    r = subprocess.run([str(BLENDER), "-b", "--factory-startup", "--python", str(REPO / "tools" / "blender" / "scripts" / "test_addon.py"),
                        "--", "--zip", str(z)], capture_output=True, text=True, timeout=1800)
    tail = "\n".join(r.stdout.splitlines()[-30:])
    assert r.returncode == 0 and "ALL PASSED" in r.stdout and "ROUNDTRIP 115/115" in r.stdout, tail
