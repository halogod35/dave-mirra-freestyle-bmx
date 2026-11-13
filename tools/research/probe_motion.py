import struct, sys, collections
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<H', d, 2)[0]
E = [struct.unpack_from('<3I', d, 4 + 12 * i) for i in range(n)]
stats = collections.Counter()
for mid, off, size in E:
    m = d[off:off + size]
    nf, fl = struct.unpack_from('<HH', m, 0)
    step, extra = struct.unpack_from('<II', m, 4)
    ns = (fl & 0xff) >> 4
    secs = [struct.unpack_from('<BBH', m, 12 + 4 * i) for i in range(ns)]
    wide = bool(fl & 0x8000)
    p = 12 + 4 * ns + (100 if nf & 0x8000 else 0)
    stats[('hdr', fl, extra, nf >> 15)] += 1
    # init records
    for (cnt, sf, kind) in secs:
        if kind == 0:
            mask = struct.unpack_from('<I', m, p)[0]
            q = p + 0x12 + cnt * (6 if wide else 3)
        else:
            if sf & 2:
                stats['skip-init'] += 1
                continue
            mask = struct.unpack_from('<I', m, p)[0]
            q = p + (12 if mask & 1 else 6)
            if mask & 2: q += 6
            if mask & 4: q += cnt * (6 if wide else 3)
        q = (q - p + 3) // 4 * 4 + p
        p = q
    nrec = 0; times = []
    while p + 4 <= len(m):
        mask = struct.unpack_from('<I', m, p)[0]
        if mask == 0:
            break
        t = struct.unpack_from('<H', m, p + 4)[0]
        sec = mask >> 28
        cnt = secs[sec][0]
        q = p + (12 if mask & 1 else 6)
        if mask & 2: q += 6
        for b in range(cnt):
            if mask & (4 << b): q += 6 if wide else 3
        q = (q - p + 3) // 4 * 4 + p
        p = q; nrec += 1; times.append(t)
    stats[('end', len(m) - p)] += 1
    if mid <= 3 or len(m) - p not in (0, 4):
        print(mid, nf, hex(fl), step, extra, secs, 'recs', nrec, 'left', len(m) - p, m[p:p+8].hex(), 'tmax', max(times) if times else None)
print(stats)

# ---- evaluation experiment: delta persistence vs reset
def s16(v): return ((v + 0x8000) & 0xFFFF) - 0x8000
def decode(m):
    nf, fl = struct.unpack_from('<HH', m, 0)
    ns = (fl & 0xff) >> 4
    secs = [struct.unpack_from('<BBH', m, 12 + 4 * i) for i in range(ns)]
    p = 12 + 4 * ns
    init = []
    for (cnt, sf, kind) in secs:
        if kind == 0:
            vals = struct.unpack_from('<3h', m, p + 12)
            rots = [struct.unpack_from('<3h', m, p + 0x12 + 6 * b) for b in range(cnt)]
            init.append((vals, rots)); q = p + 0x12 + 6 * cnt
        else:
            mask = struct.unpack_from('<I', m, p)[0]
            q = p + (12 if mask & 1 else 6); tr = None; rots = None
            if mask & 2: tr = struct.unpack_from('<3h', m, q); q += 6
            if mask & 4: rots = [struct.unpack_from('<3h', m, q + 6 * b) for b in range(cnt)]; q += 6 * cnt
            init.append((tr, rots))
        p = (q - p + 3) // 4 * 4 + p
    recs = []
    while True:
        mask = struct.unpack_from('<I', m, p)[0]
        if mask == 0: break
        t = struct.unpack_from('<H', m, p + 4)[0]; sec = mask >> 28; cnt = secs[sec][0]
        q = p + (12 if mask & 1 else 6); tr = None; rd = {}
        if mask & 2: tr = struct.unpack_from('<3h', m, q); q += 6
        for b in range(cnt):
            if mask & (4 << b): rd[b] = struct.unpack_from('<3h', m, q); q += 6
        recs.append((t, sec, tr, rd)); p = (q - p + 3) // 4 * 4 + p
    return nf, secs, init, recs

def evaluate(nf, secs, init, recs, persist):
    rot = [list(map(list, init[0][1]))]
    delta = [[0, 0, 0] for _ in rot[0]]
    frames = [[tuple(r) for r in rot[0]]]
    for k in range(1, nf):
        if not persist: delta = [[0, 0, 0] for _ in rot[0]]
        for t, sec, tr, rd in recs:
            if t == 34 * k and sec == 0:
                for b, v in rd.items(): delta[b] = list(v)
        cur = [[s16(a + d) for a, d in zip(r, dl)] for r, dl in zip(frames[-1], delta)]
        frames.append([tuple(c) for c in cur])
    return frames

def jitter(frames):
    mx = 0
    for a, b in zip(frames, frames[1:]):
        for ra, rb in zip(a, b):
            mx = max(mx, max(abs(s16(x - y)) for x, y in zip(ra, rb)))
    return mx

res = collections.Counter()
for mid, off, size in E:
    m = d[off:off + size]
    nf, secs, init, recs = decode(m)
    if nf < 6: continue
    fp, fr = evaluate(nf, secs, init, recs, True), evaluate(nf, secs, init, recs, False)
    cp = max(abs(s16(x - y)) for ra, rb in zip(fp[0], fp[-1]) for x, y in zip(ra, rb))
    cr = max(abs(s16(x - y)) for ra, rb in zip(fr[0], fr[-1]) for x, y in zip(ra, rb))
    res['persist-closer' if cp < cr else 'reset-closer'] += 1
    if mid < 6: print(mid, nf, 'closure persist', cp, 'reset', cr, 'jitter', jitter(fp), jitter(fr))
print(res)

zero = nonzero = same_as_prev = 0
absent_after_nonzero = 0
for mid, off, size in E:
    nf, secs, init, recs = decode(d[off:off + size])
    last = {}
    for t, sec, tr, rd in recs:
        if sec: continue
        for b in range(15):
            if b in rd:
                v = rd[b]
                if v == (0, 0, 0): zero += 1
                else: nonzero += 1
                if last.get(b) == v: same_as_prev += 1
                last[b] = v
            elif last.get(b, (0, 0, 0)) != (0, 0, 0):
                absent_after_nonzero += 1
print('stored zero deltas', zero, 'nonzero', nonzero, 'repeat of previous', same_as_prev, 'absent while last!=0', absent_after_nonzero)
