from conftest import corpus_files


def test_cli_imports():
    from dmbmx.cli import build_parser
    assert build_parser()


def test_corpus_counts(game_dir):
    assert len(corpus_files(".zmd")) == 117
    assert len(corpus_files(".zal")) == 6
