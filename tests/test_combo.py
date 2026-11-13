"""DM-018: rider + bike combo glTF export (structure, counts, placement, wheels, CLI, Blender)."""
import math
import os
import subprocess
from pathlib import Path

import pytest
from conftest import GAME_DIR

from dmbmx.cli import main
from dmbmx.convert import combo, gltf
from dmbmx.convert.gltf import EXTRAS_KEY, read_glb
from dmbmx.formats import motion as M
from dmbmx.formats import zmd

BLENDER = Path(r"C:\Program Files\Blender Foundation\Blender 5.3\blender.exe")


@pytest.fixture(scope="module")
def models():
    a = GAME_DIR / "assets"
    if not (a / "motion.zal").is_file():
        pytest.skip("no game assets")
    rd, bd = (a / "DaveMirra1.ZMD").read_bytes(), (a / "DMbike01.zmd").read_bytes()
    return zmd.read(rd), zmd.read(bd), rd, bd, M.read_bank((a / "motion.zal").read_bytes())


def _combo(models, ids):
    r, b, rd, bd, bank = models
    clips = [(f"motion_{i:03d}", bank.motion(i)) for i in ids]
    return combo.export_combo(r, b, clips, rider_source=rd, bike_source=bd, rider_name="DaveMirra1",
                              bike_name="DMbike01"), clips


def test_structure_and_counts(models):
    r, b, rd, bd, bank = models
    glb, clips = _combo(models, [8, 15])
    js, bin_ = read_glb(glb)
    rjs, _ = read_glb(gltf.export_glb(r, source=rd))
    bjs, _ = read_glb(gltf.export_glb(b, source=bd))
    # everything from both models, plus one combo root
    assert len(js["nodes"]) == len(rjs["nodes"]) + len(bjs["nodes"]) + 1
    for k in ("meshes", "materials", "images", "textures", "skins"):
        assert len(js.get(k, [])) == len(rjs.get(k, [])) + len(bjs.get(k, [])), k
    root = js["nodes"][js["scenes"][0]["nodes"][0]]
    assert root["name"] == "DaveMirra1+DMbike01"
    assert root["extras"][EXTRAS_KEY]["clips"] == ["motion_008", "motion_015"]
    kids = [js["nodes"][i]["name"] for i in root["children"]]
    assert kids == ["HIPSDECK", "DAVEMIRRA1"]
    assert "rotation" not in js["nodes"][root["children"][1]]        # rider upright comes from the Hips track
    # one animation per clip; channels: 15 rider + bike channels + rider root translation + 2 wheels
    assert [a["name"] for a in js["animations"]] == ["motion_008", "motion_015"]
    for a, (_, mo) in zip(js["animations"], clips):
        nb = mo.sections[1][0]
        assert len(a["channels"]) == 15 + nb + 1 + 2
        paths = [c["target"]["path"] for c in a["channels"]]
        assert paths.count("translation") == 1
        for s in a["samplers"]:
            assert js["accessors"][s["input"]]["count"] == mo.nframes
            assert js["accessors"][s["output"]]["count"] == mo.nframes
    # all accessor/bufferView ranges inside the binary chunk
    for bv in js["bufferViews"]:
        assert bv["byteOffset"] + bv["byteLength"] <= len(bin_)
    # skin joints point at rider frame nodes
    names = {js["nodes"][j]["name"] for j in js["skins"][0]["joints"]}
    assert {"Hips", "LeftHand", "RightFoot"} <= names


def test_static_combo(models):
    glb, _ = _combo(models, [])
    js, _ = read_glb(glb)
    assert "animations" not in js and len(js["scenes"][0]["nodes"]) == 1


def test_feet_on_pedals_and_wheels(models):
    r, b, rd, bd, bank = models
    rs, bs = r.skeleton(), b.skeleton()
    mo = bank.motion(8)
    info = {}
    offs = combo.rider_offsets(rs, bs, mo, info)
    assert all(info["good"]) and max(info["residual"]) < combo.FOOT_TOLERANCE
    assert all(0.4 < o[1] < 0.8 for o in offs)
    w = combo._wheel_tracks(bs, mo, combo.GEAR_RATIO)["BackWheel"]
    total = sum(math.degrees(2 * math.acos(min(1.0, abs(sum(x * y for x, y in zip(q0, q1))))))
                for q0, q1 in zip(w, w[1:]))
    assert total == pytest.approx(360 * combo.GEAR_RATIO, rel=0.05)    # one crank turn in motion 8


def test_offsets_interpolate_off_pedal_frames(models):
    r, b, rd, bd, bank = models
    info = {}
    offs = combo.rider_offsets(r.skeleton(), b.skeleton(), bank.motion(59), info, fill="interpolate")
    assert not all(info["good"]) and any(info["good"])
    good = [o for o, g in zip(offs, info["good"]) if g]
    lo = [min(o[i] for o in good) for i in range(3)]
    hi = [max(o[i] for o in good) for i in range(3)]
    for o, g in zip(offs, info["good"]):
        if not g:   # interpolated values stay inside the range of the good frames
            assert all(lo[i] - 1e-9 <= o[i] <= hi[i] + 1e-9 for i in range(3))


def test_cli(models, tmp_path, capsys):
    a = GAME_DIR / "assets"
    out = tmp_path / "c.glb"
    assert main(["export-combo", str(a / "DaveMirra1.ZMD"), str(a / "DMbike01.zmd"), str(out), "--anims", "8"]) == 0
    js, _ = read_glb(out.read_bytes())
    assert len(js["animations"]) == 1
    assert main(["export-combo", str(a / "DaveMirra1.ZMD"), str(a / "DMbike01.zmd"), str(out),
                 "--anims", "none", "--no-wheel-spin"]) == 0
    with pytest.raises(SystemExit):
        main(["export-combo", str(a / "DaveMirra1.ZMD"), str(a / "DMbike01.zmd"), str(out), "--anims", "999"])


@pytest.mark.skipif(not BLENDER.exists() or os.environ.get("DMBMX_NO_BLENDER"), reason="Blender 5.3 not installed")
def test_blender_combo_check(tmp_path):
    a = GAME_DIR / "assets"
    out = tmp_path / "m8.glb"
    assert main(["export-combo", str(a / "DaveMirra1.ZMD"), str(a / "DMbike01.zmd"), str(out), "--anims", "8"]) == 0
    script = Path(__file__).resolve().parents[1] / "tools" / "blender" / "scripts" / "check_combo.py"
    r = subprocess.run([str(BLENDER), "-b", "--factory-startup", "--python", str(script), "--", str(out), "--riding"],
                       capture_output=True, text=True, timeout=600)
    assert "COMBO CHECK OK" in r.stdout, r.stdout[-2000:]


def test_offsets_engine_fill_off_pedal_frames(models):
    """DM-047: off-pedal frames follow the game's placement F(-bike translation) / 100 + the clip's constant."""
    r, b, rd, bd, bank = models
    mo = bank.motion(59)
    info = {}
    offs = combo.rider_offsets(r.skeleton(), b.skeleton(), mo, info)
    c = info["engine_offset"]
    eng = combo.engine_offsets(mo, c)
    assert not all(info["good"]) and any(info["good"])
    for o, e, g in zip(offs, eng, info["good"]):
        if not g:
            assert o == pytest.approx(e)
    # where the feet are on the pedals (motion 15: all frames) the relation holds to a few cm
    info15 = {}
    offs15 = combo.rider_offsets(r.skeleton(), b.skeleton(), bank.motion(15), info15)
    d = sorted(math.dist(o, e) for o, e in zip(offs15, combo.engine_offsets(bank.motion(15), info15["engine_offset"])))
    assert all(info15["good"]) and d[len(d) // 2] < 0.05
    with pytest.raises(ValueError):
        combo.rider_offsets(r.skeleton(), b.skeleton(), mo, fill="nope")
    # a clip with no on-pedal frame uses the global constant
    assert combo.engine_offsets(bank.motion(1))[0] == pytest.approx(
        tuple(e + o for e, o in zip(combo.engine_offsets(bank.motion(1), (0, 0, 0))[0], combo.ENGINE_OFFSET)))
