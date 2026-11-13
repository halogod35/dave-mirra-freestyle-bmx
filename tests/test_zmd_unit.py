"""Unit tests for dmbmx.core.chunks and dmbmx.formats.zmd on synthetic data (no game files)."""
import struct

import pytest

from fuzz import fuzz_parser
from dmbmx.core import chunks as C
from dmbmx.core.errors import FormatError, LimitError, TruncatedError
from dmbmx.formats import zmd

PNG_STUB = b"\x89PNG\r\n\x1a\n" + b"\0\0\0\0IEND\xaeB\x60\x82"


def c4(tag, body):
    return tag + struct.pack("<I", len(body)) + body


def rw(t, body, ver=0x301):
    return struct.pack("<III", t, len(body), ver) + body


def test_4cc_roundtrip_with_trailing():
    data = c4(b"MODL", c4(b"MLOD", c4(b"MLDF", bytes(8))) + b"\xcc\xcc") + b"\xcc" * 3
    roots, trail = C.parse_4cc_ex(data, {b"MODL", b"MLOD"})
    assert [c.name for _, c in C.walk(roots)] == ["MODL", "MLOD", "MLDF"]
    assert roots[0].trailing == b"\xcc\xcc" and trail == b"\xcc" * 3
    assert C.write_4cc(roots, trail) == data


def test_4cc_strict_errors():
    with pytest.raises(TruncatedError):
        C.parse_4cc(c4(b"MODL", b"abcd")[:-1])
    with pytest.raises(C.ChunkError):
        C.parse_4cc(b"\x00\x01\x02\x03" + bytes(4))
    deep = c4(b"MLDF", b"")
    for _ in range(C.MAX_DEPTH + 3):
        deep = c4(b"MODL", deep)
    with pytest.raises(LimitError):
        C.parse_4cc(deep, {b"MODL"})


def test_rw_parse_and_opaque_fallback():
    good = rw(0x10, rw(1, b"\x01\x00\x00\x00") + rw(3, b""))
    (cl,) = C.parse_rw(good)
    assert cl.is_container and cl.find(1).data == b"\x01\x00\x00\x00" and cl.name == "Clump(0x10)"
    # a "container" id whose payload is not a chunk sequence stays an opaque leaf
    odd = rw(0x10, b"\xff" * 7)
    (o,) = C.parse_rw(odd)
    assert o.data == b"\xff" * 7 and C.write_rw([o]) == odd
    assert C.write_rw(C.parse_rw(good)) == good


def test_reader_bounds():
    r = C.Reader(b"\x01\x00\x00\x00", fmt="X")
    assert r.u32() == 1
    with pytest.raises(TruncatedError):
        r.u32()
    with pytest.raises(TruncatedError):
        C.Reader(b"").array("f", 1 << 40)
    with pytest.raises(TruncatedError):
        C.Reader(b"").tuples("3f", 1 << 40)


def test_name_hash():
    assert zmd.name_hash("Head") == 0x4402A5
    assert zmd.name_hash("RightHand") == 0x440CC8
    assert zmd.name_hash("DAVEMIRRA") == zmd.name_hash("DaveMirra") == 0x410D2A


def _synthetic_zmd():
    """A one-triangle textured model built purely from the object model."""
    g = zmd.Geometry()
    g.flags = zmd.GEO_POSITIONS | zmd.GEO_TEXTURED | zmd.GEO_NORMALS | zmd.GEO_LIGHT
    g.uv_sets = [[(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]]
    g.triangles = [(1, 0, 0, 2)]
    g.morph_targets = [zmd.MorphTarget((0.0, 0.0, 0.0, 1.0),
                                       [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                                       [(0.0, 0.0, 1.0)] * 3)]
    g.materials = [zmd.Material(texture=zmd.Texture("tri"))]
    g.material_indices = [-1]
    g.bin_mesh = zmd.BinMesh(0, [(0, [0, 1, 2])])
    g.ext = [C.Chunk(C.RW.BINMESH, data=b"", version=0x301)]
    cl = zmd.Clump()
    cl.frames = [zmd.Frame(anim_id=zmd.name_hash("Root"),
                           ext=[C.Chunk(C.RW.ANIMATION, data=bytes(8), version=0x301)])]
    cl.atomics = [zmd.Atomic(0, g)]
    md = zmd.Mdef()
    md.kind, md.name, md.name_raw, md.name_id = 0, "TRI", None, zmd.name_hash("TRI")
    md.params, md.unk, md.entries = (0.0, 0.0, 0.0, 0.0), 0xFFFF, [zmd.MdefEntry("Root")]
    t = zmd.Mati()
    t.madf_head = struct.pack("<I", 0x48) + bytes(64)
    t.name, t.name_raw, t.name_id, t.kind, t.img_tag = "TRI", None, zmd.name_hash("TRI"), "png", None
    t.image, t.pad = PNG_STUB, b""
    return zmd.Zmd([zmd.Lod((7, 0), cl), md, t])


def test_synthetic_build_and_read():
    data = zmd.write(_synthetic_zmd())
    m = zmd.read(data)
    assert zmd.write(m) == data
    (mesh,) = list(m.meshes())
    assert mesh["indices"] == [0, 1, 2] and mesh["materials"] == ["tri"]
    assert m.texture("TRI").kind == "png" and m.texture("tri").pad == b""  # 20-byte stub is 4-aligned
    assert m.skeleton()[0]["name"] == "Root"
    assert m.lods[0].clump.atomics[0].geometry.bin_mesh.meshes == [(0, [0, 1, 2])]


def test_empty_and_bad_magic():
    assert zmd.read(b"").empty and zmd.write(zmd.read(b"")) == b""
    with pytest.raises(FormatError):
        zmd.read(b"XXXX" + bytes(12))


def test_synthetic_fuzz():
    fuzz_parser(zmd.read, zmd.write(_synthetic_zmd()), n=300, seed=3)


def test_name_hash_is_engine_zhash():
    from dmbmx.core.hash import zhash
    for n in zmd.BONE_NAMES + ("DAVEMIRRA1", "NAME_917_0", "x"):
        assert zmd.name_hash(n) == zhash(n)


def test_bone_name_fallbacks():
    h = zmd.name_hash("LeftFoot")
    assert zmd.bone_name(h, {h: "LeftFoot01"}) == ("LeftFoot01", "mdef")
    assert zmd.bone_name(h) == ("LeftFoot", "hash")
    assert zmd.bone_name(zmd.name_hash("Tail"), extra_names=["Tail"]) == ("Tail", "hash")
    assert zmd.bone_name(0x00440AE8) == ("bone_0x00440AE8", "unknown")
    assert zmd.bone_name(zmd.NO_ID, index=5) == ("frame_5", "none")
