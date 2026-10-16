#pragma once
#include "ssd1681_fast.h"

/* Four bit planes count 0..8 independently for 32 pixels at once.
 * Internal SRAM: 25 KB, including the actual target image. */
#define DRIVE_WORDS (SSD1681_FRAME_BYTES / sizeof(uint32_t))
typedef struct {
    uint32_t target[DRIVE_WORDS], count[4][DRIVE_WORDS];
    unsigned pending_pixels;
} drive_counters_t;

void drive_counters_seed(drive_counters_t *c, const uint8_t *clean);
unsigned drive_counters_prepare(drive_counters_t *c, const uint8_t *target,
                               unsigned black_pulses, unsigned white_pulses,
                               uint8_t *black, uint8_t *white);
/* Consume only successfully delivered masks, before the next prepare. */
void drive_counters_complete(drive_counters_t *c, const uint8_t *black, const uint8_t *white);
