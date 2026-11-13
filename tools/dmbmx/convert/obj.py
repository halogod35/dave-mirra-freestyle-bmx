"""ZMD -> Wavefront OBJ + MTL + PNG textures (simple interchange; no hierarchy/skinning).

Frame matrices are baked (``meshes(world=True)``) and the same upright correction as the glTF
export is applied, so OBJ and glTF line up after import. UVs are written as ``(u, 1 - v)``
(OBJ origin is bottom-left; RW/glTF is top-left).
"""
from __future__ import annotations

from pathlib import Path

from dmbmx.convert import imaging
from dmbmx.convert.gltf import upright_correction

__all__ = ["export_obj"]


def _safe(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name) or "tex"


def _corr(kind):
    if kind == "rx-90":      # (x, y, z) -> (x, z, -y)
        return lambda p: (p[0], p[2], -p[1])
    if kind == "rx+90":      # (x, y, z) -> (x, -z, y)
        return lambda p: (p[0], -p[2], p[1])
    return lambda p: tuple(p)


def export_obj(model, out, *, lod=0, upright=True, name=None):
    """Write ``out`` (.obj) plus ``.mtl`` and ``<stem>_<texture>.png`` files next to it.
    Returns ``(vertices, triangles, materials)``."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    mtl = out.with_suffix(".mtl")
    fix = _corr(upright_correction(model, lod) if upright else None)
    skel = model.skeleton(lod)
    used = {}
    lines = [f"# dmbmx export {name or out.stem} LOD {lod}", f"mtllib {mtl.name}"]
    base = 1
    nv = nt = 0
    for mesh in model.meshes(lod, world=True):
        lines.append(f"o {_safe(skel[mesh['frame']]['name'])}_mesh{mesh['atomic']}")
        P, N, T = mesh["positions"], mesh["normals"], mesh["uvs"]
        lines += ["v %.6f %.6f %.6f" % fix(p) for p in P]
        if T:
            lines += [f"vt {u:.6f} {1.0 - v:.6f}" for u, v in T]
        if N:
            lines += ["vn %.6f %.6f %.6f" % fix(n) for n in N]
        cur = None
        ix = mesh["indices"]
        for t, mid in enumerate(mesh["material_ids"]):
            tex = mesh["materials"][mid] if mid < len(mesh["materials"]) else None
            mname = _safe(tex) if tex else "untextured"
            if mname != cur:
                lines.append(f"usemtl {mname}")
                cur = mname
                used[mname] = tex
            a, b, c = (ix[3 * t + k] + base for k in range(3))
            if T and N:
                lines.append(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}")
            elif T:
                lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
            elif N:
                lines.append(f"f {a}//{a} {b}//{b} {c}//{c}")
            else:
                lines.append(f"f {a} {b} {c}")
        base += len(P)
        nv += len(P)
        nt += len(mesh["material_ids"])
    out.write_text("\n".join(lines) + "\n")
    ml = []
    for mname, tex in used.items():
        ml += [f"newmtl {mname}", "Kd 1 1 1"]
        t = model.texture(tex) if tex else None
        if t is not None:
            img = out.parent / f"{out.stem}_{_safe(t.name)}.png"
            img.write_bytes(t.image if t.kind == "png" else imaging.bmp_to_png(t.image))
            ml.append(f"map_Kd {img.name}")
        ml.append("")
    mtl.write_text("\n".join(ml))
    return nv, nt, len(used)
