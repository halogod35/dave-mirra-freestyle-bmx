#!/usr/bin/env bash
# Repo Python. Usage: tools/testing/py.sh script.py | tools/testing/py.sh -m dmbmx --help | tools/testing/py.sh -m pytest tests
# Runs the repo's .venv Python with tools/ (the dmbmx package and the other Python tools) on PYTHONPATH.
# Works from the main checkout and from git worktrees: the code is this checkout's, the venv is this
# checkout's .venv if present, else the main checkout's (shared).
R="$(cd "$(dirname "$0")/../.." && pwd)"
PY="$R/.venv/Scripts/python.exe"
[ -x "$PY" ] || PY="$R/.venv/bin/python"
if [ ! -x "$PY" ]; then
    COMMON_GIT="$(git -C "$R" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
    if [ -n "$COMMON_GIT" ]; then
        MAIN="$(cd "$COMMON_GIT/.." && pwd)"
        PY="$MAIN/.venv/Scripts/python.exe"
        [ -x "$PY" ] || PY="$MAIN/.venv/bin/python"
    fi
fi
if [ ! -x "$PY" ]; then
    echo "error: no venv found ($R/.venv or the main checkout's .venv). Create it (see README.md, Quick start):" >&2
    echo "       python -m venv .venv && .venv/Scripts/python.exe -m pip install -r requirements.txt" >&2
    exit 2
fi
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) SEP=";"; TOOLS="$(cygpath -w "$R/tools" 2>/dev/null || printf '%s' "$R/tools")" ;; *) SEP=":"; TOOLS="$R/tools" ;; esac
PYTHONPATH="$TOOLS${PYTHONPATH:+$SEP$PYTHONPATH}" exec "$PY" "$@"
