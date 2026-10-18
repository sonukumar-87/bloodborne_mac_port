#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Adds the keyboard and mouse add-on to an installed Bloodborne.app (for example a release
# download): copies out/bbmouse/libbbmouse.dylib into its Frameworks, adds the loader block to its
# run.sh if missing (the originals are kept next to them as *.orig), and re-signs the app ad hoc.
#
#   bash tools/bbmouse/build.sh /Applications/Bloodborne.app
#   bash tools/bbmouse/install.sh /Applications/Bloodborne.app
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
app=${1:?usage: install.sh /path/to/Bloodborne.app}
lib=out/bbmouse/libbbmouse.dylib
run="$app/Contents/Resources/port/run.sh"
[[ -f $lib ]] || { echo "Build it first: bash tools/bbmouse/build.sh $app" >&2; exit 1; }
[[ -f $run ]] || { echo "$app is not a Bloodborne.app (no Resources/port/run.sh)" >&2; exit 1; }
cp "$lib" "$app/Contents/Frameworks/.libbbmouse.new"
mv -f "$app/Contents/Frameworks/.libbbmouse.new" "$app/Contents/Frameworks/libbbmouse.dylib"
if ! grep -q 'libbbmouse.dylib' "$run"; then
    [[ -f $run.orig ]] || cp -p "$run" "$run.orig"
    python3 - "$run" tools/bbmouse/run_block.sh <<'EOF'
import sys
path, block = sys.argv[1], open(sys.argv[2]).read()
text = open(path).read()
anchor = 'probe_args=("$out/boot-linked.bin"'
if text.count(anchor) != 1:
    sys.exit("run.sh: launch line not found; add tools/bbmouse/run_block.sh by hand")
open(path, "w").write(text.replace(anchor, block + anchor))
EOF
fi
codesign --force --sign - --preserve-metadata=entitlements,flags "$app"
codesign --verify --deep --strict "$app"
echo "Keyboard and mouse add-on installed in $app"
