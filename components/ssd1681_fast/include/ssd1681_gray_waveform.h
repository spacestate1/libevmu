#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Bounded, volatile grayscale waveform variants. No arbitrary LUT/register interface. */
typedef enum {
    SSD1681_GRAY_WAVE_NORMAL,
    SSD1681_GRAY_WAVE_WEAK11,
    SSD1681_GRAY_WAVE_WEAK88,
    SSD1681_GRAY_WAVE_COUNT
} ssd1681_gray_wave_mode_t;

static inline bool ssd1681_gray_wave_weak(unsigned mode) {
    return mode>=SSD1681_GRAY_WAVE_WEAK11 && mode<=SSD1681_GRAY_WAVE_WEAK88;
}
/* Patches the one-frame FR7 explicit-mask partial LUT only. Rows: 00 HOLD, 01 white, 10 black,
 * 11 special; G1 phase A is selector bits 7:6, other rows, phases and VCOM are unchanged. */
static inline void ssd1681_gray_wave_lut(uint8_t *lut, unsigned mode) {
    if (ssd1681_gray_wave_weak(mode)) lut[37]=0xc0; /* A: VSH2 */
}
static inline uint8_t ssd1681_gray_wave_vsh2(unsigned mode) {
    /* SSD1681 rev0.13 command04 table: 2D=11V, CE=8.8V, B0=5.8V. */
    return mode==SSD1681_GRAY_WAVE_WEAK11 ? 0x2d :
           mode==SSD1681_GRAY_WAVE_WEAK88 ? 0xce : 0xb0;
}
