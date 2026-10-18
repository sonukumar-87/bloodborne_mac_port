#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds and runs tools/bbmouse/test_bbmouse.c under Rosetta with the add-on loaded (after build.sh).
#   bash tools/bbmouse/test.sh [Bloodborne.app]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
app=${1:-dist/Bloodborne.app}
fw="$app/Contents/Frameworks"
inc=${BB_SDL_INCLUDE:-deps/arm64/include}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/home/Library/Application Support/Bloodborne"
clang -arch x86_64 -mmacosx-version-min=15.0 -I "$inc" -o "$tmp/test" tools/bbmouse/test_bbmouse.c \
    "$fw/libSDL3.0.dylib" -Wl,-rpath,"$fw"
# Run directly (not through /usr/bin/arch, which drops DYLD_ variables).
HOME="$tmp/home" BB_MOUSE_SHM="$tmp/shm" DYLD_INSERT_LIBRARIES="$PWD/out/bbmouse/libbbmouse.dylib" "$tmp/test"
