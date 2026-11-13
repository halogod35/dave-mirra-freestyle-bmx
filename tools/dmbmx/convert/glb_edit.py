"""Tiny scripted editor for dmbmx .glb files (tests, play-test generators, batch edits).

    doc = GlbDoc(glb_bytes)
    ni = doc.atomic_node(frame_name="FrontWheel")
    doc.map_positions(ni, lambda p: (p[0] * 1.5, p[1] * 1.5, p[2] * 1.5))
    doc.set_image(doc.image_index("NAME_431_0"), new_png)
    new_glb = doc.to_bytes()

Edits rewrite whole accessors (appending a new bufferView), so counts may change. The result is
an ordinary .glb that :func:`dmbmx.convert.gltf_import.import_glb` reads.
"""
from __future__ import annotations

import json
import struct

from dmbmx.convert.gltf import EXTRAS_KEY, read_glb

_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
_FMT = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}


class GlbDoc:
    def __init__(self, data: bytes | None = None, *, js: dict | None = None):
        """From .glb bytes, or (``js=``) from a glTF JSON dict with an empty binary buffer: then every
        accessor/image the importer reads must be (re)written with :meth:`add`/:meth:`set_image`."""
        if js is not None:
            self.js, self.bin = js, bytearray()
            self.js.setdefault("bufferViews", [])
            self.js.setdefault("accessors", [])
        else:
            self.js, b = read_glb(data)
            self.bin = bytearray(b)

    # -- lookup ---------------------------------------------------------------
    def ex(self, i):
        return (self.js["nodes"][i].get("extras") or {}).get(EXTRAS_KEY, {})

    def atomic_node(self, *, frame_name=None, atomic=None, lod=0):
        """Node index of an atomic's mesh node, by its frame's name or atomic index."""
        nodes = self.js["nodes"]
        names = {self.ex(i)["frame"]: n.get("name") for i, n in enumerate(nodes) if "record" in self.ex(i)}
        for i, n in enumerate(nodes):
            e = self.ex(i)
            if "atomic" in e and (atomic is None or e["atomic"] == atomic) and \
                    (frame_name is None or names.get(e["frame"]) == frame_name):
                return i
        raise KeyError(f"no atomic node for frame {frame_name!r} / atomic {atomic!r}")

    def frame_node(self, name):
        return next(i for i, n in enumerate(self.js["nodes"]) if "record" in self.ex(i) and n.get("name") == name)

    def image_index(self, name):
        return next(i for i, im in enumerate(self.js["images"]) if im.get("name", "").upper() == name.upper())

    def mesh_prims(self, ni):
        return self.js["meshes"][self.js["nodes"][ni]["mesh"]]["primitives"]

    # -- accessors --------------------------------------------------------------
    def get(self, ai):
        acc = self.js["accessors"][ai]
        bv = self.js["bufferViews"][acc["bufferView"]]
        n = _NCOMP[acc["type"]]
        flat = struct.unpack_from(f"<{n * acc['count']}{_FMT[acc['componentType']]}", self.bin,
                                  bv.get("byteOffset", 0) + acc.get("byteOffset", 0))
        return [flat[k:k + n] if n > 1 else flat[k] for k in range(0, len(flat), n)]

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.js["bufferViews"].append(bv)
        return len(self.js["bufferViews"]) - 1

    def set(self, ai, values):
        """Replace accessor ``ai``'s data (any count) with ``values`` (list of tuples/scalars)."""
        acc = self.js["accessors"][ai]
        n = _NCOMP[acc["type"]]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        fmt = _FMT[acc["componentType"]]
        target = self.js["bufferViews"][acc["bufferView"]].get("target")
        acc["bufferView"] = self._view(struct.pack(f"<{len(flat)}{fmt}", *flat), target)
        acc.pop("byteOffset", None)
        acc["count"] = len(values)
        if "min" in acc:
            vals = struct.unpack(f"<{len(flat)}{fmt}", struct.pack(f"<{len(flat)}{fmt}", *flat))
            acc["min"] = [min(vals[k::n]) for k in range(n)]
            acc["max"] = [max(vals[k::n]) for k in range(n)]

    def add(self, values, component_type, typ, *, target=None, normalized=False, minmax=False):
        """Append a new accessor; returns its index. ``values``: list of tuples (or scalars)."""
        n = _NCOMP[typ]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        fmt = _FMT[component_type]
        data = struct.pack(f"<{len(flat)}{fmt}", *flat)
        acc = {"bufferView": self._view(data, target), "componentType": component_type, "count": len(values),
               "type": typ}
        if normalized:
            acc["normalized"] = True
        if minmax and values:
            vals = struct.unpack(f"<{len(flat)}{fmt}", data)
            acc["min"] = [min(vals[k::n]) for k in range(n)]
            acc["max"] = [max(vals[k::n]) for k in range(n)]
        self.js["accessors"].append(acc)
        return len(self.js["accessors"]) - 1

    def map_positions(self, ni, fn):
        ai = self.mesh_prims(ni)[0]["attributes"]["POSITION"]
        self.set(ai, [tuple(fn(p)) for p in self.get(ai)])

    def set_image(self, ii, png: bytes):
        self.js["images"][ii]["bufferView"] = self._view(png)

    # -- output -------------------------------------------------------------------
    def to_bytes(self) -> bytes:
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.js, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin))
        return out + struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin)
