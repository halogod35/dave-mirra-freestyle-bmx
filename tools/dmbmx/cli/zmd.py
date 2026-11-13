"""dmbmx zmd {info, dump-obj, extract-textures}: ZMD model tools (DM-011)."""
import os
from pathlib import Path

from dmbmx.core.chunks import parse_4cc, parse_rw, walk
from dmbmx.formats import zmd


def register(sub):
    p = sub.add_parser("zmd", help="ZMD model files: info, OBJ dump, texture extraction")
    zs = p.add_subparsers(dest="zmd_cmd", metavar="<subcommand>")

    q = zs.add_parser("info", help="chunk tree + geometry/skeleton/texture stats")
    q.add_argument("file")
    q.add_argument("--tree", action="store_true", help="print the full chunk tree (long)")
    q.set_defaults(func=cmd_info)

    q = zs.add_parser("dump-obj", help="quick OBJ(+MTL+textures) export of one LOD")
    q.add_argument("file")
    q.add_argument("out", help="output .obj path")
    q.add_argument("--lod", type=int, default=0)
    q.add_argument("--local", action="store_true", help="don't apply frame matrices (parts at origin)")
    q.set_defaults(func=cmd_dump_obj)

    q = zs.add_parser("extract-textures", help="write the embedded TBMP/TPNG images as-is")
    q.add_argument("file")
    q.add_argument("outdir")
    q.set_defaults(func=cmd_extract_textures)

    p.set_defaults(func=lambda a: (p.print_help(), 1)[1])


def _load(path):
    return zmd.read(Path(path).read_bytes())


def cmd_info(args):
    data = Path(args.file).read_bytes()
    m = zmd.read(data)
    print(f"{args.file}: {len(data)} bytes")
    if m.empty:
        print("  empty file (0 bytes)")
        return 0
    if args.tree:
        for d, c in walk(parse_4cc(data, zmd.CONTAINERS_4CC)):
            print(f"  {'  ' * d}{c.name} {len(c.payload())}")
            if c.tag == b"MDFF":
                for rd, rc in walk(parse_rw(c.data)):
                    print(f"  {'  ' * (d + 1 + rd)}{rc.name} {len(rc.payload())} v=0x{rc.version:X}")
    for li, lod in enumerate(m.lods):
        print(f"  LOD {li}: MLDF flags=0x{lod.mldf[0]:X} rw=0x{lod.rw_version:X}")
        cl = lod.clump
        if cl is None:
            print("    (MDFF not decoded)")
            continue
        print(f"    frames={len(cl.frames)} atomics={len(cl.atomics)}")
        for ai, a in enumerate(cl.atomics):
            g = a.geometry
            plug = ",".join(c.name.split("(")[0] for c in g.ext)
            tex = sorted({mt.texture.name for mt in g.materials if mt.texture})
            print(f"    atomic {ai}: frame {a.frame} flags=0x{g.flags:X} verts={g.num_vertices} "
                  f"tris={len(g.triangles)} uvsets={len(g.uv_sets)} prelit={g.prelit is not None} "
                  f"mats={len(g.material_indices)} bones={len(g.bones or [])} ext=[{plug}] tex={tex}")
    if m.mdef:
        md = m.mdef
        print(f"  MDEF: name={md.name!r} id=0x{md.name_id:X} kind={md.kind} entries={len(md.entries)}")
        if m.lods and m.lods[0].clump:
            for b in m.skeleton():
                pos = tuple(round(b["world"][i][3], 3) for i in range(3))
                print(f"    bone {b['index']:2d} parent {b['parent']:2d} {b['name']:<18s} "
                      f"id=0x{(b['id'] or 0):06X} world_pos={pos}")
    for t in m.materials():
        print(f"  MATI: {t.name!r} id=0x{t.name_id:X} {t.kind} {len(t.image)} bytes (+{len(t.pad)} pad)")
    return 0


def _safe(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name) or "tex"


def cmd_extract_textures(args):
    m = _load(args.file)
    os.makedirs(args.outdir, exist_ok=True)
    for t in m.materials():
        out = Path(args.outdir) / f"{_safe(t.name)}.{t.kind}"
        out.write_bytes(t.image)
        print(out)
    return 0


def cmd_dump_obj(args):
    m = _load(args.file)
    if m.empty or not m.lods:
        print("nothing to export")
        return 1
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    mtl = out.with_suffix(".mtl")
    used = {}
    lines = [f"# dmbmx zmd dump-obj {Path(args.file).name} LOD {args.lod}", f"mtllib {mtl.name}"]
    base = 1
    nv = nt = 0
    for mesh in m.meshes(args.lod, world=not args.local):
        lines.append(f"o {mesh['name']}_frame{mesh['frame']}")
        P, N, T = mesh["positions"], mesh["normals"], mesh["uvs"]
        lines += [f"v {x:.6f} {y:.6f} {z:.6f}" for x, y, z in P]
        if T:
            lines += [f"vt {u:.6f} {1.0 - v:.6f}" for u, v in T]
        if N:
            lines += [f"vn {x:.6f} {y:.6f} {z:.6f}" for x, y, z in N]
        cur = None
        ix = mesh["indices"]
        for t, mid in enumerate(mesh["material_ids"]):
            name = mesh["materials"][mid] if mid < len(mesh["materials"]) else None
            mname = _safe(name) if name else "untextured"
            if mname != cur:
                lines.append(f"usemtl {mname}")
                cur = mname
                used[mname] = name
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
        t = m.texture(tex) if tex else None
        if t is not None:
            img = out.parent / f"{_safe(t.name)}.{t.kind}"
            img.write_bytes(t.image)
            ml.append(f"map_Kd {img.name}")
        ml.append("")
    mtl.write_text("\n".join(ml))
    print(f"{out}: {nv} vertices, {nt} triangles, {len(used)} materials")
    return 0
