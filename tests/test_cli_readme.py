"""tools/dmbmx/README.md documents every CLI command, subcommand and option (walks the real argparse tree)."""
import argparse
import re
from pathlib import Path

from dmbmx.cli import build_parser

README = Path(__file__).resolve().parent.parent / "tools" / "dmbmx" / "README.md"


def _subparsers(parser):
    for a in parser._actions:
        if isinstance(a, argparse._SubParsersAction):
            yield from a.choices.items()


def _walk(parser, path=()):
    """(path tuple, parser) for every command and subcommand, depth first."""
    for name, sp in _subparsers(parser):
        yield path + (name,), sp
        yield from _walk(sp, path + (name,))


def _commands():
    return list(_walk(build_parser()))


def test_tree_is_not_empty():
    paths = [p for p, _ in _commands()]
    assert len([p for p in paths if len(p) == 1]) >= 15
    assert ("save", "unlock-all") in paths and ("gui", "hud-import") in paths


def test_every_command_and_subcommand_is_documented():
    text = README.read_text(encoding="utf-8")
    missing = [" ".join(p) for p, _ in _commands() if f"`{' '.join(p)}`" not in text]
    assert not missing, f"commands missing from {README.name} (as `command sub`): {missing}"


def test_every_top_level_command_has_a_row_in_the_command_table():
    text = README.read_text(encoding="utf-8")
    table = text[text.index("## Command table"):text.index("## ", text.index("## Command table") + 3)]
    rows = set(re.findall(r"^\| \[`([\w-]+)`\]", table, flags=re.M))
    tops = {p[0] for p, _ in _commands()}
    assert tops <= rows, sorted(tops - rows)


def test_every_option_is_documented():
    text = README.read_text(encoding="utf-8")
    missing = []
    for path, sp in _commands():
        for a in sp._actions:
            if a.help == argparse.SUPPRESS or isinstance(a, (argparse._HelpAction, argparse._SubParsersAction)):
                continue
            for opt in a.option_strings:
                if opt.startswith("--") and opt not in text:
                    missing.append(f"{' '.join(path)} {opt}")
    assert not missing, missing
