# VMU

Dreamcast VMU emulator for the Waveshare ESP32-S3-ePaper-1.54, using Falco Girgis's
libEVMU for emulation and InkBoy's e-paper driver for the panel. See `main/main.c`'s
header comment for how it behaves on the board.

**This firmware project lives on the `main` branch of the `libevmu` repo, separate
from the libEVMU library itself (which lives on its own branches).** A plain `git
clone` checks out the library's default branch, not this one — run `git checkout
main` right after cloning, before anything below.

## Build and flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.1 installed and
its `export.sh` sourced in your shell. This is a full SDK/toolchain install (not
bundled in this repo) and is the only extra thing you need beyond cloning and
connecting the board — everything else (`libevmu`, the e-paper driver, flash data)
is vendored in-tree.

Once ESP-IDF is set up, connect the board over USB-C, then from this directory:

```sh
idf.py build
idf.py -p PORT flash      # or just `idf.py flash` to auto-detect the port
```

The target (`esp32s3`) is already set via `sdkconfig.defaults`. If the project
directory has been moved or renamed since the last build, run `idf.py fullclean`
first — the build cache embeds the old path.

If the port isn't accessible without `sudo`, run `tools/serial_permissions.sh` once.

## Data on the device

`flash_data/` is baked into the board's data partition on every flash:

- `BIOS.BIN` — VMU BIOS image (optional; without it the VMU falls back to software
  BIOS emulation)
- `VMU.BIN` — the default "My VMU" save image
- `LIB/` — programs listed in the launcher, built with `tools/make_library.py`

`BIOS.BIN` here is reassembled from the US v1.05 disassembly, so it doesn't skip its
date/time setup screen by default. Run `tools/enable_bios_setup_skip.sh` once, before
flashing, to opt into skipping it.

## Afterward

`tools/flycast_sync.py` syncs VMU images between Flycast on the PC and the board over
USB (`list` / `push` / `pull` / `sync` / `run NAME` / `menu`).
