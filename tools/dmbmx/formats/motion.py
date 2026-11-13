"""Motion bank (motion.zal) and motion clips: reader / writer / evaluator. Spec: docs/formats/ANIM.md.

motion.zal holds one LZO1X group (id 0). Its payload is a *headerless* ZAL (``u16 flags,
u16 count`` + group table, offsets from byte 0) with 238 motions, ids 1..238 = the motion number the
game code uses ([V] action.c fills a table of 238 pointers in bank order, rider.c indexes it by id).

A motion clip (decoder: zactor.c ``zactorDecodeMotion`` 0x00472B10)::

    0x00 u16  frames | 0x8000 (= a 100-byte block follows the section table)
    0x02 u16  flags: bits 4-7 = number of sections; bit 15 = 16-bit rotations (else 8-bit)
    0x04 u32  frame time in ms (34)
    0x08 u32  extra offset (0)
    0x0C      sections[n]: u8 channels, u8 flags, u16 kind (0 = rider, else bike)
    ...       [100 bytes if frames & 0x8000]
    ...       init record per section, then delta records, then u32 0, u32 end time
              (+ trailing bytes)

Records start 4-byte aligned. Generic record::

    u32 mask   bits 28-31 section; bit 0: 12-byte header (u32 mask, u16 time, u16 h, f32 value) else
               6-byte (u32 mask, u16 time); bit 1: translation 3 x s16; bit 2+i: channel i present
    then translation, then 3 x s16 (or 3 x s8/u8) per present channel; zero-padded to 4.

The rider (kind 0) init record is fixed: u32 mask, u16 time, u16 h, f32 value, 3 x s16 translation,
then every channel. Init values are absolute (translation x 1/8, angles), delta records add
translation x 1/256 and angle deltas; a channel's last delta **persists** until replaced ([V] the
decoder never clears its delta buffer).
"""
from __future__ import annotations

import math
import struct

from dmbmx.core.errors import FormatError, LimitError, TruncatedError, need
from dmbmx.formats import zal

__all__ = ["MotionError", "Motion", "Record", "MotionBank", "RIDER_CHANNELS", "BIKE_CHANNELS",
           "euler_to_quat", "engine_to_model", "channel_rotation", "ARM_FIX", "ARM_FIX_LEGACY", "read_bank", "Pose"]

FMT = "MOTION"
RIDER_CHANNELS = ("Hips", "Chest", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm",
                  "RightLowArm", "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg",
                  "RightLowLeg", "RightFoot")                       # [V] NgIFzactor 0x00465130
BIKE_CHANNELS = ("Hipsdeck", "Handlebars", "Sprocket", "RightPedal", "LeftPedal", "BackWheel",
                 "FrontWheel", "LeftGrip", "RightGrip", "SeatGrip")  # [V] NgIFzactor 0x00465340
MIRROR = (0, 1, 2, 6, 7, 8, 3, 4, 5, 12, 13, 14, 9, 10, 11)          # [V] 0x0055F364 (left <-> right)
MAX_CHANNELS = 26                                                    # mask bits 2..27


class MotionError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


def _s16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


class Record:
    """One stream record. ``rots`` maps channel index -> (a, b, c) raw ints (s16, or s8/u8 in 8-bit
    mode). ``init_rider`` marks the fixed-layout rider init record (all channels, translation always)."""
    __slots__ = ("section", "mask", "time", "h", "value", "trans", "rots", "pad", "init_rider")

    def __init__(self, section=0, mask=0, time=0, h=0, value=None, trans=None, rots=None, pad=b"",
                 init_rider=False):
        self.section, self.mask, self.time, self.h, self.value = section, mask, time, h, value
        self.trans, self.rots, self.pad, self.init_rider = trans, rots or {}, pad, init_rider

    def __repr__(self):
        return f"Record(sec={self.section}, t={self.time}, mask=0x{self.mask:08x}, rots={len(self.rots)})"


class Motion:
    """A decoded motion clip; :meth:`write` reproduces the input bytes exactly."""

    def __init__(self):
        self.frames_word = 0
        self.flags = 0x9823
        self.frame_ms = 34
        self.extra = 0
        self.sections = []          # [(channels, flags, kind)]
        self.block = None           # 100 bytes when frames_word & 0x8000
        self.inits = []             # per section: Record or None (section flag 0x02 = no init record)
        self.records = []           # delta records in stream order
        self.end_time = 0
        self.trailing = b""

    # -- header fields --------------------------------------------------------
    @property
    def nframes(self):
        return self.frames_word & 0x7FFF

    @property
    def wide(self):
        """16-bit rotation values (all shipped motions); else 8-bit."""
        return bool(self.flags & 0x8000)

    @property
    def duration_ms(self):
        return self.nframes * self.frame_ms

    # -- read -----------------------------------------------------------------
    @classmethod
    def read(cls, data):
        data = bytes(data)
        n = len(data)
        need(data, 0, 12, fmt=FMT, what="motion header")
        m = cls()
        m.frames_word, m.flags, m.frame_ms, m.extra = struct.unpack_from("<HHII", data, 0)
        ns = (m.flags & 0xFF) >> 4
        p = 12
        need(data, p, 4 * ns, fmt=FMT, what="section table")
        for i in range(ns):
            m.sections.append(struct.unpack_from("<BBH", data, p + 4 * i))
        for cnt, _, _ in m.sections:
            if cnt > MAX_CHANNELS:
                raise LimitError(f"section with {cnt} channels (max {MAX_CHANNELS})", fmt=FMT, offset=p)
        p += 4 * ns
        if m.frames_word & 0x8000:
            need(data, p, 100, fmt=FMT, what="100-byte block")
            m.block = data[p:p + 100]
            p += 100
        vw = 6 if m.wide else 3
        for si, (cnt, sflags, kind) in enumerate(m.sections):
            if kind == 0:
                need(data, p, 0x12 + cnt * vw, fmt=FMT, what="rider init record")
                mask, t, h, value = struct.unpack_from("<IHHf", data, p)
                trans = struct.unpack_from("<3h", data, p + 12)
                rots = {b: m._val(data, p + 0x12 + b * vw, True) for b in range(cnt)}
                q = p + 0x12 + cnt * vw
                rec = Record(si, mask, t, h, value, trans, rots, init_rider=True)
            elif sflags & 2:
                m.inits.append(None)
                continue
            else:
                rec, q = m._read_record(data, p, si, init=True)
            rec.pad, p = m._read_pad(data, p, q)
            m.inits.append(rec)
        while True:
            need(data, p, 4, fmt=FMT, what="record mask")
            mask = struct.unpack_from("<I", data, p)[0]
            if mask == 0:
                need(data, p, 8, fmt=FMT, what="terminator")
                m.end_time = struct.unpack_from("<I", data, p + 4)[0]
                m.trailing = data[p + 8:]
                break
            si = mask >> 28
            if si >= ns:
                raise MotionError(f"record for section {si}, motion has {ns}", offset=p)
            rec, q = m._read_record(data, p, si)
            rec.pad, p = m._read_pad(data, p, q)
            m.records.append(rec)
            if len(m.records) > 100000:
                raise LimitError("too many records", fmt=FMT, offset=p)
        return m

    def _val(self, data, off, init):
        if self.wide:
            return struct.unpack_from("<3h", data, off)
        return tuple(struct.unpack_from("<3B" if init else "<3b", data, off))

    def _read_record(self, data, p, si, init=False):
        need(data, p, 6, fmt=FMT, what="record header")
        mask, t = struct.unpack_from("<IH", data, p)
        h, value, q = 0, None, p + 6
        if mask & 1:
            need(data, p, 12, fmt=FMT, what="record header")
            h, value = struct.unpack_from("<Hf", data, p + 6)
            q = p + 12
        trans = None
        if mask & 2:
            need(data, q, 6, fmt=FMT, what="translation")
            trans = struct.unpack_from("<3h", data, q)
            q += 6
        cnt = self.sections[si][0]
        vw = 6 if self.wide else 3
        rots = {}
        if init:
            if mask & 4:
                need(data, q, cnt * vw, fmt=FMT, what="init rotations")
                for b in range(cnt):
                    rots[b] = self._val(data, q, True)
                    q += vw
        else:
            for b in range(cnt):
                if mask & (4 << b):
                    need(data, q, vw, fmt=FMT, what="rotation delta")
                    rots[b] = self._val(data, q, False)
                    q += vw
        return Record(si, mask, t, h, value, trans, rots), q

    @staticmethod
    def _read_pad(data, p, q):
        end = p + ((q - p + 3) & ~3)
        need(data, q, end - q, fmt=FMT, what="record padding")
        return data[q:end], end

    # -- write ----------------------------------------------------------------
    def write(self):
        out = bytearray(struct.pack("<HHII", self.frames_word, self.flags, self.frame_ms, self.extra))
        if len(self.sections) != (self.flags & 0xFF) >> 4:
            raise MotionError("section count does not match the flags word")
        for s in self.sections:
            out += struct.pack("<BBH", *s)
        if self.frames_word & 0x8000:
            if self.block is None or len(self.block) != 100:
                raise MotionError("frames bit 15 needs a 100-byte block")
            out += self.block
        for si, rec in enumerate(self.inits):
            if rec is not None:
                out += self._write_record(rec, init=True)
        for rec in self.records:
            out += self._write_record(rec)
        out += struct.pack("<II", 0, self.end_time) + self.trailing
        return bytes(out)

    def _pack_val(self, v, init):
        if self.wide:
            return struct.pack("<3h", *v)
        return struct.pack("<3B" if init else "<3b", *v)

    def _write_record(self, rec, init=False):
        b = bytearray()
        cnt = self.sections[rec.section][0]
        if rec.init_rider:
            b += struct.pack("<IHHf", rec.mask, rec.time, rec.h, rec.value or 0.0)
            b += struct.pack("<3h", *(rec.trans or (0, 0, 0)))
            for i in range(cnt):
                b += self._pack_val(rec.rots[i], True)
        else:
            b += struct.pack("<IH", rec.mask, rec.time)
            if rec.mask & 1:
                b += struct.pack("<Hf", rec.h, rec.value or 0.0)
            if rec.mask & 2:
                b += struct.pack("<3h", *rec.trans)
            if init:
                if rec.mask & 4:
                    for i in range(cnt):
                        b += self._pack_val(rec.rots[i], True)
            else:
                for i in range(cnt):
                    if rec.mask & (4 << i):
                        b += self._pack_val(rec.rots[i], False)
        pad = rec.pad if len(rec.pad) == (-len(b)) % 4 else bytes((-len(b)) % 4)
        return bytes(b + pad)

    # -- evaluate -------------------------------------------------------------
    def sample(self):
        """Per-frame poses (list of :class:`Pose`, one per frame, frame k at k*frame_ms ms),
        following the game's decoder (zactorDecodeMotion 0x00472B10): absolute init values;
        then, per frame step, the next group of delta records (records with the time of the first
        pending one) if it is due (time <= k * frame_ms), and every channel adds its current delta.
        As in the game: one record group per step; a delta persists until replaced; a non-rider
        section with section flag 0x02 is never stepped; once the record stream is used up the
        decoder stops (the game ends the clip), so any later frames repeat the last pose. For the
        shipped bank this is the same as applying the records of time k * frame_ms at frame k
        ([V] tests/test_decomp_zactor_zprim.py)."""
        poses = []
        st = []
        for si, (cnt, sflags, kind) in enumerate(self.sections):
            rec = self.inits[si]
            rot = [[0, 0, 0] for _ in range(cnt)]
            trans = [0.0, 0.0, 0.0]
            if rec is not None:
                for b, v in rec.rots.items():
                    rot[b] = list(v) if self.wide else [_s16(x << 8) for x in v]
                if rec.trans is not None:
                    trans = [c * 0.125 for c in rec.trans]
            st.append({"kind": kind, "stepped": kind == 0 or not sflags & 2, "rot": rot,
                       "drot": [[0, 0, 0] for _ in range(cnt)], "trans": trans,
                       "dtrans": [0.0, 0.0, 0.0], "value": rec.value if rec is not None else None})
        recs = self.records
        nxt = 0
        root = [0.0, 0.0, 0.0]
        for k in range(self.nframes):
            if k and nxt < len(recs):
                if recs[nxt].time <= k * self.frame_ms:
                    t0 = recs[nxt].time
                    while nxt < len(recs) and recs[nxt].time == t0:
                        r = recs[nxt]
                        nxt += 1
                        s = st[r.section]
                        if r.value is not None:
                            s["value"] = r.value
                        if r.trans is not None:
                            s["dtrans"] = [c / 256.0 for c in r.trans]
                        for b, v in r.rots.items():
                            s["drot"][b] = list(v) if self.wide else [x << 7 for x in v]
                for s in st:
                    if not s["stepped"]:
                        continue
                    for rr, dd in zip(s["rot"], s["drot"]):
                        for i in range(3):
                            rr[i] = _s16(rr[i] + dd[i])
                    if s["kind"] == 0:
                        # x/z are per-frame displacements (actor movement), y accumulates (height)
                        root[0] += s["dtrans"][0]
                        root[2] += s["dtrans"][2]
                        s["trans"][1] += s["dtrans"][1]
                    else:
                        for i in range(3):
                            s["trans"][i] += s["dtrans"][i]
            poses.append(Pose(k * self.frame_ms, st, root))
        return poses


class Pose:
    """One frame: ``rider`` / ``bike`` = list of (x, y, z) s16 Euler angles per channel (or None),
    ``rider_trans`` = (root x, height y, root z) in motion units, ``bike_trans``, ``value``."""
    __slots__ = ("time_ms", "rider", "bike", "rider_trans", "bike_trans", "value")

    def __init__(self, t, st, root):
        self.time_ms = t
        self.rider = self.bike = self.rider_trans = self.bike_trans = self.value = None
        for s in st:
            rots = [tuple(r) for r in s["rot"]]
            if s["kind"] == 0:
                self.rider = rots
                self.rider_trans = (s["trans"][0] + root[0], s["trans"][1], s["trans"][2] + root[2])
                self.value = s["value"]
            else:
                self.bike = rots
                self.bike_trans = tuple(s["trans"])


# --------------------------------------------------------------------------- rotations

def euler_to_quat(a):
    """[V] zquatFromEuler 0x004832B0: s16 angles (65536 = 360 deg) -> quaternion (x, y, z, w) = qz * qy * qx
    (exact maths; the exe goes through its 8192-entry sine table, within 1.7e-3)."""
    hx, hy, hz = (v * math.pi / 65536.0 for v in a)      # half angles
    cx, sx = math.cos(hx), math.sin(hx)
    cy, sy = math.cos(hy), math.sin(hy)
    cz, sz = math.cos(hz), math.sin(hz)
    return (sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz)


def quat_mul(a, b):
    """Hamilton product a*b, (x, y, z, w) layout ([V] zquatMul 0x00483070)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def engine_to_model(q):
    """[V] zrwModelQuatToRwMatrix 0x00487E70: the engine's node quaternion (Y-down axes) becomes the
    RenderWare frame rotation as (x, -y, -z, w), i.e. R_model = F R(q) F with F = diag(1, -1, -1)."""
    return (q[0], -q[1], -q[2], q[3])


_ZP = euler_to_quat((0, 0, 0x4000))
_ZM = euler_to_quat((0, 0, -0x4000))
# Arm corrections, [V] NgIFzactor_ApplyMotionPose 0x004654B0: UpArm q*C, LowArm/Hand C^-1*q*C with
# C = Cz(+90) left / Cz(-90) right. With the engine's F conjugation (engine_to_model) on every channel this
# puts the hands on the handlebars (docs/formats/ANIM.md section 3.1).
ARM_FIX = {
    "LeftUpArm": (None, _ZP), "LeftLowArm": (_ZM, _ZP), "LeftHand": (_ZM, _ZP),
    "RightUpArm": (None, _ZM), "RightLowArm": (_ZP, _ZM), "RightHand": (_ZP, _ZM),
}
ARM_FIX_AS_DECOMPILED = ARM_FIX      # the earlier name for the same (literal) table
# Legacy (superseded by the F conjugation): an empirical Rx(180) before the upper arms, used *without* the F
# conjugation. It approximated the missing leading F for the arms only. Select with legacy=True.
_X180 = euler_to_quat((-0x8000, 0, 0))
ARM_FIX_LEGACY = {
    "LeftUpArm": (_X180, _ZP), "LeftLowArm": (_ZM, _ZP), "LeftHand": (_ZM, _ZP),
    "RightUpArm": (_X180, _ZM), "RightLowArm": (_ZP, _ZM), "RightHand": (_ZP, _ZM),
}


def channel_rotation(name, euler, arm_fix=None, *, legacy=False, frf=None):
    """Local frame rotation (x, y, z, w), in model (RenderWare/glTF) axes, that the game gives the frame of
    channel ``name`` (rider or bike) for its Euler angles: Euler -> quaternion, the NgIFzactor arm table,
    then the engine -> model conjugation F q F (:func:`engine_to_model`).

    ``legacy=True`` reproduces the legacy chain (Rx(180) arm table, no F). ``arm_fix`` overrides the table and
    ``frf`` the conjugation (default: on unless legacy)."""
    q = euler_to_quat(euler)
    table = arm_fix if arm_fix is not None else (ARM_FIX_LEGACY if legacy else ARM_FIX)
    pre, post = table.get(name, (None, None))
    if pre is not None:
        q = quat_mul(pre, q)
    if post is not None:
        q = quat_mul(q, post)
    if frf if frf is not None else not legacy:
        q = engine_to_model(q)
    return q


# --------------------------------------------------------------------------- bank

class MotionBank:
    """motion.zal: ``archive`` (outer ZAL), ``bank`` (headerless inner ZAL), ``motions`` {id: Motion}."""

    def __init__(self, archive, bank):
        self.archive, self.bank = archive, bank

    @classmethod
    def read(cls, data):
        arc = zal.read(data)
        if not arc.entries:
            raise MotionError("motion.zal has no groups")
        bank = zal.ZalArchive.read(arc.entries[0].data(), headerless=True)
        return cls(arc, bank)

    @property
    def ids(self):
        return [e.id for e in self.bank.entries]

    def motion(self, mid):
        try:
            return Motion.read(self.bank.find(mid).data())
        except KeyError:
            raise MotionError(f"no motion {mid}") from None

    def set_motion(self, mid, motion):
        """Replace one motion; the bank is re-encoded (LZO1X) into the outer archive on write()."""
        e = self.bank.find(mid)
        payload = motion.write()
        a = e.assets[0]
        a.stored, a.size_field = payload, len(payload)
        self.archive.set_data(self.archive.entries[0].id, self.bank.write())

    def write(self):
        return self.archive.write()


def read_bank(data):
    return MotionBank.read(data)


# --------------------------------------------------------------------------- forward kinematics (F conjugation)

def _qmat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]


def pose_matrices(skeleton, rotations):
    """World 4x4 matrices (column-vector convention, model space) of ``Zmd.skeleton()`` frames when the
    frames named in ``rotations`` ({name: quaternion}) get their local rotation replaced, the way the game
    applies motion channels (bind translations kept)."""
    W = []
    for b in skeleton:
        L = [row[:] for row in b["matrix"]]
        q = rotations.get(b["name"])
        if q is not None:
            R = _qmat(q)
            for r in range(3):
                L[r][:3] = R[r]
        p = b["parent"]
        if p is not None and p >= 0:
            P = W[p]
            L = [[sum(P[i][k] * L[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
        W.append(L)
    return W


def pose_positions(skeleton, rotations):
    """{frame name: world position} for :func:`pose_matrices`."""
    W = pose_matrices(skeleton, rotations)
    return {b["name"]: [W[i][r][3] for r in range(3)] for i, b in enumerate(skeleton)}


def rider_rotations(pose, arm_fix=None, *, legacy=False, frf=None):
    return {n: channel_rotation(n, e, arm_fix, legacy=legacy, frf=frf) for n, e in zip(RIDER_CHANNELS, pose.rider or [])}


def bike_rotations(pose, *, legacy=False, frf=None):
    return {n: channel_rotation(n, e, legacy=legacy, frf=frf) for n, e in zip(BIKE_CHANNELS, pose.bike or [])}
