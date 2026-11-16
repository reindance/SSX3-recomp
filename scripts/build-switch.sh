#!/bin/sh
# Nintendo Switch build (docs/SWITCH.md) in devkitPro's devkitA64 container.
#
#   scripts/build-switch.sh            configure + build build-switch/ssx3_recomp.nro
#   scripts/build-switch.sh clean      start from an empty build-switch/
#
# Needs podman, the toolkit next to this repo (scripts/setup.sh) and the lifted
# code (scripts/regen.sh). The container sees the parent directory (this repo
# and ../xboxrecomp) and runs offline. The image is pinned; it is the one the
# SSX Tricky Switch port uses (devkitA64 GCC 15.2, SDL 2.28.5, libnx, deko3d).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
parent=$(dirname -- "$here")
name=$(basename -- "$here")
image=${SSX3_SWITCH_IMAGE:-docker.io/devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282}
jobs=${JOBS:-$(nproc)}

[ -d "$parent/xboxrecomp" ] || { echo "no ../xboxrecomp: run scripts/setup.sh" >&2; exit 1; }
[ -n "$(ls "$here"/src/recomp/gen/*.c 2>/dev/null)" ] || { echo "no lifted code: run scripts/regen.sh" >&2; exit 1; }
[ "${1:-}" = clean ] && rm -rf "$here/build-switch"
mkdir -p "$here/build-switch"

exec podman run --rm --network=none --userns=keep-id --security-opt label=disable \
    -v "$parent:/w:rw" -w "/w/$name" "$image" sh -eu -c "
        cmake -S . -B build-switch -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=\$DEVKITPRO/cmake/Switch.cmake \
            -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_DIR=/w/xboxrecomp
        cmake --build build-switch --parallel $jobs
    "
