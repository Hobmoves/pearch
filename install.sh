#!/bin/sh
# pearch installer for Linux and macOS
#
#   curl -fsSL https://raw.githubusercontent.com/Hobmoves/pearch/main/install.sh | sh
#
# Installs the pearch binary into ~/.local/bin (or $PEARCH_BIN_DIR), then
# shows a quick tour. Falls back to building from source when there is no
# prebuilt binary for this machine.
#
# Optional environment variables:
#   PEARCH_VERSION=v1.0.0     install a specific release
#   PEARCH_BIN_DIR=/usr/local/bin
#   PEARCH_NO_LAUNCH=1        don't offer to open pearch at the end
#   PEARCH_REPO=owner/repo    install from a fork
set -eu

REPO="${PEARCH_REPO:-Hobmoves/pearch}"
BIN_DIR="${PEARCH_BIN_DIR:-$HOME/.local/bin}"

if [ -t 1 ]; then
    green=$(printf '\033[32m'); gold=$(printf '\033[33m'); dim=$(printf '\033[2m'); red=$(printf '\033[31m'); off=$(printf '\033[0m')
else
    green=''; gold=''; dim=''; red=''; off=''
fi
step() { printf '  %s>%s %s\n' "$green" "$off" "$1"; }
note() { printf '    %s%s%s\n' "$dim" "$1" "$off"; }
die() { printf '\n  %s%s%s\n\n' "$red" "$1" "$off" >&2; exit 1; }

printf '\n  %sp e a r c h%s   installing instant file search\n\n' "$gold" "$off"

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die "This installer is for Linux and macOS. On Windows run: irm https://raw.githubusercontent.com/$REPO/main/install.ps1 | iex" ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x64 ;;
    arm64 | aarch64) arch=arm64 ;;
    *) arch=$(uname -m) ;;
esac
asset="pearch-$os-$arch"

work=$(mktemp -d 2>/dev/null || mktemp -d -t pearch)
trap 'rm -rf "$work"' EXIT
out="$work/pearch"

fetch() {
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
    else return 1; fi
}

build_from_source() {
    src="$1"
    command -v cmake >/dev/null 2>&1 || return 1
    command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || return 1
    step "Building from source (this takes a minute)"
    cmake -S "$src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DPEARCH_BUILD_TESTS=OFF -DPEARCH_BUILD_BENCH=OFF >/dev/null || return 1
    jobs=$( (getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu) 2>/dev/null || echo 4)
    cmake --build "$work/build" --parallel "$jobs" >/dev/null || return 1
    cp "$work/build/pearch" "$out"
}

got=0
if [ -n "${PEARCH_VERSION:-}" ]; then
    url="https://github.com/$REPO/releases/download/$PEARCH_VERSION/$asset"
else
    url="https://github.com/$REPO/releases/latest/download/$asset"
fi
step "Downloading $asset"
if fetch "$url" "$out" 2>/dev/null && [ -s "$out" ]; then
    got=1
else
    note "No prebuilt binary at $url"
fi

if [ "$got" = 0 ] && command -v gh >/dev/null 2>&1; then
    step "Trying the GitHub CLI"
    if gh release download ${PEARCH_VERSION:+"$PEARCH_VERSION"} --repo "$REPO" --pattern "$asset" --dir "$work" >/dev/null 2>&1; then
        mv "$work/$asset" "$out" && got=1
    fi
fi

if [ "$got" = 0 ] && command -v git >/dev/null 2>&1; then
    if git clone --depth 1 "https://github.com/$REPO.git" "$work/src" >/dev/null 2>&1; then
        build_from_source "$work/src" && got=1
    fi
fi

[ "$got" = 1 ] || die "Couldn't get pearch: no release for $asset, and building needs git, CMake and a C++20 compiler."

mkdir -p "$BIN_DIR"
chmod +x "$out"
mv -f "$out" "$BIN_DIR/pearch"
step "Installed to $BIN_DIR/pearch"

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        note "$BIN_DIR is not on your PATH yet. Add this to your shell profile:"
        note "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac

"$BIN_DIR/pearch" tour

if [ -z "${PEARCH_NO_LAUNCH:-}" ] && [ -r /dev/tty ] && [ -t 1 ]; then
    printf '  Press %sEnter%s to open pearch now (it indexes your files the first time), or Ctrl+C to finish. ' "$green" "$off"
    read -r _ </dev/tty || exit 0
    exec "$BIN_DIR/pearch" </dev/tty
fi
