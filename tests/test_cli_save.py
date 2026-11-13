import subprocess
import sys

from conftest import corpus_files
from dmbmx.cli import main
from dmbmx.formats import save


def test_save_help():
    r = subprocess.run([sys.executable, "-m", "dmbmx", "save", "fix-crc", "--help"], capture_output=True, text=True)
    assert r.returncode == 0 and "fix-crc" in r.stdout


def test_save_cli_real(game_dir, tmp_path, capsys):
    src = corpus_files(".dat")[0]
    assert main(["save", "info", str(src)]) == 0
    assert "game accepts: yes" in capsys.readouterr().out
    out = tmp_path / "SAVE.DAT"
    assert main(["save", "fix-crc", str(src), str(out)]) == 0
    assert out.read_bytes() == src.read_bytes()
    # edited + broken header -> fixed and accepted
    s = save.read(src.read_bytes())
    s.scores[0][0].score = 777
    s.size_field = 0
    bad = tmp_path / "bad.dat"
    bad.write_bytes(save.write(s))
    assert main(["save", "info", str(bad)]) == 1
    assert main(["save", "fix-crc", str(bad), str(out)]) == 0
    assert save.read(out.read_bytes()).scores[0][0].score == 777
