#include "drive_counters.h"
#include <string.h>

_Static_assert(SSD1681_FRAME_BYTES % sizeof(uint32_t) == 0, "whole mask words");

void drive_counters_seed(drive_counters_t *c, const uint8_t *clean)
{
    memcpy(c->target, clean, SSD1681_FRAME_BYTES);
    memset(c->count, 0, sizeof(c->count));
    c->pending_pixels = 0;
}

unsigned drive_counters_prepare(drive_counters_t *c, const uint8_t *target,
                               unsigned black_pulses, unsigned white_pulses,
                               uint8_t *black, uint8_t *white)
{
    unsigned pending = 0;
    for (unsigned i = 0; i < DRIVE_WORDS; ++i) {
        uint32_t next;
        memcpy(&next, target + i * sizeof(next), sizeof(next));
        const uint32_t changed = next ^ c->target[i];
        uint32_t active = 0;
        for (unsigned plane = 0; plane < 4; ++plane) {
            uint32_t value = c->count[plane][i];
            if (changed) {
                const uint32_t budget = ((black_pulses & (1u << plane)) ? ~next : 0) |
                                        ((white_pulses & (1u << plane)) ? next : 0);
                value = (value & ~changed) | (budget & changed);
                c->count[plane][i] = value;
            }
            active |= value;
        }
        c->target[i] = next;
        uint32_t b = active & ~next, w = active & next;
        memcpy(black + i * sizeof(b), &b, sizeof(b));
        memcpy(white + i * sizeof(w), &w, sizeof(w));
        pending += __builtin_popcount(active);
    }
    c->pending_pixels = pending;
    return pending;
}

void drive_counters_complete(drive_counters_t *c, const uint8_t *black, const uint8_t *white)
{
    unsigned pending = 0;
    for (unsigned i = 0; i < DRIVE_WORDS; ++i) {
        uint32_t b, w;
        memcpy(&b, black + i * sizeof(b), sizeof(b));
        memcpy(&w, white + i * sizeof(w), sizeof(w));
        uint32_t active = 0;
        for (unsigned plane = 0; plane < 4; ++plane) active |= c->count[plane][i];
        /* Subtract only from delivered, matching counters; an inactive pixel would wrap from 0 to 15. */
        uint32_t borrow = active & ((b & ~c->target[i]) | (w & c->target[i]));
        active = 0;
        for (unsigned plane = 0; plane < 4; ++plane) {
            uint32_t old = c->count[plane][i];
            c->count[plane][i] = old ^ borrow;
            borrow &= ~old;
            active |= c->count[plane][i];
        }
        pending += __builtin_popcount(active);
    }
    c->pending_pixels = pending;
}
