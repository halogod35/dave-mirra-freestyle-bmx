"""Re-implementation of the game's integrity checks (AD-7 route 1: re-sign, no exe changes).

Inventory so far (docs/re/integrity-checks.md; DM-030 completes it):

* SAVE.DAT (memcard load, VA 0x00412190): **no CRC or checksum.** The only
  checks are a successful 0x235C-byte read and ``u32 @4 == 0x235C``.
  ``check_save`` reproduces that decision exactly; ``resign_save`` fixes it.
* The exe's only CRC-32 code (table 0x005677B0, ``crc32`` 0x00518B20) belongs
  to the statically linked libpng/zlib ("CRC error" string 0x0069D3D0 is
  libpng's ``png_crc_finish``). The save path never calls it.
"""
import struct

from dmbmx.formats import save as _save


def check_save(data):
    """Return (accepted, reason) exactly as NgBMX.exe's memcard load (0x00412190) decides.

    Return codes in the exe: 0 ok, 5 open failed, 9 short read, 8 bad size field, -1 no memory.
    """
    if len(data) < _save.SIZE:
        return False, f"short read: {len(data)} < 0x{_save.SIZE:X} bytes (exe code 9)"
    size_field = struct.unpack_from("<I", data, 4)[0]
    if size_field != _save.SIZE:
        return False, f"size field 0x{size_field:X} != 0x{_save.SIZE:X} (exe code 8, 'probably old version')"
    return True, "ok"


def resign_save(data):
    """Return ``data`` changed so the game accepts it: magic + size field set, trailing bytes dropped.

    A no-op on a file the game wrote. Raises TruncatedError if the data is too short
    (missing content can't be invented).
    """
    s = _save.read(data, lenient=True)
    return _save.write(s.resign())
