"""DM-021: glTF (.glb) -> ZMD importer: unedited round-trip over the corpus, scripted edits,
error/limit handling, fuzz, CLI."""
import collections
import random
import struct

import pytest
from conftest import corpus_files, corpus_params
from fuzz import fuzz_parser

from dmbmx.cli import main
from dmbmx.convert import gltf, imaging
from dmbmx.convert.glb_edit import GlbDoc
from dmbmx.convert.gltf_import import (GltfImportError, GltfImportLimit, _canon, import_glb, strip_triangles,
                                       stripify)
from dmbmx.core.errors import FormatError
from dmbmx.formats import zmd


def _find(stem):
    for f in corpus_files(".zmd"):
        if f.stem.lower() == stem.lower():
            return f
    pytest.skip(f"{stem} not in corpus")


def _export(stem):
    data = _find(stem).read_bytes()
    return data, gltf.export_glb(zmd.read(data), source=data, source_name=stem)


def _geo_bytes(model, li, ai):
    return model.lods[li].clump.atomics[ai].geometry.to_chunk(model.lods[li].rw_version).to_bytes()


# ------------------------------------------------------------------------------- corpus

@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_unedited_import_is_byte_identical(path):
    data = path.read_bytes()
    m = zmd.read(data)
    if m.empty:
        pytest.skip("empty file")
    out, rep = import_glb(gltf.export_glb(m, source=data, source_name=path.name))
    assert out == data
    assert rep.changes == [] and rep.warnings == []
    assert set(rep.binmesh.values()) == {"kept"}


# ------------------------------------------------------------------------------- strips

def _grid(n):
    tris = []
    for y in range(n):
        for x in range(n):
            a, b, c, d = y * (n + 1) + x, y * (n + 1) + x + 1, (y + 1) * (n + 1) + x, (y + 1) * (n + 1) + x + 1
            tris += [(a, b, d), (a, d, c)]
    return tris


@pytest.mark.parametrize("tris", [_grid(1), _grid(4), _grid(7), [(0, 1, 2)], [(0, 1, 2), (2, 1, 0)],
                                  [(0, 1, 2), (3, 4, 5), (5, 4, 6)]])
def test_stripify_decodes_to_input(tris):
    s = stripify(tris)
    assert collections.Counter(_canon(t) for t in strip_triangles(s)) == collections.Counter(_canon(t) for t in tris)


def test_stripify_random_and_efficiency():
    rnd = random.Random(1)
    tris = _grid(10)
    rnd.shuffle(tris)
    s = stripify(tris)
    assert collections.Counter(_canon(t) for t in strip_triangles(s)) == collections.Counter(_canon(t) for t in tris)
    assert len(s) < 3 * len(tris)  # better than one triangle per sub-strip
    assert stripify([]) == [] and stripify([(1, 1, 2)]) == []


# ------------------------------------------------------------------------------- geometry edits

def test_edit_scale_bike_wheels():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    for name in ("FrontWheel", "BackWheel"):
        doc.map_positions(doc.atomic_node(frame_name=name), lambda p: (p[0] * 1.5, p[1] * 1.5, p[2] * 1.5))
    out, rep = import_glb(doc.to_bytes(), base=data)
    assert rep.warnings == []
    a, b = zmd.read(data), zmd.read(out)
    sk = a.skeleton()
    wheels = [ai for ai, at in enumerate(a.lods[0].clump.atomics) if sk[at.frame]["name"] in ("FrontWheel", "BackWheel")]
    assert len(wheels) == 2
    for ai, (x, y) in enumerate(zip(a.lods[0].clump.atomics, b.lods[0].clump.atomics)):
        gx, gy = x.geometry, y.geometry
        if ai in wheels:
            for p, q in zip(gx.morph_targets[0].vertices, gy.morph_targets[0].vertices):
                assert all(abs(q[k] - struct.unpack("<f", struct.pack("<f", p[k] * 1.5))[0]) < 1e-6 for k in range(3))
            assert gy.morph_targets[0].sphere[3] == pytest.approx(gx.morph_targets[0].sphere[3] * 1.5, rel=1e-3)
            # topology, normals, UVs, materials, BinMesh, plugins unchanged
            assert gx.triangles == gy.triangles and gx.uv_sets == gy.uv_sets
            assert gx.morph_targets[0].normals == gy.morph_targets[0].normals
            assert [c.to_bytes() for c in gx.ext] == [c.to_bytes() for c in gy.ext]
            assert gx.flags == gy.flags
        else:
            assert _geo_bytes(a, 0, ai) == _geo_bytes(b, 0, ai)
    # everything that is not geometry is identical
    assert a.mdef.encode() == b.mdef.encode()
    assert [t.to_chunk().to_bytes() for t in a.materials()] == [t.to_chunk().to_bytes() for t in b.materials()]
    fa = [(f.right, f.up, f.at, f.pos, f.parent, f.anim_id) for f in a.lods[0].clump.frames]
    fb = [(f.right, f.up, f.at, f.pos, f.parent, f.anim_id) for f in b.lods[0].clump.frames]
    assert fa == fb and a.lods[0].mldf == b.lods[0].mldf
    assert len(out) == len(data) and out != data
    assert sum(1 for c in rep.changes if "sphere" in c) == 2


@pytest.mark.parametrize("mode,flag", [("auto", 1), ("strips", 1), ("lists", 0)])
def test_edit_topology_regenerates_binmesh(mode, flag):
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    ni = doc.atomic_node(frame_name="Hipsdeck")
    prim = doc.mesh_prims(ni)[0]
    ix = doc.get(prim["indices"])
    keep = [ix[k:k + 3] for k in range(0, len(ix), 3)][::2]     # delete every other triangle
    doc.set(prim["indices"], [v for t in keep for v in t])
    out, rep = import_glb(doc.to_bytes(), binmesh=mode)
    b = zmd.read(out)
    g = b.lods[0].clump.atomics[6].geometry
    assert len(g.triangles) == len(keep)
    assert g.bin_mesh.flags == flag
    assert bool(g.flags & zmd.GEO_TRISTRIP) == bool(flag)
    assert rep.binmesh["LOD 0 atomic 6"] == ("strips" if flag else "lists")
    want = {_canon((t[1], t[0], t[3])) for t in g.triangles}
    got = set()
    for _, mix in g.bin_mesh.meshes:
        tl = strip_triangles(mix) if flag else [tuple(mix[k:k + 3]) for k in range(0, len(mix), 3)]
        got |= {_canon(t) for t in tl}
    assert got == want
    a = zmd.read(data)
    for ai in range(6):
        assert _geo_bytes(a, 0, ai) == _geo_bytes(b, 0, ai)


def test_edit_rider_joint_reassignment_resorts_vertices():
    data, glb = _export("DaveMirra1")
    a = zmd.read(data)
    sk = a.skeleton()
    head = next(b["index"] for b in sk if b["name"] == "Head")
    doc = GlbDoc(glb)
    ni = doc.atomic_node(atomic=0)
    attrs = doc.mesh_prims(ni)[0]["attributes"]
    joints = doc.get(attrs["JOINTS_0"])
    P = doc.get(attrs["POSITION"])
    joints = [(head, 0, 0, 0) if v < 3 else j for v, j in enumerate(joints)]   # 3 of the 4 _Abdomen verts -> Head
    doc.set(attrs["JOINTS_0"], joints)
    out, rep = import_glb(doc.to_bytes())
    assert any("re-sorted" in c for c in rep.changes) and any("Bone plugin" in c for c in rep.changes)
    b = zmd.read(out)
    ms = next(b.meshes())
    expect = collections.Counter((tuple(P[v]), joints[v][0]) for v in range(len(P)))
    got = collections.Counter((tuple(p), j[0]) for p, j in zip(ms["positions"], ms["joints"]))
    assert got == expect
    # triangles still connect the same positions
    ms0 = next(a.meshes())
    tri_a = collections.Counter(tuple(tuple(ms0["positions"][i]) for i in ms0["indices"][k:k + 3])
                                for k in range(0, len(ms0["indices"]), 3))
    tri_b = collections.Counter(tuple(tuple(ms["positions"][i]) for i in ms["indices"][k:k + 3])
                                for k in range(0, len(ms["indices"]), 3))
    assert tri_a == tri_b
    # Bone ranges contiguous, cover all vertices, same entry order as the original
    g = b.lods[0].clump.atomics[0].geometry
    used = [e for e in g.bones if e[1]]
    assert sum(e[1] for e in used) == g.num_vertices
    assert [e[0] for e in used] == sorted(e[0] for e in used)
    ga = a.lods[0].clump.atomics[0].geometry
    assert [e[2] for e in g.bones if e[1]] == [e[2] for e in ga.bones if e[1]]


def test_emptied_bone_entry_warns():
    data, glb = _export("DaveMirra1")
    sk = zmd.read(data).skeleton()
    head = next(b["index"] for b in sk if b["name"] == "Head")
    doc = GlbDoc(glb)
    attrs = doc.mesh_prims(doc.atomic_node(atomic=0))[0]["attributes"]
    j = doc.get(attrs["JOINTS_0"])
    doc.set(attrs["JOINTS_0"], [(head, 0, 0, 0) if v < 4 else x for v, x in enumerate(j)])  # all of _Abdomen
    out, rep = import_glb(doc.to_bytes())
    assert any("lost all their vertices" in w for w in rep.warnings)
    g = zmd.read(out).lods[0].clump.atomics[0].geometry
    assert g.bones[0] == (0, 0, 0) and len(g.bones) == 20


def test_multi_weight_vertex_takes_largest_and_warns():
    data, glb = _export("DaveMirra1")
    sk = zmd.read(data).skeleton()
    head = next(b["index"] for b in sk if b["name"] == "Head")
    doc = GlbDoc(glb)
    attrs = doc.mesh_prims(doc.atomic_node(atomic=0))[0]["attributes"]
    j = doc.get(attrs["JOINTS_0"])
    w = doc.get(attrs["WEIGHTS_0"])
    j[0] = (j[0][0], head, 0, 0)
    w[0] = (0.3, 0.7, 0.0, 0.0)
    doc.set(attrs["JOINTS_0"], j)
    doc.set(attrs["WEIGHTS_0"], w)
    out, rep = import_glb(doc.to_bytes())
    assert any("several bone weights" in x for x in rep.warnings)
    with pytest.raises(GltfImportError):
        import_glb(doc.to_bytes(), strict=True)


def test_edit_frame_transform():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    fi = doc.frame_node("FrontWheel")
    m = doc.js["nodes"][fi]["matrix"]
    m[12] += 0.25   # move the front wheel frame along +X (column-major translation)
    out, rep = import_glb(doc.to_bytes())
    a, b = zmd.read(data), zmd.read(out)
    k = doc.ex(fi)["frame"]
    fa, fb = a.lods[0].clump.frames[k], b.lods[0].clump.frames[k]
    assert fb.pos[0] == pytest.approx(fa.pos[0] + 0.25, abs=1e-6) and fb.pos[1:] == fa.pos[1:]
    assert any("transform changed" in c for c in rep.changes)
    assert all(_geo_bytes(a, 0, i) == _geo_bytes(b, 0, i) for i in range(7))


def test_partial_lod_export_needs_base():
    path = _find("DaveMirra_shadow")
    data = path.read_bytes()
    glb = gltf.export_glb(zmd.read(data), source=data, lods=[1])
    with pytest.raises(GltfImportError):
        import_glb(glb)
    out, rep = import_glb(glb, base=data)
    assert out == data and any("taken from --base" in c for c in rep.changes)


def test_base_mismatch_warns():
    data, glb = _export("DMbike01")
    other = _find("DMbike02").read_bytes()
    out, rep = import_glb(glb, base=other)
    assert out == data and any("SHA-256" in w for w in rep.warnings)


# ------------------------------------------------------------------------------- textures

def _invert_png(png):
    img = imaging.png_decode(png)
    rows = imaging.png_to_rgba_rows(img)
    inv = [bytes((255 - b) if k % 4 != 3 else b for k, b in enumerate(r)) for r in rows]
    return imaging.png_encode(img["width"], img["height"], inv, color_type=6), inv


def test_edit_tbmp_24bit_texture():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    ii = doc.image_index("NAME_431_0")
    png, inv = _invert_png(gltf.image_bytes(doc.js, bytes(doc.bin), ii))
    doc.set_image(ii, png)
    out, rep = import_glb(doc.to_bytes())
    assert any("NAME_431_0: edited (TBMP)" in c for c in rep.changes) and rep.warnings == []
    a, b = zmd.read(data), zmd.read(out)
    ta, tb = a.texture("NAME_431_0"), b.texture("NAME_431_0")
    assert tb.kind == "bmp" and len(tb.image) == len(ta.image) and tb.image[:54] == ta.image[:54]
    rows = imaging.png_decode(imaging.bmp_to_png(tb.image))["rows"]
    assert rows == [bytes(b for k, b in enumerate(r) if k % 4 != 3) for r in inv]
    for x, y in zip(a.materials(), b.materials()):
        if x.name != "NAME_431_0":
            assert x.to_chunk().to_bytes() == y.to_chunk().to_bytes()
    assert out[:8] == data[:8] or len(out) == len(data)
    assert all(_geo_bytes(a, 0, i) == _geo_bytes(b, 0, i) for i in range(7))


def test_edit_tpng_texture_written_as_is():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    ii = doc.image_index("TIRE")
    png, _ = _invert_png(gltf.image_bytes(doc.js, bytes(doc.bin), ii))
    doc.set_image(ii, png)
    out, rep = import_glb(doc.to_bytes())
    t = zmd.read(out).texture("TIRE")
    assert t.kind == "png" and t.image == png


def test_edit_palettised_texture_quantised():
    path = _find("ABBike01")
    data = path.read_bytes()
    m = zmd.read(data)
    pal = [t for t in m.materials() if t.kind == "bmp" and imaging.bmp_info(t.image).bpp <= 8]
    assert pal, "ABBike01 has a palettised TBMP"
    t0 = pal[0]
    bpp = imaging.bmp_info(t0.image).bpp
    doc = GlbDoc(gltf.export_glb(m, source=data))
    ii = doc.image_index(t0.name)
    png, _ = _invert_png(gltf.image_bytes(doc.js, bytes(doc.bin), ii))
    doc.set_image(ii, png)
    out, rep = import_glb(doc.to_bytes(), palettised="quantize")
    assert any("nearest" in w for w in rep.warnings)
    t1 = zmd.read(out).texture(t0.name)
    info = imaging.bmp_info(t1.image)
    assert info.bpp == bpp and info.palette == imaging.bmp_info(t0.image).palette
    assert len(t1.image) == len(t0.image)


def test_edit_palettised_texture_promoted_by_default():
    path = _find("ABBike01")
    data = path.read_bytes()
    m = zmd.read(data)
    t0 = next(t for t in m.materials() if t.kind == "bmp" and imaging.bmp_info(t.image).bpp <= 8)
    doc = GlbDoc(gltf.export_glb(m, source=data))
    ii = doc.image_index(t0.name)
    png, inv = _invert_png(gltf.image_bytes(doc.js, bytes(doc.bin), ii))
    doc.set_image(ii, png)
    out, rep = import_glb(doc.to_bytes())
    assert rep.warnings == [] and any("promoted to 24-bit" in c for c in rep.changes)
    t1 = zmd.read(out).texture(t0.name)
    info = imaging.bmp_info(t1.image)
    assert info.bpp == 24 and (info.width, info.height) == (imaging.bmp_info(t0.image).width, imaging.bmp_info(t0.image).height)
    rgb = imaging.png_decode(imaging.bmp_to_png(t1.image))["rows"]
    assert rgb == [bytes(b for k, b in enumerate(r) if k % 4 != 3) for r in inv]   # exact colours, no quantisation


def test_transparent_pixels_become_colour_key():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    ii = doc.image_index("NAME_431_0")
    img = imaging.png_decode(gltf.image_bytes(doc.js, bytes(doc.bin), ii))
    rows = imaging.png_to_rgba_rows(img)
    rows[0] = bytes(b if k % 4 != 3 else 0 for k, b in enumerate(rows[0]))      # first row fully transparent
    doc.set_image(ii, imaging.png_encode(img["width"], img["height"], rows, color_type=6))
    out, rep = import_glb(doc.to_bytes())
    t = zmd.read(out).texture("NAME_431_0")
    top = imaging.png_decode(imaging.bmp_to_png(t.image))["rows"][0]
    assert top == bytes((255, 0, 255)) * img["width"]
    assert any("colour key" in c for c in rep.changes)


def test_texture_resize_warns():
    data, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    ii = doc.image_index("NAME_431_0")
    doc.set_image(ii, imaging.png_encode(3, 5, [bytes(9)] * 5, color_type=2))
    out, rep = import_glb(doc.to_bytes())
    assert any("size changed" in w for w in rep.warnings) and any("power of two" in w for w in rep.warnings)
    info = imaging.bmp_info(zmd.read(out).texture("NAME_431_0").image)
    assert (info.width, info.height, info.bpp) == (3, 5, 24)


# ------------------------------------------------------------------------------- refusals

def test_missing_uvs_refused():
    _, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    for p in doc.mesh_prims(doc.atomic_node(frame_name="FrontWheel")):
        p["attributes"].pop("TEXCOORD_0")
    with pytest.raises(GltfImportError, match="TEXCOORD_0"):
        import_glb(doc.to_bytes())


def test_too_many_vertices_refused():
    _, glb = _export("sfx")
    doc = GlbDoc(glb)
    attrs = doc.mesh_prims(doc.atomic_node(atomic=0))[0]["attributes"]
    for k, ai in attrs.items():
        v = doc.get(ai)
        doc.set(ai, (v * (65536 // len(v) + 1))[:65536])
    with pytest.raises(GltfImportLimit):
        import_glb(doc.to_bytes())


def test_bone_count_change_refused():
    _, glb = _export("DaveMirra1")
    doc = GlbDoc(glb)
    doc.js["skins"][0]["joints"].pop()
    with pytest.raises(GltfImportError, match="joints"):
        import_glb(doc.to_bytes())


def test_deleted_frame_refused():
    _, glb = _export("DMbike01")
    doc = GlbDoc(glb)
    fi = doc.frame_node("LeftGrip")
    del doc.js["nodes"][fi]["extras"]
    with pytest.raises(GltfImportError, match="frames"):
        import_glb(doc.to_bytes())


def test_foreign_glb_refused():
    _, glb = _export("sfx")
    doc = GlbDoc(glb)
    for n in doc.js["nodes"]:
        n.pop("extras", None)
    with pytest.raises(GltfImportError, match="dmbmx root"):
        import_glb(doc.to_bytes())


def test_root_rotation_ignored_with_warning():
    data, glb = _export("DaveMirra1")
    doc = GlbDoc(glb)
    doc.js["nodes"][0]["rotation"] = [0.0, 0.0, 0.0, 1.0]
    out, rep = import_glb(doc.to_bytes())
    assert out == data and any("root node" in w for w in rep.warnings)


def test_fuzz_import():
    files = corpus_files(".zmd")
    if not files:
        pytest.skip("no corpus")
    data, glb = _export("sfx")
    fuzz_parser(import_glb, glb, n=200, seed=0, timeout=5.0)


# ------------------------------------------------------------------------------- CLI

def test_cli_import(tmp_path, capsys):
    src = _find("DMbike01")
    glb = tmp_path / "b.glb"
    assert main(["export", str(src), str(glb)]) == 0
    out = tmp_path / "b.zmd"
    assert main(["import", str(glb), str(out), "--base", str(src)]) == 0
    assert "identical to --base" in capsys.readouterr().out
    assert out.read_bytes() == src.read_bytes()
    # never into the install
    assert main(["import", str(glb), str(src.parent / "dmbmx_test_should_not_exist.zmd")]) == 2
    assert not (src.parent / "dmbmx_test_should_not_exist.zmd").exists()
    bad = tmp_path / "bad.glb"
    bad.write_bytes(b"glTF" + bytes(30))
    assert main(["import", str(bad), str(tmp_path / "x.zmd")]) == 1


def test_errors_are_format_errors():
    assert issubclass(GltfImportError, FormatError) and issubclass(GltfImportLimit, GltfImportError)
