#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Folder for VMU images synced from a PC (Flycast). */
#define LINK_DIR "/data/FLY"

void link_start(void);

/* Implemented in main.c */
bool link_is_active_image(const char *path);
/* Restarts into FLY/<name>, or into the launcher when name is NULL, saving the running
 * VMU's flash first. */
void link_restart(const char *name);
/* Stops the running VMU for good (until restart), saving its flash first if asked. */
void link_halt_emulator(bool save);
/* A 48x32 VMU screen from the PC (Flycast), 1 bit per pixel, row-major, MSB first, 1 = on. */
void link_mirror_frame(const uint8_t bits[192]);
/* Opens the clock screen, as a BOOT+PWR tap does on battery. */
void link_clock(void);
/* Holds VMU buttons for ms: letters u d l r a b m (mode) s (sleep). */
void link_key(const char *buttons, unsigned ms);
/* The frame on the panel, 200x200, 1 bit per pixel, row-major, MSB first, 1 = white. */
const uint8_t *link_screen(void);
