#!/bin/bash
# Re-vendors libevmu + libgimbal source into components/evmu/libevmu from a sibling
# libevmu checkout, then re-applies the ESP32-only patches (tools/patches/*.patch)
# to libgimbal on top. components/evmu/libevmu is a flattened snapshot, not a
# submodule, so this copy-and-repatch is how library fixes make it into the firmware.
#
# Usage: tools/sync_libevmu.sh [path-to-libevmu-checkout]   (default: ../libevmu)
set -euo pipefail

cd "$(dirname "$0")/.."

SRC="${1:-../libevmu}"
DST="components/evmu/libevmu"
PATCH_DIR="tools/patches"

[ -d "$SRC/lib" ] || { echo "No libevmu checkout found at $SRC" >&2; exit 1; }
[ -d "$SRC/lib/lib/libgimbal/lib" ] || {
    echo "$SRC/lib/lib/libgimbal looks empty; is the libgimbal submodule checked out there?" >&2
    exit 1
}

echo "Syncing evmu library from $SRC..."
rsync -a --delete "$SRC/lib/source/" "$DST/lib/source/"
rsync -a --delete "$SRC/lib/api/" "$DST/lib/api/"
rsync -a --delete "$SRC/legacy/" "$DST/legacy/"
cp "$SRC/lib/CMakeLists.txt" "$DST/lib/CMakeLists.txt"
cp "$SRC/CMakeLists.txt" "$DST/CMakeLists.txt"

echo "Syncing libgimbal from $SRC/lib/lib/libgimbal..."
rsync -a --delete --exclude='.git' "$SRC/lib/lib/libgimbal/" "$DST/lib/lib/libgimbal/"

echo "Re-applying ESP32 libgimbal patches..."
for p in "$PATCH_DIR"/*.patch; do
    [ -e "$p" ] || continue
    echo "  $p"
    patch -p1 -d "$DST/lib/lib/libgimbal" < "$p"
done

echo
echo "Done. Review with 'git status' / 'git diff' in $(pwd), then rebuild before committing."
