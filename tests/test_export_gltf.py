"""DM-020: ZMD -> .glb export. Structural checks of our own .glb output for every ZMD in the
corpus, plus the lossless restore (export -> restore_zmd == original bytes).

The Blender import check is a script (tools/blender/scripts/verify_glb.py), not part of pytest.
"""
import hashlib
import math
import struct

import pytest
from conftest import corpus_files, corpus_params

from dmbmx.convert import gltf, imaging
from dmbmx.formats import zmd

_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
_CSIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}


def validate_glb(data):
    """Structural glTF 2.0 checks on a .glb. Returns (json, bin)."""
    assert data[:4] == b"glTF" and struct.unpack_from("<I", data, 4)[0] == 2
    assert struct.unpack_from("<I", data, 8)[0] == len(data)
    jlen, jtype = struct.unpack_from("<II", data, 12)
    assert jtype == 0x4E4F534A and jlen % 4 == 0
    js, bin_ = gltf.read_glb(data)
    assert len(data) % 4 == 0
    assert js["asset"]["version"] == "2.0"
    if bin_:
        assert len(bin_) == js["buffers"][0]["byteLength"]
    for bv in js.get("bufferViews", []):
        assert bv["buffer"] == 0 and bv["byteLength"] > 0
        assert bv["byteOffset"] % 4 == 0
        assert bv["byteOffset"] + bv["byteLength"] <= len(bin_)
    for i, acc in enumerate(js.get("accessors", [])):
        bv = js["bufferViews"][acc["bufferView"]]
        size = _CSIZE[acc["componentType"]] * _NCOMP[acc["type"]]
        assert acc["count"] >= 1
        assert acc.get("byteOffset", 0) + size * acc["count"] <= bv["byteLength"], f"accessor {i} out of bounds"
        if "min" in acc:
            vals = gltf.accessor_values(js, bin_, i)
            vals = [v if isinstance(v, tuple) else (v,) for v in vals]
            n = _NCOMP[acc["type"]]
            assert [min(v[k] for v in vals) for k in range(n)] == acc["min"]
            assert [max(v[k] for v in vals) for k in range(n)] == acc["max"]
    nodes = js["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            assert 0 <= c < len(nodes) and c not in parent, f"node {c} has two parents"
            parent[c] = i
        if "matrix" in n:
            assert len(n["matrix"]) == 16 and all(math.isfinite(x) for x in n["matrix"])
            assert "rotation" not in n and "translation" not in n
        if "mesh" in n:
            assert 0 <= n["mesh"] < len(js["meshes"])
        if "skin" in n:
            assert 0 <= n["skin"] < len(js["skins"]) and "mesh" in n
    for r in js["scenes"][0]["nodes"]:
        assert r not in parent
    for i in range(len(nodes)):  # acyclic: every node reaches a root
        seen, k = set(), i
        while k in parent:
            assert k not in seen
            seen.add(k)
            k = parent[k]
        assert k in js["scenes"][0]["nodes"]
    for si, sk in enumerate(js.get("skins", [])):
        ibm = js["accessors"][sk["inverseBindMatrices"]]
        assert ibm["type"] == "MAT4" and ibm["count"] == len(sk["joints"])
        assert all(0 <= j < len(nodes) for j in sk["joints"]) and len(set(sk["joints"])) == len(sk["joints"])
    for m in js.get("meshes", []):
        for p in m["primitives"]:
            a = p["attributes"]
            nv = js["accessors"][a["POSITION"]]["count"]
            for k, ai in a.items():
                assert js["accessors"][ai]["count"] == nv, k
            assert js["accessors"][a["POSITION"]]["componentType"] == 5126
            idx = js["accessors"][p["indices"]]
            assert idx["count"] % 3 == 0 and idx["max"][0] < nv and idx["min"][0] >= 0
            assert p.get("mode", 4) == 4
            if "material" in p:
                assert 0 <= p["material"] < len(js["materials"])
            if "NORMAL" in a:
                for v in gltf.accessor_values(js, bin_, a["NORMAL"]):
                    assert abs(math.sqrt(sum(c * c for c in v)) - 1.0) < 5e-4
            if "WEIGHTS_0" in a:
                for v in gltf.accessor_values(js, bin_, a["WEIGHTS_0"]):
                    assert abs(sum(v) - 1.0) < 1e-6
    for t in js.get("textures", []):
        assert 0 <= t["source"] < len(js["images"]) and 0 <= t["sampler"] < len(js["samplers"])
    for mat in js.get("materials", []):
        bct = mat["pbrMetallicRoughness"].get("baseColorTexture")
        if bct:
            assert 0 <= bct["index"] < len(js["textures"])
    for i, im in enumerate(js.get("images", [])):
        assert im["mimeType"] == "image/png"
        png = gltf.image_bytes(js, bin_, i)
        assert png[:8] == imaging.PNG_SIG
        imaging.png_decode(png)  # CRCs, zlib stream, filters all valid
    return js, bin_


def _load(path):
    data = path.read_bytes()
    return data, zmd.read(data)


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_export_structure_and_counts(path):
    data, m = _load(path)
    if m.empty:
        with pytest.raises(ValueError):
            gltf.export_glb(m, source=data)
        return
    glb = gltf.export_glb(m, source=data, source_name=path.name)
    js, bin_ = validate_glb(glb)
    root = js["nodes"][js["scenes"][0]["nodes"][0]]
    meta = root["extras"]["dmbmx"]
    assert meta["source"]["sha256"] == hashlib.sha256(data).hexdigest()
    assert meta["source"]["size"] == len(data) and meta["complete"] is True
    # counts: per LOD, every atomic -> one node; vertices / triangles / joints match dmbmx
    lod_items = [it for it in meta["items"] if it["type"] == "MLOD"]
    assert len(lod_items) == len(m.lods)
    skins = js.get("skins", [])
    for li, it in enumerate(lod_items):
        lx = js["nodes"][it["node"]]["extras"]["dmbmx"]
        meshes = list(m.meshes(li))
        assert len(lx["atomic_nodes"]) == len(meshes)
        assert lx["frames"] == len(m.lods[li].clump.frames)
        for ms, ni in zip(meshes, lx["atomic_nodes"]):
            node = js["nodes"][ni]
            prims = js["meshes"][node["mesh"]]["primitives"]
            assert js["accessors"][prims[0]["attributes"]["POSITION"]]["count"] == len(ms["positions"])
            assert sum(js["accessors"][p["indices"]]["count"] for p in prims) == len(ms["indices"])
            if ms["joints"]:
                sk = skins[node["skin"]]
                assert len(sk["joints"]) == len(m.skeleton(li))
                jt = gltf.accessor_values(js, bin_, prims[0]["attributes"]["JOINTS_0"])
                assert [j[0] for j in jt] == [j[0] for j in ms["joints"]]
                assert max(j[0] for j in jt) < len(sk["joints"])
            else:
                assert "skin" not in node
    # images: one per MATI; TPNG embedded verbatim, TBMP converted
    for k, t in enumerate(m.materials()):
        png = gltf.image_bytes(js, bin_, k)
        if t.kind == "png":
            assert png == t.image
        else:
            assert imaging.png_info(png)["width"] == imaging.bmp_info(t.image).width


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_export_restore_lossless(path):
    """The extras + buffers are enough to rebuild the original file byte-for-byte (DM-021 baseline)."""
    data, m = _load(path)
    if m.empty:
        pytest.skip("empty file")
    assert gltf.restore_zmd(gltf.export_glb(m, source=data, source_name=path.name)) == data


def test_corpus_count():
    files = corpus_files(".zmd")
    if not files:
        pytest.skip("no corpus")
    assert len(files) == 117
    assert sum(1 for f in files if f.stat().st_size == 0) == 2


def _find(stem):
    for f in corpus_files(".zmd"):
        if f.stem.lower() == stem.lower():
            return f
    pytest.skip(f"{stem} not in corpus")


def test_rider_skin_and_upright():
    data, m = _load(_find("DaveMirra1"))
    assert gltf.upright_correction(m) == "rx-90"
    js, bin_ = gltf.read_glb(gltf.export_glb(m, source=data))
    root = js["nodes"][js["scenes"][0]["nodes"][0]]
    assert root["rotation"] == gltf.UPRIGHT_ROTATIONS["rx-90"]
    assert root["extras"]["dmbmx"]["axis"] == {"rw_to_gltf": "identity", "root_correction": "rx-90"}
    sk = js["skins"][0]
    names = [js["nodes"][j]["name"] for j in sk["joints"]]
    assert names[:3] == ["Skin", "Hips", "RightUpLeg"] and "Head" in names and len(names) == 20
    # inverse bind * joint world == identity, joint world built from the node matrices
    ibm = gltf.accessor_values(js, bin_, sk["inverseBindMatrices"])
    skel = m.skeleton()
    for b, inv in zip(skel, ibm):
        w = b["world"]
        prod = [[sum(inv[k * 4 + r] * w[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
        assert all(abs(prod[r][c] - (r == c)) < 1e-4 for r in range(4) for c in range(4))
    # after the root rotation the head is above the feet in glTF +Y
    head = next(b for b in skel if b["name"] == "Head")["world"]
    foot = next(b for b in skel if b["name"] == "LeftFoot")["world"]
    assert head[2][3] - foot[2][3] > 1.0  # model +Z ... -> glTF +Y under rx-90
    assert gltf.export_glb(m, source=data, upright=False) != gltf.export_glb(m, source=data)


def test_bike_rigid_parts_under_frames():
    data, m = _load(_find("DMbike01"))
    assert gltf.upright_correction(m) is None
    js, bin_ = gltf.read_glb(gltf.export_glb(m, source=data))
    assert "skins" not in js
    root = js["nodes"][js["scenes"][0]["nodes"][0]]
    assert "rotation" not in root
    parent = {c: i for i, n in enumerate(js["nodes"]) for c in n.get("children", [])}
    cl = m.lods[0].clump
    skel = m.skeleton()
    for ai, a in enumerate(cl.atomics):
        ni = next(i for i, n in enumerate(js["nodes"]) if n.get("extras", {}).get("dmbmx", {}).get("atomic") == ai)
        fnode = js["nodes"][parent[ni]]
        assert fnode["extras"]["dmbmx"]["frame"] == a.frame and fnode["name"] == skel[a.frame]["name"]
        assert "matrix" not in js["nodes"][ni]  # vertices are frame-local; the frame node carries the matrix
    assert {"FrontWheel", "BackWheel", "Handlebars", "Hipsdeck"} <= {n.get("name") for n in js["nodes"]}


def test_partial_lod_export_flags_incomplete():
    data, m = _load(_find("DaveMirra_shadow"))
    assert len(m.lods) == 2
    glb = gltf.export_glb(m, source=data, lods=[1])
    js, _ = validate_glb(glb)
    assert js["nodes"][0]["extras"]["dmbmx"]["complete"] is False
    with pytest.raises(gltf.GltfError):
        gltf.restore_zmd(glb)
    with pytest.raises(ValueError):
        gltf.export_glb(m, lods=[5])


def test_read_glb_rejects_garbage():
    for bad in (b"", b"glTF", b"glTF" + struct.pack("<II", 2, 99) + bytes(8), b"x" * 40):
        with pytest.raises(gltf.GltfError):
            gltf.read_glb(bad)
