"""Level formats on synthetic data (no game files needed)."""
import struct

import pytest

from dmbmx.core.errors import BadMagicError, FormatError, LimitError, TruncatedError
from dmbmx.core.hash import zhash
from dmbmx.formats import level as lvl
from dmbmx.formats import rwworld


def chunk(tag, payload):
    return tag + struct.pack("<I", len(payload)) + payload


def make_zsd(entries=((4, "BSP._sd", 100), (1, "Mat._sd", 8))):
    body = chunk(b"SCDF", b"\x00" + b"\xcc" * 15)
    for t, n, s in entries:
        body += chunk(b"SCFI", struct.pack("<B3sI", t, b"\xcc\xcc\xcc", s) + (n.encode() + b"\0").ljust(32, b"\xcc"))
    return chunk(b"SCEN", body)


def test_zsd_roundtrip_and_edit():
    data = make_zsd()
    z = lvl.Zsd.read(data)
    assert [(e.type, e.name, e.size) for e in z.entries] == [(4, "BSP._sd", 100), (1, "Mat._sd", 8)]
    assert z.write() == data
    assert len(data) == 8 + 24 + 2 * 48          # SCEN size covers SCDF + every SCFI
    z.entry(4).size = 12345
    z.entry("mat._sd").name = "Other._sd"
    z2 = lvl.Zsd.read(z.write())
    assert z2.entry(4).size == 12345 and z2.entry(1).name == "Other._sd"
    # the junk after the NUL survives an unchanged name, a changed name is zero padded
    assert z2.entries[0].name_raw.endswith(b"\xcc" * 8)
    assert z2.entries[1].name_raw == b"Other._sd".ljust(32, b"\0")


def test_zsd_errors():
    with pytest.raises(BadMagicError):
        lvl.Zsd.read(b"SCEX" + bytes(4))
    bad = chunk(b"SCEN", chunk(b"SCDF", bytes(16)) + chunk(b"SCFI", bytes(0x24)))
    with pytest.raises(FormatError, match="0x28"):
        lvl.Zsd.read(bad)
    with pytest.raises(TruncatedError):
        lvl.Zsd.read(make_zsd()[:-5])
    with pytest.raises(FormatError):
        lvl.Zsd.read(b"")


def test_obji_padding():
    o = lvl.Obji(b"\x01\x02\x03\x04\x05")
    enc = o.encode()
    assert enc == struct.pack("<II", 8, 5) + b"\x01\x02\x03\x04\x05\0\0\0"
    back = lvl.Obji.decode(enc)
    assert back.data == o.data and back.pad == b"\0\0\0"
    with pytest.raises(FormatError):
        lvl.Obji.decode(struct.pack("<II", 8, 99) + bytes(4))


def make_props():
    rec = struct.pack("<II", 0x48, zhash("Rail")) + b"Rail".ljust(16, b"\0") \
        + struct.pack("<9f3f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 1.5, -2.0, 3.0)
    inner = chunk(b"PRBL", rec) + chunk(b"OBJI", lvl.Obji(bytes(10)).encode())
    return chunk(b"PRBL", inner)


def test_prop_sd():
    data = make_props()
    ps = lvl.PropSd.read(data)
    p = ps.props[0]
    assert p.name == "Rail" and p.pos == (1.5, -2.0, 3.0) and p.obji.data == bytes(10)
    assert ps.write() == data
    p.pos = (0.0, 0.0, 0.0)
    assert lvl.PropSd.read(ps.write()).props[0].pos == (0.0, 0.0, 0.0)
    assert lvl.pos_to_rw((1.0, 2.0, 3.0), (10.0, 20.0, 30.0)) == (11.0, 18.0, 27.0)
    assert lvl.zge_to_rw((100.0, 200.0, 300.0), (10.0, 20.0, 30.0)) == (11.0, 18.0, 27.0)
    # header-size field must match the engine's struct size
    bad = chunk(b"PRBL", chunk(b"PRBL", bytes(0x44)))
    with pytest.raises(FormatError, match="0x48"):
        lvl.PropSd.read(bad)


def test_matrix_to_rw_is_conjugation():
    # yaw +30 deg in ZGE axes becomes yaw -30 deg in RW axes (F M F, F = diag(1,-1,-1))
    import math
    c, s = math.cos(math.radians(30)), math.sin(math.radians(30))
    m = (c, 0, s, 0, 1, 0, -s, 0, c)
    r = lvl.matrix_to_rw(m)
    assert r[0] == pytest.approx((c, 0, -s)) and r[1] == pytest.approx((0, 1, 0)) and r[2] == pytest.approx((s, 0, c))
    assert lvl.matrix_to_rw((1, 0, 0, 0, 1, 0, 0, 0, 1)) == [(1, 0, 0), (0, 1, 0), (0, 0, 1)]


def make_scene():
    rec = struct.pack("<III", 0x4C, zhash("Player"), 1) + b"Player\0".ljust(16, b"\xff") \
        + struct.pack("<9f3f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
    return chunk(b"SCPT", chunk(b"SDPT", rec))


def test_scene_sd():
    data = make_scene()
    sc = lvl.SceneSd.read(data)
    assert sc.points[0].name == "Player" and sc.points[0].type == 1 and sc.panorama is None
    assert sc.write() == data


def make_tags():
    t = lvl.TagSd()
    f0 = lvl.TagFace([(0, 0, 0), (100, 0, 0), (0, 0, 100)], (0, -1, 0))
    f0.tag = lvl.FaceTag(0x1001, 0, 1, 0, 0)
    f0.edge_tags[1] = lvl.EdgeTag(3, 1)
    f1 = lvl.TagFace([(0, 0, 0), (0, 100, 0), (100, 0, 0)], (0, 0, 1))
    f1.vertex_tags[2] = lvl.EdgeTag(4, 0, pad=0)
    t.faces = [f0, f1]
    root = lvl.TagNode(2)
    root.split = 50.0
    root.low, root.high = lvl.TagNode(1), lvl.TagNode(1)
    root.low.faces, root.high.faces = [0, 1], [0]
    t.root = root
    return t


def test_tag_sd_build_and_roundtrip():
    t = make_tags()
    data = t.write()
    back = lvl.TagSd.read(data)
    assert back.write() == data
    assert back.faces[0].tag.code == 0x1001 and back.faces[0].edge_tags[1].code == 3
    assert back.faces[1].vertex_tags[2].code == 4
    assert [n.kind for n in back.root.walk()] == [2, 1, 1] and back.root.low.faces == [0, 1]
    # TAGI dataSize: the trailing 3-byte record's pad byte (0) is not counted
    tagi = data.rindex(b"TAGI")
    hdr, size = struct.unpack_from("<II", data, tagi + 8)
    blob_len = struct.unpack_from("<I", data, tagi + 4)[0] - 8
    assert (hdr, size, blob_len) == (8, 12 + 4 + 4 - 1, 20)


def test_tag_sd_errors():
    data = make_tags().write()
    with pytest.raises(BadMagicError):
        lvl.TagSd.read(b"TAGX" + data[4:])
    # TAGD face count disagreeing with the TAGF chunks
    i = data.index(b"TAGD")
    bad = bytearray(data)
    struct.pack_into("<I", bad, i + 12, 5)
    with pytest.raises(FormatError, match="face count"):
        lvl.TagSd.read(bytes(bad))
    # a self-referencing split chain runs out of nodes instead of recursing forever
    t = lvl.TagSd()
    t.faces = make_tags().faces
    deep = lvl.TagNode(2)
    cur = deep
    for _ in range(100):
        cur.low, cur.high = lvl.TagNode(2), lvl.TagNode(1)
        cur = cur.low
    cur.kind = 1
    t.root = deep
    with pytest.raises(LimitError):
        lvl.TagSd.read(t.write())


def test_writer_rejects_unaligned_chunks():
    ps = lvl.PropSd.read(make_props())
    ps.items.append(lvl.Chunk(b"JUNK", data=b"\x01\x02\x03"))
    with pytest.raises(ValueError, match="multiple of 4"):
        ps.write()


def tiny_world():
    """A one-sector world with one triangle, built through the object model."""
    w = rwworld.World()
    w.inv_origin = (-1.0, -2.0, -3.0)
    w.surface = (0.6, 0.0, 0.2)
    w.flags = 0x4000003C
    w.col_sector_size = 0
    w.root_is_sector = 1
    s = rwworld.AtomicSector()
    s.vertices = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)]
    s.normals = [bytes((0, 127, 0, 0xCD))] * 3
    s.prelit = [(255, 255, 255, 255)] * 3
    s.uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
    s.triangles = [(0, 0, 1, 2)]
    s.sup = (1.0, 0.0, 1.0)
    s.rebuild_bin_mesh()
    w.root = s
    w.materials = [rwworld.Material(texture=rwworld.Texture("grass"))]
    w.material_indices = [-1]
    return w


def test_world_build_roundtrip():
    w = tiny_world()
    data = w.write()
    back = rwworld.read(data)
    assert back.write() == data
    assert back.num_triangles == 1 and back.num_sectors == 1 and back.origin == (1.0, 2.0, 3.0)
    assert back.texture_names() == ["grass"]
    assert back.sectors()[0].normal_vectors()[0] == (0.0, 1.0, 0.0)
    ms = list(back.meshes())
    assert ms[0]["indices"] == [0, 1, 2] and ms[0]["material_ids"] == [0]


def test_world_errors():
    data = tiny_world().write()
    with pytest.raises(BadMagicError):
        rwworld.read(b"\x0c" + data[1:])
    with pytest.raises(FormatError):
        rwworld.read(data[:-1])
    w = tiny_world()
    w.root.vertices = [(0.0, 0.0, 0.0)] * 300
    with pytest.raises(ValueError, match="u8"):
        w.write()
