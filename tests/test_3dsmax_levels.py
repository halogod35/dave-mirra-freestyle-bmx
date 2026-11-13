"""DM-060 phase 2: dmparse's level readers agree with dmbmx (DM-013) on all 12 levels: world sectors,
material slots, props and their RW placement, prop-def and panorama models, scene points, tag faces
and the colour-keyed textures."""
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "parser"))
import dmparse  # noqa: E402

from dmbmx.formats import level as L  # noqa: E402
from dmbmx.formats import texture  # noqa: E402


def level_dirs():
    root = GAME_DIR / "assets"
    if not root.is_dir():
        return [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))]
    return [pytest.param(d, id=d.name) for d in sorted(root.iterdir())
            if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())]


def test_twelve_levels():
    ds = [p.values[0] for p in level_dirs()]
    if ds and ds[0] is None:
        pytest.skip("no corpus")
    assert len(ds) == 12


def _bits(fs):
    import struct
    return struct.pack(f"<{len(fs)}f", *fs)


def _model_equal(za, zb):
    ma, mb = dmparse.model_meshes(za), list(zb.meshes(0, world=True))
    assert len(ma) == len(mb)
    for x, y in zip(ma, mb):
        assert all(abs(p - q) < 1e-9 for u, v in zip(x["positions"], y["positions"]) for p, q in zip(u, v))
        assert [v for t in x["triangles"] for v in t[:3]] == y["indices"]
        assert [t[3] for t in x["triangles"]] == y["material_ids"]
        assert [s["texture"] for s in x["slots"]] == y["materials"]


@pytest.mark.parametrize("d", level_dirs())
def test_level_matches_dmbmx(d):
    a = dmparse.read_level(str(d))
    b = L.Level.read_dir(str(d))
    w, wa = b.world, a["world"]
    assert tuple(wa["origin"]) == w.origin and wa["flags"] == w.flags
    assert [s["texture"] for s in wa["slots"]] == w.texture_names()
    assert [tuple(s["color"]) for s in wa["slots"]] == [tuple(m.color) for m in w.material_list()]
    secs = w.sectors()
    assert len(wa["sectors"]) == len(secs)
    for x, s in zip(wa["sectors"], secs):
        assert [tuple(v) for v in x["vertices"]] == [tuple(v) for v in s.vertices]
        assert (x["uvs"] or None) == ([list(u) for u in s.uvs] if s.uvs else None)
        assert (x["colors"] or None) == ([list(c) for c in s.prelit] if s.prelit else None)
        assert [tuple(n) for n in x["normals"] or []] == list(s.normal_vectors() or [])
        assert [tuple(t) for t in x["triangles"]] == [(s.mat_base + m, p, q, r) for m, p, q, r in s.triangles]
    # props + defs + placement
    assert [(p["def_id"], p["name"], tuple(p["matrix"]), tuple(p["pos"]), p["obji"]) for p in a["props"]] == \
           [(p.def_id, p.name, tuple(p.matrix), tuple(p.pos), p.obji.data if p.obji else None) for p in b.props.props]
    for x, y in zip(dmparse.prop_instances(a), b.prop_instances()):
        assert tuple(x[2]) == tuple(y[2]) and [tuple(r) for r in x[3]] == [tuple(r) for r in y[3]]
        assert (x[1]["id"] if x[1] else None) == (y[1].id if y[1] else None)
    assert len(a["prop_defs"]) == len(b.prop_defs.defs)
    for x, y in zip(a["prop_defs"], b.prop_defs.defs):
        assert (x["id"], x["name"], tuple(x["extents"]), x["obji"]) == \
               (y.id, y.name, tuple(y.extents), y.obji.data if y.obji else None)
        _model_equal(x["model"], y.model)
    # scene points + panorama
    pts = b.scene.points
    assert [(p["id"], p["type"], p["name"], tuple(p["matrix"]), tuple(p["pos"]), p["obji"]) for p in a["scene"]["points"]] == \
           [(p.id, p.type, p.name, tuple(p.matrix), tuple(p.pos), p.obji.data if p.obji else None) for p in pts]
    _model_equal(a["scene"]["panorama"], b.scene.panorama.model)
    # tags
    t = a["tags"]
    assert len(t["faces"]) == len(b.tags.faces)
    for x, y in zip(t["faces"], b.tags.faces):
        # bitwise: some shipped tag faces have NaN normals
        assert _bits(x["verts"][0] + x["verts"][1] + x["verts"][2] + x["normal"]) ==                _bits([c for v in y.verts for c in v] + list(y.normal))
        assert x["code"] == (y.tag.code if y.tag else None)
        assert x["edges"] == [e.code if e else None for e in y.edge_tags]
    assert t["leaves"] == len(b.tags.leaves()) and t["nodes"] == len(list(b.tags.root.walk()))


@pytest.mark.parametrize("d", level_dirs())
def test_color_key_matches_dmbmx(d):
    for m in dmparse.read_mat_sd((d / "Mat._sd").read_bytes()):
        if m["kind"] != "bmp":
            continue
        img = texture.decode(m["data"])
        keyed = texture.apply_color_key(img)
        png = dmparse.color_key_png(m["data"])
        if keyed.rgba == img.rgba:
            assert png is None, m["name"]
        else:
            assert texture.decode(png).rgba == keyed.rgba, m["name"]


def test_level_cli_dump(capsys):
    ds = [p.values[0] for p in level_dirs()]
    if not ds or ds[0] is None:
        pytest.skip("no corpus")
    import json
    assert dmparse.main(["dump", str(ds[0])]) == 0
    js = json.loads(capsys.readouterr().out)
    assert js["type"] == "level" and js["props"]
    assert dmparse.main(["tree", str(ds[0] / "Prop._sd")]) == 0
    out = capsys.readouterr().out
    assert "PRBL" in out
    assert dmparse.read_any((ds[0] / "BSP._sd").read_bytes(), "BSP._sd")["type"] == "bsp"
