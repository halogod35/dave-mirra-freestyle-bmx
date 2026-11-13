"""ZAL <-> directory (DM-023): `extract_dir` unpacks an archive, `pack_dir` rebuilds it.

Directory layout written by ``extract_dir``::

    <dir>/_index.json         header fields, padding/tail bytes and one record per asset
    <dir>/<id:08x>.<ext>      decompressed payload (.bmp .iff .strs .zal .bin)
    <dir>/<id:08x>/           only with recursive=True and a nested ZAL payload: its own extract dir
    <dir>/_stored/<id>.lzo    only for compressed archives: the original compressed stream

``pack_dir`` reads the payloads back. An asset whose payload still has the recorded SHA-256 is written
with its original stored bytes (so an unedited directory packs byte-identically); an edited payload is
stored raw or re-compressed with LZO1X. A nested directory, if present, takes priority over the
``.zal`` file next to it and is packed first.
"""
import hashlib
import json
from pathlib import Path

from ..core import lzo
from ..core.errors import FormatError
from . import zal

INDEX = "_index.json"
STORED = "_stored"
FORMAT_TAG = "dmbmx-zal-dir/1"
MAX_DEPTH = 8

_EXT = {b"FORM": ".iff", b"STRS": ".strs", zal.MAGIC_BYTES: ".zal"}


def ext_for(payload):
    head = bytes(payload[:4])
    if head in _EXT:
        return _EXT[head]
    if head[:2] == b"BM":
        return ".bmp"
    return ".bin"


def kind_of(payload):
    head = bytes(payload[:4])
    if head == zal.MAGIC_BYTES:
        return "ZAL"
    if head[:2] == b"BM":
        return "BMP"
    if len(head) == 4 and all(0x20 <= b < 0x7F for b in head):
        return head.decode("ascii")
    return head.hex()


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def extract_dir(arc, outdir, recursive=False, raw=False, _depth=0):
    """Write ``arc`` to ``outdir``. Returns the number of assets written (nested ones included)."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    records = []
    count = 0
    for e in arc.entries:
        for i, a in enumerate(e.assets):
            payload = a.stored if raw else arc.asset_data(a)
            name = f"{e.id:08x}" + (f"_{i}" if len(e.assets) > 1 else "")
            fname = name + (".lzo" if raw and arc.compressed else ext_for(payload))
            (outdir / fname).write_bytes(payload)
            count += 1
            rec = {"id": e.id, "asset": i, "file": fname, "type": kind_of(payload),
                   "size": a.size, "size_field": a.size_field, "stored": len(a.stored),
                   "sha256": _sha(payload), "pad": a.pad.hex() if a.pad is not None else None,
                   "pad_from": a.pad_from}
            if arc.compressed and not raw:
                sp = outdir / STORED / f"{name}.lzo"
                sp.parent.mkdir(exist_ok=True)
                sp.write_bytes(a.stored)
                rec["stored_file"] = f"{STORED}/{name}.lzo"
            if recursive and not raw and zal.is_zal(payload) and _depth < MAX_DEPTH:
                count += extract_dir(zal.read(payload), outdir / name, True, False, _depth + 1)
                rec["nested"] = name
            records.append(rec)
    meta = {"format": FORMAT_TAG, "raw": raw, "reserved": arc.reserved.hex(), "flags": arc.flags,
            "count_word": arc.count_word, "per_group": arc.assets_per_group, "groups": len(arc.entries),
            "compressed": arc.compressed, "method": arc.method, "tail": arc.tail.hex(),
            "entries": records}
    (outdir / INDEX).write_text(json.dumps(meta, indent=1), newline="\n")
    return count


class PackError(FormatError):
    def __init__(self, msg):
        super().__init__(msg, fmt="ZAL-DIR")


def _hex(rec, key, default=b""):
    v = rec.get(key)
    return default if v is None else bytes.fromhex(v)


def pack_dir(indir, _depth=0, report=None):
    """Rebuild the archive bytes from an ``extract_dir`` directory.

    ``report`` (optional list) receives ``(path, id, action)`` for every asset, where action is
    "kept" (original stored bytes) or "re-encoded"."""
    indir = Path(indir)
    try:
        meta = json.loads((indir / INDEX).read_text())
    except (OSError, ValueError) as e:
        raise PackError(f"{indir / INDEX}: {e}") from e
    if meta.get("format") != FORMAT_TAG:
        raise PackError(f"{indir}: not a {FORMAT_TAG} directory (re-extract with this version)")
    if meta.get("raw"):
        raise PackError(f"{indir}: made with --raw; pack needs a decompressed extract")
    arc = zal.ZalArchive()
    arc.reserved = bytes.fromhex(meta["reserved"])
    arc.flags = int(meta["flags"])
    arc.count_word = int(meta["count_word"])
    arc.tail = bytes.fromhex(meta.get("tail", ""))
    if len(arc.reserved) != 12:
        raise PackError("reserved must be 12 bytes")
    groups = {}
    for rec in meta["entries"]:
        gid = int(rec["id"])
        if gid not in groups:
            groups[gid] = zal.ZalEntry(gid, [], arc)
            arc.entries.append(groups[gid])
        nested = rec.get("nested")
        if nested and (indir / nested).is_dir() and _depth < MAX_DEPTH:
            payload = pack_dir(indir / nested, _depth + 1, report)
        else:
            try:
                payload = (indir / rec["file"]).read_bytes()
            except OSError as e:
                raise PackError(f"missing payload file: {e}") from e
        size_field = int(rec["size_field"])
        unchanged = _sha(payload) == rec.get("sha256")
        if unchanged and not arc.compressed:
            stored = payload
        elif unchanged and rec.get("stored_file") and (indir / rec["stored_file"]).is_file():
            stored = (indir / rec["stored_file"]).read_bytes()
        else:
            unchanged = False
            if arc.method == zal.METHOD_NONE:
                stored = payload
            elif arc.method == zal.METHOD_LZO1X:
                stored = lzo.compress(payload)
            else:
                raise PackError(f"cannot encode compression method {arc.method}")
            if len(payload) > zal.SIZE_MASK:
                raise PackError(f"group 0x{gid:x}: payload too large for the 24-bit size field")
            size_field = (size_field & ~zal.SIZE_MASK & 0xFFFFFFFF) | len(payload)
        pad = _hex(rec, "pad", None) if rec.get("pad") is not None else None
        groups[gid].assets.append(zal.ZalAsset(stored, size_field, None, pad, rec.get("pad_from")))
        if report is not None:
            report.append((str(indir), gid, "kept" if unchanged else "re-encoded"))
    return arc.write()


def replace(arc, id_path, payload):
    """Replace the payload addressed by ``id_path`` (list of group ids; all but the last select
    nested archives). Only the touched asset (and its parent archives) are re-encoded."""
    if not id_path:
        raise ValueError("empty id path")
    if len(id_path) == 1:
        arc.set_data(id_path[0], payload)
        return arc
    entry = arc.find(id_path[0])
    inner = entry.data()
    if not zal.is_zal(inner):
        raise PackError(f"group 0x{id_path[0]:x} is not a nested archive")
    sub = replace(zal.read(inner), id_path[1:], payload)
    arc.set_data(id_path[0], sub.write())
    return arc
