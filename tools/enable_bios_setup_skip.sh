#!/bin/sh
# Opts into EvmuRom_skipBiosSetup() working with our BIOS.BIN, which is reassembled
# from the US v1.05 disassembly and so has a different CRC32 than the official dump
# (see main.c, start_session()). Drops a marker file into flash_data/, which
# "idf.py flash" bakes into the board's data partition on the next flash.
# Safe to run again; does nothing once the marker is already there.
# To undo: rm flash_data/BIOS_FORCE_TYPE.TXT and reflash.
set -eu

cd "$(dirname "$0")/../flash_data"
MARKER=BIOS_FORCE_TYPE.TXT

if [ -e "$MARKER" ]; then
    echo "ok: $MARKER already present, run idf.py flash to deploy it"
else
    echo "treat BIOS.BIN as the US v1.05 image" > "$MARKER"
    echo "created $MARKER, run idf.py flash to deploy it"
fi
