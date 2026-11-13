"""Level folder -> OBJ/MTL/PNG and glTF 2.0 binary (.glb). Pure stdlib.

Axes: RW world space is written unchanged (Y up, like the ZMD exporter, ``docs/tools/export.md``).
Tag faces / scene points / props are converted from ZGE (engine) coordinates with
:func:`dmbmx.formats.level.zge_to_rw` / :func:`~dmbmx.formats.level.pos_to_rw` /
:func:`~dmbmx.formats.level.matrix_to_rw` (scene points); props use their stored rotation rows
(:func:`~dmbmx.formats.level.prop_matrix_to_rw`, via ``Level.prop_instances``; DM-065).

glTF layout (``export_glb``)::

    <Level>                       extras.dmbmx: level name, origin, file sizes
      World                       mesh: one primitive per material (all sectors merged)
      Props                       one node per prop instance (matrix), mesh shared per PropDef,
                                  extras: def id/name, stored ZGE matrix + pos, OBJI hex
      Tags                        one child per face-tag code: mesh of those collision faces,
                                  extras: code, class, id, flags, face count
      ScenePoints                 one empty per SCPT (matrix), extras: name, type, id, OBJI hex
      Panorama                    (optional) the sky model at the world origin
"""
from __future__ import annotations

import struct
from pathlib import Path

from dmbmx.convert import imaging
from dmbmx.convert.gltf import _Builder, EXTRAS_KEY, FLOAT, UBYTE, UINT, ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER
from dmbmx.formats import level as lvl
from dmbmx.formats import texture

__all__ = ["export_obj", "export_glb", "world_batches", "prop_batches", "tag_batches"]


def _safe(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in str(name)) or "tex"


def _png(mati, colorkey=True):
    """PNG for a MATI. With ``colorkey`` the game's colour key is applied ([V] TEXTURE.md 3:
    RGB 255,0,255 becomes transparent), so magenta-keyed cut-outs (palm leaves) look right."""
    plain = bytes(mati.image) if mati.kind == "png" else imaging.bmp_to_png(mati.image)
    if not colorkey:
        return plain
    img = texture.decode(mati.image)
    keyed = texture.apply_color_key(img)
    if keyed.rgba == img.rgba:
        return plain
    return texture.encode_png(keyed, alpha=True)


def _apply(R, t, p):
    """Row-vector rotation rows ``R`` (right, up, at) + translation ``t``."""
    return tuple(p[0] * R[0][i] + p[1] * R[1][i] + p[2] * R[2][i] + t[i] for i in range(3))


def _rot(R, n):
    return tuple(n[0] * R[0][i] + n[1] * R[1][i] + n[2] * R[2][i] for i in range(3))


class Batch:
    """Triangles of one material: ``positions``, ``normals`` (or None), ``uvs`` (or None),
    ``colors`` (RGBA bytes or None), ``indices`` (CCW), ``texture`` (name or None)."""
    __slots__ = ("texture", "positions", "normals", "uvs", "colors", "indices")

    def __init__(self, texture):
        self.texture = texture
        self.positions, self.normals, self.uvs, self.colors, self.indices = [], [], [], [], []


def _batch_add(batches, tex, P, N, T, C, tri):
    b = batches.get(tex)
    if b is None:
        b = batches[tex] = Batch(tex)
    base = len(b.positions)
    remap = {}
    for v in tri:
        if v not in remap:
            remap[v] = base + len(remap)
            b.positions.append(P[v])
            b.normals.append(N[v] if N else (0.0, 1.0, 0.0))
            b.uvs.append(T[v] if T else (0.0, 0.0))
            b.colors.append(tuple(C[v]) if C else (255, 255, 255, 255))
        b.indices.append(remap[v])


def world_batches(world):
    """World sectors merged and grouped by texture name: {texture: Batch}. Vertices are shared
    inside a sector (per texture), never across sectors."""
    names = world.texture_names()
    out, remaps = {}, {}
    for ms in world.meshes():
        P, N, T, C = ms["positions"], ms["normals"], ms["uvs"], ms["colors"]
        ix = ms["indices"]
        for k, mid in enumerate(ms["material_ids"]):
            tex = names[mid] if 0 <= mid < len(names) else None
            b = out.get(tex)
            if b is None:
                b = out[tex] = Batch(tex)
                remaps[tex] = {}
            remap = remaps[tex]
            for v in ix[3 * k:3 * k + 3]:
                key = (ms["sector"], v)
                i = remap.get(key)
                if i is None:
                    i = remap[key] = len(b.positions)
                    b.positions.append(P[v])
                    b.normals.append(N[v] if N else (0.0, 1.0, 0.0))
                    b.uvs.append(T[v] if T else (0.0, 0.0))
                    b.colors.append(tuple(C[v]) if C else (255, 255, 255, 255))
                b.indices.append(i)
    return out


def model_batches(model, R=None, t=(0.0, 0.0, 0.0)):
    """A ZMD/MODL model (LOD 0, frames baked) grouped by texture; optional placement R/t."""
    out = {}
    for ms in model.meshes(0, world=True):
        P = ms["positions"]
        N = ms["normals"]
        if R is not None:
            P = [_apply(R, t, p) for p in P]
            N = [_rot(R, n) for n in N] if N else None
        ix = ms["indices"]
        for k, mid in enumerate(ms["material_ids"]):
            tex = ms["materials"][mid] if mid < len(ms["materials"]) else None
            _batch_add(out, tex, P, N, ms["uvs"], ms["colors"], ix[3 * k:3 * k + 3])
    return out


def prop_batches(level):
    """All prop instances placed in RW space, grouped by texture: {texture: Batch}."""
    out = {}
    for p, d, t, R in level.prop_instances():
        if d is None or d.model is None:
            continue
        for tex, b in model_batches(d.model, R, t).items():
            dst = out.get(tex)
            if dst is None:
                out[tex] = b
                continue
            base = len(dst.positions)
            dst.positions += b.positions
            dst.normals += b.normals
            dst.uvs += b.uvs
            dst.colors += b.colors
            dst.indices += [i + base for i in b.indices]
    return out


def tag_batches(level):
    """Tag (collision) faces in RW space grouped by face-tag code: {code or None: Batch}."""
    o = level.world.origin
    out = {}
    for f in level.tags.faces:
        code = f.tag.code if f.tag is not None else None
        b = out.get(code)
        if b is None:
            b = out[code] = Batch(None)
        n = (f.normal[0], -f.normal[1], -f.normal[2])
        for v in f.verts:
            b.indices.append(len(b.positions))
            b.positions.append(lvl.zge_to_rw(v, o))
            b.normals.append(n)
            b.uvs.append((0.0, 0.0))
            b.colors.append((255, 255, 255, 255))
    return out


# ------------------------------------------------------------------------------- OBJ

def export_obj(level, out, *, props=True, tags=False):
    """Write ``out`` (.obj) + ``.mtl`` + ``<stem>_<texture>.png``. Returns (vertices, triangles, materials)."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    mtl = out.with_suffix(".mtl")
    lines = [f"# dmbmx level export {level.name}", f"mtllib {mtl.name}"]
    used = {}
    base = 1
    nv = nt = 0
    groups = [("world", world_batches(level.world))]
    if props and level.props is not None:
        groups.append(("props", prop_batches(level)))
    if tags and level.tags is not None:
        groups.append(("tags", {("tag_%s" % ("none" if c is None else "%04x" % c)): b
                                for c, b in tag_batches(level).items()}))
    for gname, batches in groups:
        lines.append(f"o {gname}")
        for tex, b in batches.items():
            mname = _safe(tex) if tex else "untextured"
            used[mname] = tex if gname != "tags" else None
            lines.append(f"usemtl {mname}")
            lines += ["v %.6f %.6f %.6f" % p for p in b.positions]
            lines += [f"vt {u:.6f} {1.0 - v:.6f}" for u, v in b.uvs]
            lines += ["vn %.6f %.6f %.6f" % n for n in b.normals]
            ix = b.indices
            for k in range(0, len(ix), 3):
                a, bb, c = (ix[k + j] + base for j in range(3))
                lines.append(f"f {a}/{a}/{a} {bb}/{bb}/{bb} {c}/{c}/{c}")
            base += len(b.positions)
            nv += len(b.positions)
            nt += len(ix) // 3
    out.write_text("\n".join(lines) + "\n")
    ml = []
    for mname, tex in used.items():
        ml += [f"newmtl {mname}", "Kd 1 1 1"]
        m = level.texture(tex)
        if m is not None:
            img = out.parent / f"{out.stem}_{_safe(m.name)}.png"
            img.write_bytes(_png(m))
            ml.append(f"map_Kd {img.name}")
        ml.append("")
    mtl.write_text("\n".join(ml))
    return nv, nt, len(used)


# ------------------------------------------------------------------------------- glTF

def _mesh(b, batches, gl_material, name):
    prims = []
    for tex, bt in batches.items():
        if not bt.indices:
            continue
        attrs = {
            "POSITION": b.accessor([c for p in bt.positions for c in p], FLOAT, "VEC3", target=ARRAY_BUFFER,
                                   minmax=True),
            "NORMAL": b.accessor([c for n in _unit(bt.normals) for c in n], FLOAT, "VEC3", target=ARRAY_BUFFER),
            "TEXCOORD_0": b.accessor([c for t in bt.uvs for c in t], FLOAT, "VEC2", target=ARRAY_BUFFER),
            "COLOR_0": b.accessor([c for col in bt.colors for c in col], UBYTE, "VEC4", target=ARRAY_BUFFER,
                                  normalized=True),
        }
        prims.append({"attributes": attrs, "material": gl_material(tex),
                      "indices": b.accessor(bt.indices, UINT, "SCALAR", target=ELEMENT_ARRAY_BUFFER)})
    if not prims:
        return None
    b.g["meshes"].append({"name": name, "primitives": prims})
    return len(b.g["meshes"]) - 1


def _unit(ns):
    out = []
    for n in ns:
        L = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
        out.append((n[0] / L, n[1] / L, n[2] / L) if L > 1e-9 else (0.0, 1.0, 0.0))
    return out


def _matrix(R, t):
    """Row-vector rotation rows + translation -> glTF column-major 4x4 list."""
    return [R[0][0], R[0][1], R[0][2], 0.0, R[1][0], R[1][1], R[1][2], 0.0,
            R[2][0], R[2][1], R[2][2], 0.0, t[0], t[1], t[2], 1.0]


def export_glb(level, *, props=True, tags=True, points=True, panorama=False) -> bytes:
    b = _Builder()
    g = b.g
    g["asset"]["generator"] = "dmbmx level export (DM-013)"
    files = {e.name: e.size for e in level.zsd.entries}
    root = b.node({"name": level.name or "level", "extras": {EXTRAS_KEY: {
        "format": "dmbmx-level", "version": 1, "zsd": level.zsd_name, "scfi_sizes": files,
        "origin": list(level.world.origin), "zge_scale": lvl.ZGE_SCALE,
        "axis": "RW world space (Y up); ZGE = 100 * (x - ox, -(y - oy), -(z - oz))"}}})
    tex_index = {}
    mat_index = {}
    if level.materials is not None:
        g["samplers"].append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})

    def texture(name):
        key = name.upper()
        if key not in tex_index:
            m = level.texture(name)
            if m is None:
                tex_index[key] = None
            else:
                png = _png(m)
                g["images"].append({"name": m.name, "mimeType": "image/png", "bufferView": b.view(png)})
                g["textures"].append({"sampler": 0, "source": len(g["images"]) - 1, "name": m.name})
                tex_index[key] = (len(g["textures"]) - 1, imaging.png_has_alpha(png))
        return tex_index[key]

    def gl_material(tex, color=(1.0, 1.0, 1.0, 1.0), name=None):
        key = (tex.upper() if tex else None, tuple(color))
        if key in mat_index:
            return mat_index[key]
        pbr = {"baseColorFactor": list(color), "metallicFactor": 0.0, "roughnessFactor": 1.0}
        mat = {"name": name or tex or "untextured", "pbrMetallicRoughness": pbr, "doubleSided": False}
        t = texture(tex) if tex else None
        if t:
            pbr["baseColorTexture"] = {"index": t[0]}
            if t[1] == "blend":
                mat["alphaMode"] = "BLEND"
            elif t[1] == "mask":
                mat["alphaMode"] = "MASK"
        g["materials"].append(mat)
        mat_index[key] = len(g["materials"]) - 1
        return mat_index[key]

    w = level.world
    wm = _mesh(b, world_batches(w), gl_material, "World")
    wn = {"name": "World", "extras": {EXTRAS_KEY: {"type": "world", "sectors": len(w.sectors()),
                                                  "materials": len(w.materials), "flags": w.flags}}}
    if wm is not None:
        wn["mesh"] = wm
    b.node(wn, root)

    if props and level.props is not None:
        pn = b.node({"name": "Props"}, root)
        def_mesh = {}
        for k, (p, d, t, R) in enumerate(level.prop_instances()):
            ex = {"type": "prop", "index": k, "def_id": p.def_id, "name": p.name,
                  "matrix_zge": list(p.matrix), "pos_zge100": list(p.pos),
                  "obji": p.obji.data.hex() if p.obji is not None else None}
            nd = {"name": f"{p.name}_{k}", "matrix": _matrix(R, t), "extras": {EXTRAS_KEY: ex}}
            if d is not None and d.model is not None:
                if d.id not in def_mesh:
                    def_mesh[d.id] = _mesh(b, model_batches(d.model), gl_material, f"PropDef_{d.name}")
                if def_mesh[d.id] is not None:
                    nd["mesh"] = def_mesh[d.id]
                ex["def_obji"] = d.obji.data.hex() if d.obji is not None else None
            b.node(nd, pn)

    if tags and level.tags is not None:
        tn = b.node({"name": "Tags", "extras": {EXTRAS_KEY: {"type": "tags", "faces": len(level.tags.faces),
                                                            "leaves": len(level.tags.leaves())}}}, root)
        for code, bt in sorted(tag_batches(level).items(), key=lambda kv: -1 if kv[0] is None else kv[0]):
            cname = "none" if code is None else "%04x" % code
            col = _tag_color(code)
            m = _mesh(b, {None: bt}, lambda _t, c=col, n=cname: gl_material(None, c, f"tag_{n}"), f"Tag_{cname}")
            ex = {"type": "tag", "code": code, "class": None if code is None else code >> 12,
                  "id": None if code is None else code & 0xFFF, "faces": len(bt.indices) // 3}
            nd = {"name": f"tag_{cname}", "extras": {EXTRAS_KEY: ex}}
            if m is not None:
                nd["mesh"] = m
            b.node(nd, tn)

    if points and level.scene is not None:
        sn = b.node({"name": "ScenePoints"}, root)
        o = w.origin
        for k, sp in enumerate(level.scene.points):
            R = lvl.matrix_to_rw(sp.matrix)
            t = lvl.pos_to_rw(sp.pos, o)
            ex = {"type": "scene_point", "name": sp.name, "point_type": sp.type, "id": sp.id,
                  "matrix_zge": list(sp.matrix), "pos_zge100": list(sp.pos),
                  "obji": sp.obji.data.hex() if sp.obji is not None else None}
            b.node({"name": f"{sp.name}_{k}", "matrix": _matrix(R, t), "extras": {EXTRAS_KEY: ex}}, sn)
        if panorama and level.scene.panorama is not None:
            m = _mesh(b, model_batches(level.scene.panorama.model), gl_material, "Panorama")
            nd = {"name": "Panorama", "translation": list(o)}
            if m is not None:
                nd["mesh"] = m
            b.node(nd, root)
    return b.finish()


def _tag_color(code):
    if code is None:
        return (0.6, 0.6, 0.6, 0.5)
    h = (code * 2654435761) & 0xFFFFFF
    return (((h >> 16) & 255) / 255.0, ((h >> 8) & 255) / 255.0, (h & 255) / 255.0, 0.5)
