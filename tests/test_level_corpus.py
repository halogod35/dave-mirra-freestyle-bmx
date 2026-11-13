"""DM-013: level folders round-trip byte-for-byte through the decoded object model (AD-3)."""
import math
import struct

import pytest

from conftest import GAME_DIR, corpus_files, corpus_params
from dmbmx.core.hash import zhash
from dmbmx.formats import level as lvl
from dmbmx.formats import rwworld
from dmbmx.integrity import zsd as zsdcheck

READ = {"bsp._sd": rwworld.read, "prop._sd": lvl.PropSd.read, "propdef._sd": lvl.PropDefSd.read,
        "scene._sd": lvl.SceneSd.read, "tag._sd": lvl.TagSd.read, "temp.dff": lvl.TempDff.read}


KNOWN_MISSING_TEXTURES = {"AcclaimStreet": {"trees11_2"}}


def _first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def level_dirs():
    return sorted({p.parent for p in corpus_files(".zsd") if p.parent.parent.name.lower() == "assets"})


def level_params():
    ds = level_dirs()
    if not ds:
        return [pytest.param(None, marks=pytest.mark.skip(reason="no level corpus"))]
    return [pytest.param(d, id=d.name) for d in ds]


def sd_params():
    files = [p for p in corpus_files("._sd") + corpus_files(".dff") if p.name.lower() in READ]
    if not files:
        return [pytest.param(None, marks=pytest.mark.skip(reason="no level corpus"))]
    return [pytest.param(p, id=str(p.relative_to(GAME_DIR))) for p in files]


def test_level_corpus_counts(game_dir):
    assert len(level_dirs()) == 12
    level_zsd = [p for p in corpus_files(".zsd") if p.parent.parent.name.lower() == "assets"]
    assert len(level_zsd) == 12
    names = [p.name.lower() for p in corpus_files("._sd")]
    for n in ("bsp._sd", "mat._sd", "prop._sd", "propdef._sd", "scene._sd", "tag._sd"):
        assert names.count(n) == 12, n
    assert len([p for p in corpus_files(".dff") if p.name.lower() == "temp.dff"]) == 8


@pytest.mark.parametrize("path", sd_params())
def test_sd_roundtrip(path):
    data = path.read_bytes()
    obj = READ[path.name.lower()](data)
    out = obj.write()
    assert out == data, f"first difference at 0x{_first_diff(out, data):x}"


@pytest.mark.parametrize("path", [p for p in corpus_params(".zsd")
                                  if p.values[0] is None or p.values[0].parent.parent.name.lower() == "assets"])
def test_zsd_roundtrip(path):
    data = path.read_bytes()
    z = lvl.Zsd.read(data)
    assert z.write() == data
    assert [e.type for e in z.entries] and sorted(e.type for e in z.entries) == [1, 2, 3, 4, 5, 6]
    assert z.scdf[0] == 0 and len(z.scdf) == 16
    for e in z.entries:
        assert lvl.COMPONENT_TYPES[e.type].lower() == e.name.lower()
        assert (path.parent / e.name).stat().st_size == e.size


@pytest.mark.parametrize("d", level_params())
def test_level_roundtrip(d):
    lv = lvl.Level.read_dir(d)
    for name, obj in lv.components.items():
        assert not isinstance(obj, (bytes, bytearray)), f"{name} not decoded"
    files = lv.files()
    on_disk = {p.name for p in d.iterdir() if p.is_file()}
    assert set(files) == on_disk
    for n, b in files.items():
        assert (d / n).read_bytes() == b, n


@pytest.mark.parametrize("d", level_params())
def test_level_invariants(d):
    lv = lvl.Level.read_dir(d)
    w = lv.world
    o = w.origin
    # counts in the World header == what the tree holds
    secs = w.sectors()
    assert (w.num_sectors, w.num_plane_sectors) == (len(secs), len(w.plane_sectors()))
    assert w.num_vertices == sum(len(s.vertices) for s in secs)
    assert all(len(s.coll) == rwworld.COLL_SIZE for s in secs if s.coll_present)
    # every world and prop texture resolves in Mat._sd, except one shipped data bug [V]:
    # AcclaimStreet's world material "trees11_2" has no MATI (the engine prints
    # "zrwWorldHookRpMaterials: Failed to ..." and keeps the RW default texture)
    missing = {n for n in w.texture_names() if n and lv.texture(n) is None}
    for dfn in lv.prop_defs.defs:
        for ms in dfn.model.meshes(0):
            missing |= {n for n in ms["materials"] if n and lv.texture(n) is None}
    assert missing == KNOWN_MISSING_TEXTURES.get(d.name, set())
    # the tag tree root box equals the world bbox through the ZGE mapping
    lo, hi = w.bbox()
    r = lv.tags.root
    corners = [lvl.zge_to_rw(r.bound_a, o), lvl.zge_to_rw(r.bound_b, o)]
    for i in range(3):
        assert math.isclose(min(c[i] for c in corners), lo[i], abs_tol=0.02)
        assert math.isclose(max(c[i] for c in corners), hi[i], abs_tol=0.02)
    # tag faces are a subset of the world mesh ([V] 92-99% of their vertices coincide)
    key = lambda p: tuple(round(c, 1) for c in p)  # noqa: E731
    wv = {key(v) for s in secs for v in s.vertices}
    tv = [key(lvl.zge_to_rw(v, o)) for f in lv.tags.faces for v in f.verts]
    assert sum(v in wv for v in tv) >= 0.90 * len(tv)
    # every leaf face index is valid and every face is in some leaf
    seen = set()
    for n in lv.tags.leaves():
        seen.update(n.faces)
    assert seen == set(range(len(lv.tags.faces)))
    # props: every instance has a def; ids are name hashes; rotations orthonormal
    defs = lv.prop_defs.by_id()
    for dfn in lv.prop_defs.defs:
        assert dfn.id == zhash(dfn.name)
        assert (dfn.obji is not None) == any(dfn.extents)
    for p in lv.props.props:
        assert p.def_id in defs and defs[p.def_id].name == p.name
        m = p.matrix
        det = (m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6])
               + m[2] * (m[3] * m[7] - m[4] * m[6]))
        assert abs(det - 1.0) < 1e-3
    for sp in lv.scene.points:
        assert sp.id == zhash(sp.name)
    assert lv.scene.panorama is not None


def test_level_edit_resigns_zsd(game_dir, tmp_path):
    src = game_dir / "assets" / "AcclaimDirt"
    lv = lvl.Level.read_dir(src)
    # grow Prop._sd by one prop, move another, retag a face, add an OBJI to a scene point
    p0 = lv.props.props[0]
    new = lvl.Prop(p0.def_id, p0.name, p0.matrix, (p0.pos[0] + 2.0, p0.pos[1], p0.pos[2]))
    lv.props.items.append(new)
    lv.props.props[1].pos = (0.0, 0.0, 0.0)
    lv.tags.faces[0].tag = lvl.FaceTag(0x1005, 0, 2, 0, 0)
    lv.tags.faces[1].edge_tags[0] = lvl.EdgeTag(7, 1)
    files = lv.write_dir(tmp_path)
    assert len(files["Prop._sd"]) == (src / "Prop._sd").stat().st_size + 88   # 8 + 8 + 0x48
    for e, real, ok in zsdcheck.check(tmp_path / lv.zsd_name):
        assert ok, (e, real)
    again = lvl.Level.read_dir(tmp_path)
    assert len(again.props.props) == len(lv.props.props)
    assert again.props.props[-1].pos == new.pos
    assert again.tags.faces[0].tag.code == 0x1005 and again.tags.faces[1].edge_tags[0].code == 7
    assert again.files() == files
    # untouched components stay byte-identical
    for n in ("BSP._sd", "Mat._sd", "PropDef._sd", "Scene._sd"):
        assert files[n] == (src / n).read_bytes()


def test_world_geometry_edit(game_dir):
    data = (game_dir / "assets" / "WoodwardVert" / "BSP._sd").read_bytes()
    w = rwworld.read(data)
    s = next(s for s in w.sectors() if s.triangles)
    s.vertices[0] = (s.vertices[0][0] + 1.0,) + tuple(s.vertices[0][1:])
    out = w.write()
    assert len(out) == len(data) and out != data
    w2 = rwworld.read(out)
    assert next(s for s in w2.sectors() if s.triangles).vertices[0] == s.vertices[0]
    # dropping a triangle shrinks the sector; the header count and the rebuilt BinMesh follow
    before = w.num_triangles
    s.triangles.pop()
    bm = s.rebuild_bin_mesh()
    assert sum(len(ix) for _, ix in bm.meshes) == 3 * len(s.triangles)
    w3 = rwworld.read(w.write())
    assert w3.num_triangles == before - 1
    s3 = next(x for x in w3.sectors() if x.triangles)
    assert sum(len(ix) for _, ix in s3.bin_mesh.meshes) == 3 * len(s3.triangles)
    assert struct.unpack_from("<I", out, 0)[0] == 0x0B
