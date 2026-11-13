#!/usr/bin/env bash
# Ghidra wrapper. The install lives in <main checkout>/.ghidra/ (gitignored); worktrees share the
# main checkout's install. DMBMX_GHIDRA_HOME points it at any other Ghidra 12.2 install.
set -u

GHIDRA_VERSION="12.2"
GHIDRA_ZIP="ghidra_12.2_PUBLIC_20261016.zip"
GHIDRA_URL="https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.2_build/$GHIDRA_ZIP"
GHIDRA_SHA256="2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67"  # from the release notes

usage() {
    cat <<EOF
Usage: tools/ghidra/ghidra.sh COMMAND [ARGS...]

Commands:
  headless ARGS...  run support/analyzeHeadless.bat with ARGS, e.g.
                      tools/ghidra/ghidra.sh headless <projDir> <projName> -import <exe> \\
                          -scriptPath <dir> -postScript <Script.java>
                    (no ARGS prints Ghidra's headless usage)
  gui               start the Ghidra GUI (ghidraRun.bat)
  home              print GHIDRA_HOME (Windows-style path)
  version           print the installed Ghidra version
  install           download Ghidra $GHIDRA_VERSION from the official GitHub release,
                    verify SHA-256 $GHIDRA_SHA256,
                    and unpack it into <main checkout>/.ghidra/ (no-op if already installed)
  -h, --help        this help

Environment:
  DMBMX_GHIDRA_HOME  use this Ghidra install instead of <main checkout>/.ghidra/ghidra_${GHIDRA_VERSION}_PUBLIC
  JAVA_HOME          optional; otherwise the first 'java' on PATH (needs JDK 21+) is used
EOF
}

HERE="$(cd "$(dirname "$0")" && pwd)"
# Main checkout root = parent of the shared .git dir (works from the main checkout and from worktrees).
COMMON_GIT="$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
if [ -n "$COMMON_GIT" ]; then
    MAIN_REPO="$(cd "$COMMON_GIT/.." && pwd)"
else
    MAIN_REPO="$(cd "$HERE/../.." && pwd)"
fi
GHIDRA_ROOT="$MAIN_REPO/.ghidra"
GHIDRA_HOME="${DMBMX_GHIDRA_HOME:-$GHIDRA_ROOT/ghidra_${GHIDRA_VERSION}_PUBLIC}"

need_install() {
    if [ ! -f "$GHIDRA_HOME/support/analyzeHeadless.bat" ]; then
        echo "error: Ghidra not found at $GHIDRA_HOME" >&2
        echo "       run: tools/ghidra/ghidra.sh install   (or set DMBMX_GHIDRA_HOME)" >&2
        exit 2
    fi
}

winpath() { cygpath -w "$1" 2>/dev/null || printf '%s\n' "$1"; }

# Run a .bat through cmd.exe. Paths with spaces are quoted for cmd; args are passed through.
run_bat() {
    local bat; bat="$(winpath "$1")"; shift
    local cmdline="\"$bat\""
    for a in "$@"; do cmdline+=" \"$a\""; done
    # The command line travels in an env var: MSYS would escape embedded quotes as \" (which
    # cmd.exe does not understand), but cmd expands %VAR% before parsing, so quotes survive.
    DMBMX_CMDLINE="$cmdline" MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
        cmd.exe /d /s /c "call %DMBMX_CMDLINE%"
}

do_install() {
    if [ -f "$GHIDRA_HOME/support/analyzeHeadless.bat" ]; then
        echo "Ghidra already installed: $(winpath "$GHIDRA_HOME")"
        return 0
    fi
    command -v unzip >/dev/null || { echo "error: 'unzip' not found" >&2; exit 2; }
    mkdir -p "$GHIDRA_ROOT/download"
    local zip="$GHIDRA_ROOT/download/$GHIDRA_ZIP"
    if [ ! -f "$zip" ] || [ "$(sha256sum "$zip" | cut -d' ' -f1)" != "$GHIDRA_SHA256" ]; then
        echo "downloading $GHIDRA_URL"
        curl -fL --retry 3 -o "$zip.part" "$GHIDRA_URL" || { echo "error: download failed" >&2; exit 1; }
        mv -f "$zip.part" "$zip"
    fi
    local got; got="$(sha256sum "$zip" | cut -d' ' -f1)"
    if [ "$got" != "$GHIDRA_SHA256" ]; then
        echo "error: SHA-256 mismatch for $zip" >&2
        echo "       expected $GHIDRA_SHA256" >&2
        echo "       got      $got" >&2
        rm -f "$zip"
        exit 1
    fi
    echo "SHA-256 OK: $got"
    local tmp="$GHIDRA_ROOT/.extract.$$"
    rm -rf "$tmp"; mkdir -p "$tmp"
    unzip -q "$zip" -d "$tmp" || { rm -rf "$tmp"; echo "error: unzip failed" >&2; exit 1; }
    mv "$tmp/ghidra_${GHIDRA_VERSION}_PUBLIC" "$GHIDRA_ROOT/" && rm -rf "$tmp"
    echo "installed: $(winpath "$GHIDRA_ROOT/ghidra_${GHIDRA_VERSION}_PUBLIC")"
}

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
    ""|-h|--help|help) usage ;;
    install) do_install ;;
    home)    need_install; winpath "$GHIDRA_HOME" ;;
    version) need_install; sed -n 's/^application\.version=//p' "$GHIDRA_HOME/Ghidra/application.properties" ;;
    headless) need_install; run_bat "$GHIDRA_HOME/support/analyzeHeadless.bat" "$@" ;;
    gui)     need_install; run_bat "$GHIDRA_HOME/ghidraRun.bat" "$@" ;;
    *) echo "error: unknown command '$cmd' (see tools/ghidra/ghidra.sh --help)" >&2; exit 2 ;;
esac
