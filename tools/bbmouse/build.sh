#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds out/bbmouse/libbbmouse.dylib (x86_64 + arm64): keyboard and mouse controls for the game,
# loaded into bb-probe and bbgpu-host by run.sh (see tools/bbmouse/README.md). It links the
# universal libSDL3 of a built app, so the same SDL is used at run time.
#
#   bash tools/bbmouse/build.sh [Bloodborne.app]      # default: dist/Bloodborne.app
#   BB_SDL_INCLUDE=/path/to/SDL/include bash tools/bbmouse/build.sh ...
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
app=${1:-dist/Bloodborne.app}
out=out/bbmouse
sdl="$app/Contents/Frameworks/libSDL3.0.dylib"
inc=${BB_SDL_INCLUDE:-}
if [[ -z $inc ]]; then
    for candidate in deps/arm64/include deps/x86_64/include; do
        if [[ -f $candidate/SDL3/SDL.h ]]; then inc=$candidate; break; fi
    done
fi
[[ -f $sdl ]] || { echo "No $sdl: build the app first (scripts/macos/package_app.sh) or pass its path" >&2; exit 1; }
[[ -n $inc && -f $inc/SDL3/SDL.h ]] || { echo "No SDL3 headers: run scripts/macos/bootstrap_deps.sh or set BB_SDL_INCLUDE" >&2; exit 1; }
mkdir -p "$out"
for arch in x86_64 arm64; do
    clang -arch $arch -mmacosx-version-min=15.0 -dynamiclib -O2 -Wall -Wextra -Wno-unused-parameter \
        -I "$inc" -install_name @rpath/libbbmouse.dylib -Wl,-rpath,@loader_path \
        -o "$out/bbmouse-$arch.dylib" tools/bbmouse/bbmouse.c "$sdl"
done
lipo -create -output "$out/libbbmouse.dylib" "$out/bbmouse-x86_64.dylib" "$out/bbmouse-arm64.dylib"
rm -f "$out/bbmouse-x86_64.dylib" "$out/bbmouse-arm64.dylib"
codesign --force --sign - "$out/libbbmouse.dylib"
echo "Built $out/libbbmouse.dylib"
