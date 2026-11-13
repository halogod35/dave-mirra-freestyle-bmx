"""Apply documented byte patches to a copy of NgBMX.exe (never the input file itself).

Refuses unless: the input's SHA-256 is the pristine exe's, every patch's original bytes match at
its file offset (and the VA maps to that offset), and the output path is not the input,
not the install's NgBMX.exe and not inside _pristine (AD-1).
"""
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMAGE_BASE = 0x400000
# (VA start, VA end, raw offset) of NgBMX.exe sections that patches may touch
SECTIONS = ((0x401000, 0x54B000, 0x1000),)


class PatchError(ValueError):
    pass


def load(path=HERE / "patches.json"):
    spec = json.loads(Path(path).read_text(encoding="utf-8"))
    for p in spec["patches"]:
        p["va_i"] = int(p["va"], 16)
        p["off_i"] = int(p["file_offset"], 16)
        p["orig_b"] = bytes.fromhex(p["orig"])
        p["new_b"] = bytes.fromhex(p["new"])
        if len(p["orig_b"]) != len(p["new_b"]):
            raise PatchError(f"{p['id']}: orig/new length differ")
        if va_to_offset(p["va_i"]) != p["off_i"]:
            raise PatchError(f"{p['id']}: va {p['va']} does not map to file offset {p['file_offset']}")
    return spec


def va_to_offset(va):
    for lo, hi, raw in SECTIONS:
        if lo <= va < hi:
            return va - lo + raw
    raise PatchError(f"VA 0x{va:08x} outside the patchable sections")


def select(spec, ids=None, group=None):
    by_id = {p["id"]: p for p in spec["patches"]}
    want = list(ids or []) + (spec["groups"].get(group, []) if group else [])
    if group and group not in spec["groups"]:
        raise PatchError(f"unknown group {group!r} (have {sorted(spec['groups'])})")
    unknown = [i for i in want if i not in by_id]
    if unknown:
        raise PatchError(f"unknown patch id(s) {unknown} (have {sorted(by_id)})")
    seen, out = set(), []
    for i in want:
        if i not in seen:
            seen.add(i)
            out.append(by_id[i])
    if not out:
        raise PatchError("no patches selected")
    return out


def apply_bytes(data, patches, spec):
    sha = hashlib.sha256(data).hexdigest()
    if sha != spec["sha256"]:
        raise PatchError(f"input SHA-256 {sha} is not the pristine {spec['exe']} ({spec['sha256']})")
    out = bytearray(data)
    for p in patches:
        o, orig = p["off_i"], p["orig_b"]
        if bytes(out[o:o + len(orig)]) != orig:
            raise PatchError(f"{p['id']}: bytes at 0x{o:x} are {bytes(out[o:o + len(orig)]).hex()}, expected {p['orig']}")
        out[o:o + len(orig)] = p["new_b"]
    return bytes(out)


def _forbidden_output(inp, out):
    inp, out = Path(inp).resolve(), Path(out).resolve()
    if out == inp:
        return "output is the input file"
    parts = [x.lower() for x in out.parts]
    if "_pristine" in parts:
        return "output is inside _pristine"
    if out.name.lower() == "ngbmx.exe" and (out.parent / "PRISTINE.sha256").exists():
        return "output is the install's NgBMX.exe"
    return None


def apply_file(inp, out, ids=None, group=None, spec=None):
    spec = spec or load()
    why = _forbidden_output(inp, out)
    if why:
        raise PatchError(f"refusing: {why} (write a new file, e.g. NgBMX_nocrc.exe)")
    patches = select(spec, ids, group)
    data = Path(inp).read_bytes()
    new = apply_bytes(data, patches, spec)
    Path(out).write_bytes(new)
    return patches, hashlib.sha256(new).hexdigest()
