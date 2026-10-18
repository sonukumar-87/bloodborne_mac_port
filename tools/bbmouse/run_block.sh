# Keyboard and mouse add-on (tools/bbmouse): loaded into the game process and the GPU host.
# BB_MOUSE=0 turns it off. Looked for next to the app's libraries, else in out/bbmouse (checkout).
mouse_lib="$(cd "$(dirname -- "$probe")/../Frameworks" 2>/dev/null && pwd)/libbbmouse.dylib"
[[ -f $mouse_lib ]] || mouse_lib="$PWD/out/bbmouse/libbbmouse.dylib"
if [[ ${BB_MOUSE:-1} != 0 && -f $mouse_lib ]]; then
    export BB_MOUSE_SHM="${TMPDIR:-/tmp/}bbmouse-$$"
    rm -f "${TMPDIR:-/tmp/}"bbmouse-* 2>/dev/null || true
    export DYLD_INSERT_LIBRARIES="$mouse_lib"
fi
