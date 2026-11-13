"""dmbmx level {info, dump-obj, export, resign}: level folder tools."""
from collections import Counter
from pathlib import Path

from dmbmx.formats import level as lvl


def register(sub):
    p = sub.add_parser("level", help="level folders (ZSD + BSP/Mat/Prop/PropDef/Scene/Tag _sd): info, OBJ/glTF export")
    ls = p.add_subparsers(dest="level_cmd", metavar="<subcommand>")

    q = ls.add_parser("info", help="ZSD index, world, props, scene points and tag statistics")
    q.add_argument("dir", help="level folder, e.g. assets/AcclaimDirt")
    q.add_argument("--props", action="store_true", help="list every prop instance")
    q.add_argument("--points", action="store_true", help="list every scene point")
    q.set_defaults(func=cmd_info)

    q = ls.add_parser("dump-obj", help="OBJ(+MTL+PNG textures) of the world and the placed props")
    q.add_argument("dir")
    q.add_argument("out", help="output .obj path")
    q.add_argument("--no-props", action="store_true")
    q.add_argument("--tags", action="store_true", help="also write the tagged collision faces")
    q.set_defaults(func=cmd_dump_obj)

    q = ls.add_parser("export", help="glTF 2.0 binary: world, props as nodes, tags, scene points (extras)")
    q.add_argument("dir")
    q.add_argument("out", help="output .glb path")
    q.add_argument("--no-props", action="store_true")
    q.add_argument("--no-tags", action="store_true")
    q.add_argument("--no-points", action="store_true")
    q.add_argument("--panorama", action="store_true", help="include the sky panorama model")
    q.set_defaults(func=cmd_export)

    q = ls.add_parser("rewrite", help="read + write a level folder (re-encodes every file, re-signs the ZSD)")
    q.add_argument("dir")
    q.add_argument("out", help="output folder (must not be the input)")
    q.set_defaults(func=cmd_rewrite)

    p.set_defaults(func=lambda a: (p.print_help(), 1)[1])


def cmd_info(args):
    lv = lvl.Level.read_dir(args.dir)
    print(f"{args.dir}: level {lv.name}, index {lv.zsd_name}")
    for e in lv.zsd.entries:
        real = (Path(args.dir) / e.name)
        size = real.stat().st_size if real.is_file() else None
        ok = "ok" if size == e.size else f"MISMATCH (file {size})"
        print(f"  SCFI type {e.type} {e.name:<12} size {e.size:>8}  {ok}")
    w = lv.world
    if w is not None:
        lo, hi = w.bbox()
        secs = w.sectors()
        print(f"  BSP: {len(secs)} sectors ({sum(1 for s in secs if s.triangles)} non-empty), "
              f"{w.num_plane_sectors} planes, {w.num_vertices} vertices, {w.num_triangles} triangles, "
              f"{len(w.materials)} materials, flags 0x{w.flags:08X}")
        print(f"       origin ({w.origin[0]:.3f}, {w.origin[1]:.3f}, {w.origin[2]:.3f}) bbox "
              f"({lo[0]:.1f}, {lo[1]:.1f}, {lo[2]:.1f})..({hi[0]:.1f}, {hi[1]:.1f}, {hi[2]:.1f})")
        missing = sorted({n for n in w.texture_names() if n and lv.texture(n) is None})
        print(f"       textures resolved in Mat._sd: {len(w.materials) - len(missing)}/{len(w.materials)}"
              + (f" (missing {missing})" if missing else ""))
    m = lv.materials
    if m is not None and not isinstance(m, (bytes, bytearray)):
        print(f"  Mat: {len(m.materials())} textures")
    if lv.prop_defs is not None:
        defs = lv.prop_defs.defs
        print(f"  PropDef: {len(defs)} definitions ({sum(1 for d in defs if d.obji)} with OBJI)")
    if lv.props is not None:
        ps = lv.props.props
        use = Counter(p.name for p in ps)
        print(f"  Prop: {len(ps)} instances of {len(use)} defs; most used: "
              + ", ".join(f"{n} x{c}" for n, c in use.most_common(5)))
        if args.props:
            for k, (p, d, t, _R) in enumerate(lv.prop_instances()):
                print(f"    {k:3d} {p.name:<16} def 0x{p.def_id:08X} rw ({t[0]:.2f}, {t[1]:.2f}, {t[2]:.2f})"
                      + (f" obji {p.obji.data.hex()}" if p.obji else ""))
    if lv.scene is not None:
        pts = lv.scene.points
        print(f"  Scene: {len(pts)} points ({', '.join(sorted({p.name for p in pts}))}), "
              f"panorama {'yes' if lv.scene.panorama else 'no'}")
        if args.points:
            o = w.origin if w is not None else (0.0, 0.0, 0.0)
            for p in pts:
                t = lvl.pos_to_rw(p.pos, o)
                print(f"    {p.name:<10} type {p.type} rw ({t[0]:.2f}, {t[1]:.2f}, {t[2]:.2f})"
                      + (f" obji {p.obji.data.hex()}" if p.obji else ""))
    t = lv.tags
    if t is not None:
        codes = Counter(f.tag.code >> 12 if f.tag else None for f in t.faces)
        print(f"  Tag: {len(t.faces)} faces, {sum(1 for _ in t.root.walk())} nodes ({len(t.leaves())} leaves); "
              f"face-tag classes: " + ", ".join(f"{'none' if k is None else k}:{v}" for k, v in sorted(
                  codes.items(), key=lambda kv: -1 if kv[0] is None else kv[0])))
        ne = sum(1 for f in t.faces for e in f.edge_tags if e)
        nv = sum(1 for f in t.faces for e in f.vertex_tags if e)
        print(f"       edge tags {ne}, vertex tags {nv}")
    if lv.temp_dff is not None:
        print(f"  temp.dff: RW clump, {len(lv.temp_dff.clump.atomics)} atomic(s) (unreferenced)")
    return 0


def cmd_dump_obj(args):
    from dmbmx.convert.level_export import export_obj
    lv = lvl.Level.read_dir(args.dir)
    nv, nt, nm = export_obj(lv, args.out, props=not args.no_props, tags=args.tags)
    print(f"wrote {args.out}: {nv} vertices, {nt} triangles, {nm} materials")
    return 0


def cmd_export(args):
    from dmbmx.convert.level_export import export_glb
    lv = lvl.Level.read_dir(args.dir)
    data = export_glb(lv, props=not args.no_props, tags=not args.no_tags, points=not args.no_points,
                      panorama=args.panorama)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(data)
    print(f"wrote {args.out}: {len(data)} bytes")
    return 0


def cmd_rewrite(args):
    src, dst = Path(args.dir).resolve(), Path(args.out).resolve()
    if src == dst:
        print("refusing to overwrite the input folder")
        return 2
    lv = lvl.Level.read_dir(src)
    files = lv.write_dir(dst)
    same = sum(1 for n, b in files.items() if (src / n).is_file() and (src / n).read_bytes() == b)
    print(f"wrote {len(files)} files to {dst} ({same} byte-identical to the input)")
    return 0
