"""Shared checks for the two 3ds Max importers (Python and MAXScript), run against the recording mock."""
from pathlib import Path

import dmparse

from dmbmx.formats import zmd

SCALE = 100.0


def det(rows):
    (a, b, c), (d, e, f), (g, h, i) = rows
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def check_import(path, root, frames, meshes, tex_dir, normals=False, bone_aim=True, flip=False):
    """``root``/``frames``/``meshes`` are mock scene nodes produced by an importer for LOD 0 of ``path``.
    ``normals``: the import used exact normals (check_normals on every mesh); ``flip``: with "flip
    normals to faces" (the reference is flip_ref of the game normals). ``bone_aim``: the bones were
    imported aimed at their children (check_bones). Returns the dmparse model; with ``normals`` also the
    summed (faces agreeing, faces checked) of check_normals."""
    data = Path(path).read_bytes()
    ref = zmd.read(data)
    z = dmparse.read_zmd(data)
    lo = z["lods"][0]
    skinned = any(a["geometry"]["bones"] for a in lo["atomics"])
    assert abs(det(root.transform.rows) - 1.0) < 1e-9 and root.transform.pos == (0.0, 0.0, 0.0)
    names = [s["name"] for s in dmparse.skeleton(z, 0)]
    assert len(frames) == len(lo["frames"])
    for f, node, name in zip(lo["frames"], frames, names):
        assert node.parent is (frames[f["parent"]] if f["parent"] >= 0 else root)
        assert node.kind == ("bone" if skinned else "dummy") and node.name == name
    assert len(meshes) == len(lo["atomics"])
    wind = [0, 0]
    for ai, (a, node, ms) in enumerate(zip(lo["atomics"], meshes, ref.meshes(0, world=True))):
        g = a["geometry"]
        assert len(node.verts) == g["num_vertices"] and len(node.faces) == len(g["triangles"])
        assert node.matids == [t[3] + 1 for t in g["triangles"]]
        assert [tuple(int(c) for c in f) for f in node.faces] == [(t[0] + 1, t[1] + 1, t[2] + 1) for t in g["triangles"]]
        assert len(node.smooth) == len(node.faces)
        for ch, uv in enumerate(g["uvs"], 1):
            m = node.maps[ch]
            assert m["n"] == len(uv) and len(m["faces"]) == len(node.faces)
            assert all(abs(m["verts"][i + 1][0] - uv[i][0]) < 1e-12 and abs(m["verts"][i + 1][1] - (1.0 - uv[i][1])) < 1e-6
                       for i in range(len(uv)))
        if g["colors"]:
            assert node.maps[0]["n"] == g["num_vertices"] and node.maps[-2]["n"] == g["num_vertices"]
            assert all(abs(node.maps[0]["verts"][i + 1][0] - g["colors"][i][0] / 255.0) < 1e-6 for i in range(g["num_vertices"]))
        else:
            assert 0 not in node.maps
        mm = node.material
        assert mm.kind == "multi" and len(mm.subs) == len(g["material_slots"]) and all(mm.subs)
        for slot, sub in zip(g["material_slots"], mm.subs):
            if slot["texture"]:
                tex = next(t for t in z["textures"] if t["name"].upper() == slot["texture"].upper())
                assert Path(sub.diffuseMap.filename).read_bytes() == tex["data"]
                assert str(tex_dir) in sub.diffuseMap.filename and sub.shown is True
                assert (sub.opacityMap is not None) == (tex["kind"] == "png")
        # every vertex lands where the game puts it: game world (dmbmx) * scale, through the root rotation
        xf = node.transform
        for v, w in list(zip(node.verts, ms["positions"]))[::5]:
            want = root.transform.xf(tuple(c * SCALE for c in w))
            got = xf.xf(v)
            assert all(abs(p - q) < 1e-6 * SCALE for p, q in zip(got, want)), (ai, got, want)
        en = edit_normals(node)
        if normals and ms["normals"] and any(unit(n) for n in ms["normals"]):
            assert en is not None and node.modifiers[0] is en          # below the Skin
            gn = flip_ref(ms["positions"], ms["normals"], g["triangles"]) if flip else ms["normals"]
            r = check_normals(node, gn, ms["positions"], g["triangles"], root.transform.rows)
            wind = [wind[0] + r[0], wind[1] + r[1]]
        else:
            assert en is None
        stack = [m for m in node.modifiers if m is not en]
        if g["bones"]:
            sk = stack[0]
            assert len(stack) == 1 and sk.kind == "Skin" and sk.bones == list(frames) and node.parent is root
            vf = dmparse.vertex_frames(z, 0, ai)
            assert sk.weights == {v + 1: f + 1 for v, f in enumerate(vf)}
        else:
            assert node.parent is frames[a["frame"]] and not stack
    if skinned:
        check_bones(z, 0, root, frames, meshes, bone_aim)
    return (z, tuple(wind)) if normals else z


# ----------------------------------------------------------------------------- bones (DM-065)

def bone_targets(z, lod):
    """Reference for the bone rule (README "Bones"), from dmparse's world joints: per frame
    (direction, length, main child) in game world space. Main child = among the children more than 1e-6
    from the joint, the one best continuing parent joint -> joint (largest cosine), or with the most
    descendants when the parent is a root; ties go to the first. Leaves continue their parent's direction
    at half its length; lengths >= 0.02; a lone root: (None, 0.05, None)."""
    lo = z["lods"][lod]
    fr = lo["frames"]
    sk = dmparse.skeleton(z, lod)
    J = [tuple(s["world"][k][3] for k in range(3)) for s in sk]
    n = len(fr)
    par = [f["parent"] if 0 <= f["parent"] < i else -1 for i, f in enumerate(fr)]
    kids = [[c for c in range(n) if par[c] == i] for i in range(n)]

    def ndesc(i):
        return sum(1 + ndesc(c) for c in kids[i])

    def unitv(v):
        m = _dot(v, v) ** 0.5
        return tuple(c / m for c in v)

    out = []
    for i in range(n):
        valid = [c for c in kids[i] if _dot(_sub(J[c], J[i]), _sub(J[c], J[i])) ** 0.5 > 1e-6]
        p = par[i]
        if valid:
            inc = _sub(J[i], J[p]) if p >= 0 else (0.0, 0.0, 0.0)
            if p >= 0 and par[p] >= 0 and _dot(inc, inc) > 1e-18:
                sc = [_dot(unitv(_sub(J[c], J[i])), unitv(inc)) for c in valid]
                best = valid[sc.index(max(sc))]
            else:
                dc = [ndesc(c) for c in valid]
                best = valid[dc.index(max(dc))]
            d = _sub(J[best], J[i])
            out.append((unitv(d), max(_dot(d, d) ** 0.5, 0.02), best))
        elif p >= 0 and out[p][0] is not None:
            out.append((out[p][0], max(0.5 * out[p][1], 0.02), None))
        else:
            out.append((None, 0.05, None))
    return out


def check_bones(z, lod, root, frames, meshes, aim=True):
    """Bones of a skinned import: the joints are exact; with ``aim`` each bone's X axis points at its
    main child (bone_targets) with that length, else the bone has its game frame's orientation and the
    old fixed length; the rotations are proper; and the Skin modifier deforms every vertex to where the
    mesh puts it (the game's world position, checked by check_import), via max_mock.skin_world."""
    from max_mock import skin_world
    sk = dmparse.skeleton(z, lod)
    R0 = root.transform
    targets = bone_targets(z, lod)
    for node, s, (d, length, _c) in zip(frames, sk, targets):
        w = s["world"]
        joint = tuple(w[k][3] * SCALE for k in range(3))
        assert _close(node.transform.pos, R0.xf(joint), 1e-6 * SCALE), node.name
        rows = node.transform.rows
        assert abs(det(rows) - 1.0) < 1e-5 and all(abs(_dot(rows[a], rows[b]) - (a == b)) < 1e-5
                                                  for a in range(3) for b in range(3)), node.name
        if aim:
            if d is not None:
                want = _rot(d, R0.rows)
                assert _close(rows[0], want, 1e-5), (node.name, rows[0], want)
            assert abs(node.length - length * SCALE) < 1e-6 * SCALE, (node.name, node.length, length)
        else:
            game_rows = tuple(_rot(tuple(w[k][j] for k in range(3)), R0.rows) for j in range(3))
            assert all(_close(a, b, 1e-9) for a, b in zip(rows, game_rows)), node.name
            assert node.length == 0.05 * SCALE
    for m in meshes:
        sks = [x for x in m.modifiers if x.kind == "Skin"]
        if not sks:
            continue
        got = skin_world(sks[0])
        assert len(got) == len(m.verts)
        for v, p in got.items():
            assert _close(p, m.transform.xf(m.verts[v - 1]), 1e-6 * SCALE), (m.name, v)


# ----------------------------------------------------------------------------- flip normals to faces (DM-065)

def flip_ref(positions, normals, triangles):
    """Reference for "Flip normals to faces": negate a normal whose dot product with the sum of its
    faces' CCW normals (b - a) x (c - a) is negative (area-weighted vote; a zero sum keeps it)."""
    if not normals:
        return normals
    acc = [(0.0, 0.0, 0.0)] * len(positions)
    for t in triangles:
        a, b, c = (positions[i] for i in t[:3])
        f = _cross(_sub(b, a), _sub(c, a))
        for v in t[:3]:
            acc[v] = tuple(x + y for x, y in zip(acc[v], f))
    return [None if n is None else (tuple(-c for c in n) if _dot(n, s) < 0 else tuple(n))
            for n, s in zip(normals, acc)]


def by_name(frames):
    return {n.name: n for n in frames}


def check_upright_rider(frames):
    b = by_name(frames)
    p = {k: b[k].transform.pos for k in ("Head", "LeftFoot", "LeftHand", "RightHand")}
    assert p["Head"][2] - p["LeftFoot"][2] > 1.0 * SCALE      # Max is Z-up
    assert p["LeftHand"][0] > p["RightHand"][0]               # not mirrored


def check_upright_bike(frames):
    b = by_name(frames)
    assert b["SeatGrip"].transform.pos[2] > b["BackWheel"].transform.pos[2] + 0.3 * SCALE
    assert b["FrontWheel"].transform.pos[1] < b["BackWheel"].transform.pos[1]   # front towards -Y


# ----------------------------------------------------------------------------- levels (phase 2)

def _close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check_level(folder, root, world, props, tag_nodes, points, pano, tex_dir, centre=True, normals=False,
                flip=False):
    """Checks one imported level against dmbmx (DM-013). ``normals``: the import used exact normals:
    World, every prop (instances share their PropDef's modifier) and the Panorama go through
    check_normals against dmparse; the tags have none. Returns the dmbmx level; with ``normals`` also
    {part: (faces agreeing, faces checked)}."""
    from dmbmx.formats import level as L
    from dmbmx.formats import texture
    b = L.Level.read_dir(str(folder))
    w = b.world
    o_full = w.origin
    o = o_full if centre else (0.0, 0.0, 0.0)
    R0 = root.transform
    tol = 1e-4 * SCALE

    def expect(rw):
        return R0.xf(tuple((rw[i] - o[i]) * SCALE for i in range(3)))

    assert abs(det(R0.rows) - 1.0) < 1e-9
    # world
    secs = w.sectors()
    verts = [v for s in secs for v in s.vertices]
    tris = [(s.mat_base + m) for s in secs for m, *_ in s.triangles]
    assert len(world.verts) == len(verts) and len(world.faces) == len(tris)
    assert world.matids == [m + 1 for m in tris]
    for v, rw in list(zip(world.verts, verts))[::97]:
        assert _close(world.transform.xf(v), expect(rw), tol)
    names = w.texture_names()
    mm = world.material
    assert len(mm.subs) == len(names)
    for tex, sub in zip(names, mm.subs):
        m = b.texture(tex) if tex else None
        if m is None:
            assert sub.diffuseMap is None
            continue
        assert Path(sub.diffuseMap.filename).is_file() and str(tex_dir) in sub.diffuseMap.filename
        if m.kind == "bmp":
            img = texture.decode(m.image)
            keyed = texture.apply_color_key(img)
            if keyed.rgba != img.rgba:
                assert sub.diffuseMap.filename.lower().endswith(".png") and sub.opacityMap is not None
                assert texture.decode(Path(sub.diffuseMap.filename).read_bytes()).rgba == keyed.rgba
            else:
                assert Path(sub.diffuseMap.filename).read_bytes() == m.image and sub.opacityMap is None
    assert 1 in world.maps and 0 in world.maps                      # UVs + prelit colours
    wind = {}
    dm = dmparse.read_level(str(folder)) if normals else None
    if normals:
        gn, gp, gt = [], [], []
        for sec in dm["world"]["sectors"]:
            base = len(gp)
            gp += sec["vertices"]
            gn += sec["normals"] or [None] * len(sec["vertices"])
            gt += [(a + base, b + base, c + base) for _, a, b, c in sec["triangles"]]
        if flip:
            gn = flip_ref(gp, gn, gt)
        wind["world"] = check_normals(world, gn, gp, gt, R0.rows)
    else:
        assert not world.modifiers
    # props: one node per instance, instances of one mesh per definition
    inst = b.prop_instances()
    assert len(props) == len(inst)
    first = {}
    dm_inst = dmparse.prop_instances(dm) if normals else [None] * len(inst)
    pw = [0, 0]
    for node, (p, d, t, R), di in zip(props, inst, dm_inst):
        pts = [q for ms in d.model.meshes(0, world=True) for q in ms["positions"]]
        assert len(node.verts) == len(pts)
        for v, q in list(zip(node.verts, pts))[::11]:
            rw = tuple(q[0] * R[0][i] + q[1] * R[1][i] + q[2] * R[2][i] + t[i] for i in range(3))
            assert _close(node.transform.xf(v), expect(rw), tol), (p.name, node.transform.xf(v), expect(rw))
        if d.id in first:
            assert node.verts is first[d.id].verts                 # an instance, not a copy
            assert node.modifiers is first[d.id].modifiers         # ... sharing the modifier stack
        else:
            first[d.id] = node
        if normals:
            mm = dmparse.model_meshes(di[1]["model"])
            gn = [n for m in mm for n in (m["normals"] or [None] * len(m["positions"]))]
            gp = [q for m in mm for q in m["positions"]]
            gt, base = [], 0
            for m in mm:
                gt += [(a + base, b + base, c + base) for a, b, c, _ in m["triangles"]]
                base += len(m["positions"])
            if flip:
                gn = flip_ref(gp, gn, gt)
            if any(unit(n) for n in gn):
                r = check_normals(node, gn, gp, gt, R0.rows, extra_rows=[tuple(x) for x in di[3]])
                pw = [pw[0] + r[0], pw[1] + r[1]]
            else:
                assert not node.modifiers
        else:
            assert not node.modifiers
    wind["props"] = tuple(pw)
    # tags
    from collections import Counter
    want = Counter(f.tag.code if f.tag else None for f in b.tags.faces)
    got = Counter()
    for n in tag_nodes:
        assert n.isHidden is True and n.kind == "mesh" and not n.modifiers
        code = n.userprops["dmbmx_tag_code"]
        got[None if code == "none" else int(code)] += len(n.faces)
    assert got == want
    f0 = b.tags.faces[0]
    code0 = "none" if f0.tag is None else str(f0.tag.code)
    n0 = next(n for n in tag_nodes if n.userprops["dmbmx_tag_code"] == code0)
    assert _close(n0.transform.xf(n0.verts[0]), expect(L.zge_to_rw(f0.verts[0], o_full)), tol)
    # scene points
    sp = b.scene.points
    assert [n.name for n in points] == [p.name for p in sp]
    for n, p in zip(points, sp):
        assert _close(n.transform.pos, expect(L.pos_to_rw(p.pos, o_full)), tol)
        R = L.matrix_to_rw(p.matrix)
        want_rows = [R0.xf(r) for r in R]            # rotation rows through the root (translation is 0)
        assert all(_close(a, c, 1e-6) for a, c in zip(n.transform.rows, want_rows))
    # panorama at the world origin
    pm = [q for ms in b.scene.panorama.model.meshes(0, world=True) for q in ms["positions"]]
    assert len(pano.verts) == len(pm)
    for v, q in list(zip(pano.verts, pm))[::13]:
        assert _close(pano.transform.xf(v), expect(tuple(q[i] + o_full[i] for i in range(3))), tol)
    if normals:
        mm = dmparse.model_meshes(dm["scene"]["panorama"])
        gn = [n for m in mm for n in (m["normals"] or [None] * len(m["positions"]))]
        gt, base = [], 0
        for m in mm:
            gt += [(a + base, b + base, c + base) for a, b, c, _ in m["triangles"]]
            base += len(m["positions"])
        gp = [q for m in mm for q in m["positions"]]
        if flip:
            gn = flip_ref(gp, gn, gt)
        wind["panorama"] = check_normals(pano, gn, gp, gt, R0.rows)
        return b, wind
    assert not pano.modifiers
    return b


# ----------------------------------------------------------------------------- exact normals (DM-063)

def unit(n):
    """Game normal scaled to length 1; None if missing or zero-length (what both importers apply)."""
    if n is None:
        return None
    s = sum(c * c for c in n) ** 0.5
    return None if s < 1e-6 else tuple(c / s for c in n)


def _rot(v, rows):
    return tuple(sum(v[k] * rows[k][j] for k in range(3)) for j in range(3))


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cos(a, b):
    la, lb = _dot(a, a) ** 0.5, _dot(b, b) ** 0.5
    return None if la < 1e-12 or lb < 1e-12 else _dot(a, b) / (la * lb)


def edit_normals(node):
    """The node's Edit_Normals modifier (None without one), checked to have been driven the fast way:
    every call with undo and redraw off."""
    ens = [m for m in node.modifiers if m.kind == "Edit_Normals"]
    assert len(ens) <= 1
    if not ens:
        return None
    en = ens[0]
    assert en.calls > 0 and en.calls_with_undo == 0 and en.calls_with_redraw == 0
    return en


def check_normals(node, game_world, game_pos, game_tris, root_rows, extra_rows=None):
    """Explicit normals of ``node`` against the game.

    ``game_world``: per vertex, the game normal in game (RW) world space (None = none); ``game_pos``:
    the game positions in the same space; ``game_tris``: the CCW triangles (0-based) in face order;
    ``root_rows``: game -> Max rotation (the level/model root); ``extra_rows``: for a placed prop, its
    rotation R (game_world is then in the model's space and goes through R first).

    * exactly the vertices with a usable game normal are explicit;
    * every explicit normal is unit length and, through the node's own transform, equals the game normal
      (normalised) through ``extra_rows`` and the root: right in Max world space (axes, origin offset);
    * for each face, its Max face winding (CCW, from the node's world vertices) and the explicit normals
      point the same way exactly when the game's CCW triangle and game normals do (winding and handedness
      kept). Returns (faces agreeing, faces checked) in Max."""
    en = edit_normals(node)
    want = {}
    for v, n in enumerate(game_world):
        u = unit(n)
        if u is not None:
            if extra_rows is not None:
                u = _rot(u, extra_rows)
            want[v + 1] = _rot(u, root_rows)
    got = en.result() if en is not None else {}
    assert set(got) == set(want), (node.name, len(got), len(want))
    rows = node.transform.rows
    for v, n in got.items():
        assert n is not None and abs(_dot(n, n) - 1.0) < 1e-9, (node.name, v, n)
        w = _rot(n, rows)
        assert all(abs(a - b) < 1e-6 for a, b in zip(w, want[v])), (node.name, v, w, want[v])
    agree = checked = 0
    xf = node.transform
    for f, (face, t) in enumerate(zip(node.faces, game_tris)):
        vs = [int(c) for c in face]
        assert vs == [t[0] + 1, t[1] + 1, t[2] + 1]
        if not all(v in got for v in vs):
            continue
        a, b, c = (game_pos[i] for i in t[:3])
        gs = _cos(_cross(_sub(b, a), _sub(c, a)), [sum(unit(game_world[i])[k] for i in t[:3]) for k in range(3)])
        p = [xf.xf(node.verts[v - 1]) for v in vs]
        ms = _cos(_cross(_sub(p[1], p[0]), _sub(p[2], p[0])), [sum(_rot(got[v], rows)[k] for v in vs) for k in range(3)])
        if gs is None or abs(gs) < 1e-4:
            continue                                   # degenerate face, or normals edge-on to it
        assert (gs > 0) == (ms > 0), (node.name, f)
        checked += 1
        agree += ms > 0
    return agree, checked
