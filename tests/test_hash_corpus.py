"""Evidence that dmbmx.core.hash.zhash == NgBMX.exe 0x004828C0: body-part name hashes occur in rider ZMDs."""
import struct

from conftest import corpus_files

from dmbmx.core.hash import zhash

PARTS = ["Hips", "Chest", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm",
         "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot"]


def test_bodypart_hashes_in_rider_models(game_dir):
    riders = [p for p in corpus_files(".zmd") if p.name.lower().startswith("davemirra") and p.stat().st_size]
    assert riders
    for p in riders:
        data = p.read_bytes()
        missing = [n for n in PARTS if struct.pack("<I", zhash(n)) not in data]
        assert not missing, f"{p.name}: no hash for {missing}"
