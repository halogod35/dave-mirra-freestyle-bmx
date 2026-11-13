"""ZMD corpus tests (DM-011): byte-identical round-trip through the object model,
per-chunk re-encode checks, geometry/skeleton/texture invariants. Skips without the install."""
import struct

import pytest

from conftest import corpus_files, corpus_params
from dmbmx.core.chunks import RW, parse_4cc, parse_rw, walk
from dmbmx.formats import zmd


def _first_diff(a, b):
    return next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))


def test_zmd_corpus_count(game_dir):
    assert len(corpus_files(".zmd")) == 117


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_roundtrip(path):
    data = path.read_bytes()
    out = zmd.write(zmd.read(data))
    assert out == data, f"len {len(out)} vs {len(data)}, first diff at 0x{_first_diff(out, data):x}"


def _raw_mdffs(data):
    """Independent raw parse of every MDFF payload into RW chunks."""
    return [parse_rw(c.data) for _, c in walk(parse_4cc(data, zmd.CONTAINERS_4CC)) if c.tag == b"MDFF"]


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_reencode_chunks(path):
    """Each decoded struct re-encodes to the exact bytes of its original chunk
    (frame list, geometry struct, bin mesh, materials, MDEF), not only the whole file."""
    data = path.read_bytes()
    m = zmd.read(data)
    if m.empty:
        pytest.skip("zero-byte file")
    raws = _raw_mdffs(data)
    assert len(raws) == len(m.lods)
    for lod, raw in zip(m.lods, raws):
        cl = lod.clump
        assert cl is not None
        rclump = raw[0]
        assert zmd._frames_to_chunk(cl.frames, lod.rw_version).to_bytes() == rclump.find(RW.FRAMELIST).to_bytes()
        ratoms = rclump.find_all(RW.ATOMIC)
        assert len(ratoms) == len(cl.atomics)
        for a, ra in zip(cl.atomics, ratoms):
            rg = ra.find(RW.GEOMETRY)
            g = a.geometry
            assert g._encode_struct() == rg.find(RW.STRUCT).data
            if g.bin_mesh is not None:
                assert g.bin_mesh.encode() == rg.find(RW.EXTENSION).find(RW.BINMESH).data
            for mat, rmat in zip(g.materials, rg.find(RW.MATLIST).find_all(RW.MATERIAL)):
                assert zmd._material_to_chunk(mat, lod.rw_version).to_bytes() == rmat.to_bytes()
    raw_mdef = [c for c in parse_4cc(data, zmd.CONTAINERS_4CC)[0].children if c.tag == b"MDEF"]
    assert [m.mdef.encode()] == [c.data for c in raw_mdef]


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_geometry_decodes(path):
    m = zmd.read(path.read_bytes())
    if m.empty:
        pytest.skip("zero-byte file")
    assert m.lods
    for li in range(len(m.lods)):
        meshes = list(m.meshes(li))
        assert meshes
        for mesh in meshes:
            nv = len(mesh["positions"])
            assert nv > 0
            assert len(mesh["indices"]) == 3 * len(mesh["material_ids"])
            assert all(0 <= i < nv for i in mesh["indices"])
            assert all(0 <= mi < len(mesh["materials"]) for mi in mesh["material_ids"])
            for key in ("normals", "uvs", "colors"):
                assert mesh[key] is None or len(mesh[key]) == nv
            if mesh["joints"] is not None:
                nb = len(m.lods[li].clump.frames)
                assert all(0 <= j[0] < nb for j in mesh["joints"])
        sk = m.skeleton(li)
        assert all(-1 <= b["parent"] < len(sk) for b in sk)


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_textures_linked(path):
    """[V] every RW texture name has a MATI with the same (upper-cased) name; MADF ids are name hashes."""
    m = zmd.read(path.read_bytes())
    if m.empty:
        pytest.skip("zero-byte file")
    for t in m.materials():
        assert t.name_id == zmd.name_hash(t.name)
        if t.kind == "bmp":
            assert t.image[:2] == b"BM" and struct.unpack_from("<I", t.image, 2)[0] == len(t.image)
        else:
            assert t.image[:8] == b"\x89PNG\r\n\x1a\n" and t.image.endswith(b"IEND\xaeB\x60\x82")
    for lod in m.lods:
        for a in lod.clump.atomics:
            for mat in a.geometry.materials:
                if mat.texture is not None:
                    assert m.texture(mat.texture.name) is not None, mat.texture.name


def test_zmd_skinning_davemirra(game_dir):
    """Rider skeleton: 17 named bones, rigid one-bone-per-vertex skin, model-space bind pose."""
    m = zmd.read((game_dir / "assets" / "DaveMirra.ZMD").read_bytes())
    sk = m.skeleton()
    names = [b["name"] for b in sk]
    assert len(sk) == 17 and names[0] == "Skin" and "Head" in names and "LeftFoot" in names
    for b in sk:
        assert b["id"] == zmd.name_hash(b["name"])
    mesh = next(m.meshes())
    head = names.index("Head")
    hv = [p for p, j in zip(mesh["positions"], mesh["joints"]) if j[0] == head]
    feet = [p for p, j in zip(mesh["positions"], mesh["joints"]) if names[j[0]].endswith("Foot")]
    assert hv and feet
    # head vertices sit above the head joint, feet near the bottom: model space, not bone-local
    assert min(p[1] for p in hv) > sk[head]["world"][1][3] - 0.1
    assert max(p[1] for p in feet) < -0.6
    ys = [p[1] for p in mesh["positions"]]
    assert 1.6 < max(ys) - min(ys) < 2.1


def test_zmd_edit_roundtrip(game_dir):
    """Decode -> modify objects -> write -> read gives the modified values (not raw passthrough)."""
    data = (game_dir / "assets" / "bike.zmd").read_bytes()
    m = zmd.read(data)
    g = m.lods[0].clump.atomics[0].geometry
    v = g.morph_targets[0].vertices
    v[0] = (v[0][0] + 1.0, v[0][1], v[0][2])
    g.materials[0].texture.name = "RENAMED_TEX"
    m.mdef.entries[1].name = "BackWheelX"
    out = zmd.write(m)
    assert out != data
    m2 = zmd.read(out)
    g2 = m2.lods[0].clump.atomics[0].geometry
    assert g2.morph_targets[0].vertices[0] == pytest.approx(v[0])
    assert g2.materials[0].texture.name == "RENAMED_TEX"
    assert m2.mdef.entries[1].name == "BackWheelX"
    assert len(out) == len(data) + 4  # "RENAMED_TEX\0" (12) replaces "tire\0"+pad (8)


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_bone_names(path):
    """Every frame with a bone id gets a real name (MDEF or zhash reverse lookup); the only
    unresolved hash in the corpus is DaveMirra_shadow LOD 1's root (0x00440AE8)."""
    m = zmd.read(path.read_bytes())
    if m.empty:
        pytest.skip("zero-byte file")
    for li in range(len(m.lods)):
        for b in m.skeleton(li):
            if b["name_source"] == "unknown":
                assert (path.name, li, b["id"]) == ("DaveMirra_shadow.zmd", 1, 0x00440AE8)
            elif b["name_source"] in ("mdef", "hash"):
                base = b["name"].rstrip("0123456789")  # renamed copies keep the base id
                assert b["id"] in (zmd.name_hash(b["name"]), zmd.name_hash(base))
