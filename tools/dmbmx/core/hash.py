"""Z-Axis NgEngine name hash (DM-010).

[V] Reimplements NgBMX.exe 0x004828C0 (`zHashName`, our name; it is called from zScene.c's NAME parser at
0x004902D5 and ~100 other sites). The string is upper-cased with the C-locale `toupper` (0x004A593C)
and hashed in two passes:

    h = sum_{i=1..n} i * C[i]                                  (pass 1, 1-based position weights)
    h += sum_{i=1..n} (C[i] + 0xFFFF * C[i-1]) << 16           (pass 2, C[0] = 0)

all mod 2**32, where C[i] = toupper(s[i]) as a *signed* char (bytes >= 0x80 are negative).
"""

MASK = 0xFFFFFFFF


def _upper(b):
    return b - 0x20 if 0x61 <= b <= 0x7A else b


def _schar(b):
    return b - 256 if b >= 0x80 else b


def zhash(name):
    """Hash a name (str or bytes) exactly like the game. Returns an unsigned 32-bit int."""
    if isinstance(name, str):
        name = name.encode("latin-1")
    cs = [_schar(_upper(b)) for b in name]
    h = 0
    for i, c in enumerate(cs, 1):
        h += c * i
    prev = 0
    for c in cs:
        h += ((c + prev * 0xFFFF) << 16)
        prev = c
    return h & MASK
