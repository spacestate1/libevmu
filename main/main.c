/* Dreamcast VMU on the Waveshare ESP32-S3-ePaper-1.54, using Falco Girgis's libEVMU
 * for emulation and InkBoy's per-pixel fast drive for the panel.
 *
 * At boot a launcher lists "My VMU" (VMU.BIN, the saves image) and every program in
 * LIB/INDEX.TXT. Each program runs on its own VMU image in G/, created on first launch,
 * with the VMU BIOS (BIOS.BIN). The emulated flash is written back to its image when it
 * changes, and the clock comes from the board's PCF85063 RTC.
 *
 * While Flycast on the PC streams its VMU screen (core/rend/vmu_mirror.cpp), the board
 * shows that screen instead and the emulated VMU pauses.
 *
 * VMU images synced from Flycast on the PC (tools/flycast_sync.py) live in FLY/ and
 * run in place; they are listed after "My VMU".
 *
 * BIOS.BIN is reassembled from the US v1.05 disassembly, so run
 * tools/enable_bios_setup_skip.sh once before flashing to also skip its
 * date/time setup screen.
 *
 * Buttons (header pins switch to GND):
 *   GP1 Up, GP2 Down, GP3 Left, GP43 Right, GP44 A, PWR B, BOOT Mode, BOOT+PWR Sleep.
 *   Hold BOOT+PWR for 3 s to return to the launcher.
 *   On battery (no USB host), a tap of BOOT+PWR pauses the VMU and shows a clock;
 *   any button goes back.
 *   Launcher: Up/Down or PWR to move, A or BOOT to start. */
#include <evmu/hw/evmu_device.h>
#include <evmu/hw/evmu_flash.h>
#include <evmu/hw/evmu_gamepad.h>
#include <evmu/hw/evmu_lcd.h>
#include <evmu/hw/evmu_rom.h>
#include <evmu/fs/evmu_file_manager.h>
#include <evmu/types/evmu_ibehavior.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "drive_counters.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1681_fast.h"
#include "link.h"

static const char *TAG = "vmu";

#define BIOS_PATH "/data/BIOS.BIN"
#define BIOS_FORCE_TYPE_PATH "/data/BIOS_FORCE_TYPE.TXT"
#define SAVES_PATH "/data/VMU.BIN"
#define LIB_DIR "/data/LIB"
#define GAMES_DIR "/data/G"
#define CURRENT_PATH "/data/CURRENT.TXT"
#define LOG_PATH "/data/LOG.TXT"
#define LOG_MAX_BYTES 16384
#define LCD_W EVMU_LCD_PIXEL_WIDTH
#define LCD_H EVMU_LCD_PIXEL_HEIGHT
#define SCALE 4
#define ORIGIN_X ((SSD1681_WIDTH - LCD_W * SCALE) / 2)
#define ORIGIN_Y ((SSD1681_HEIGHT - LCD_H * SCALE) / 2)
#define ROW_BYTES (SSD1681_WIDTH / 8)
#define FRAME_NS (1000000000ull / 60)
#define DRIVE_PERIOD_US 30000
#define DRIVE_PULSES 8
#define IDLE_POWER_OFF_US 1000000
// The fast drive leaves residue that adds up; a full refresh clears it once this many
// panel pixels have changed and the picture has then been still for CLEAN_QUIET_US
// (or after CLEAN_MAX_US even if it never is).
#define CLEAN_MIN_PIXELS 2000
#define CLEAN_QUIET_US 1500000
#define CLEAN_MAX_US 180000000
#define SAVE_CHECK_US 2000000
#define LAUNCHER_HOLD_US 3000000
// BOOT or PWR alone counts as Mode or B only after this long, so the first half of a
// BOOT+PWR chord doesn't reach the VMU
#define CHORD_US 40000
#define MIRROR_TIMEOUT_US 3000000
// clock screen: the creature walks for CLOCK_WALK_US, one step per CLOCK_STEP_US (the
// fast drive needs ~250 ms per change), then falls asleep so the panel can rest
#define CLOCK_WALK_US 60000000
#define CLOCK_STEP_US 500000
#define VMU_FLASH_BYTES (128 * 1024)
#define MAX_PROGRAMS 160
#define TITLE_LEN 32

#define PIN_BOOT 0
#define PIN_PWR_BUTTON 18
#define PIN_UP 1
#define PIN_DOWN 2
#define PIN_LEFT 3
#define PIN_RIGHT 43
#define PIN_A 44
#define PIN_VBAT_LATCH 17
#define PIN_AUDIO_PWR 42
#define PIN_EPD_PWR 6
#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define RTC_ADDR 0x51

/* Frames handed from the producer (launcher or emulator) to the display task. */
static uint8_t s_frames[2][SSD1681_FRAME_BYTES];
static _Atomic unsigned s_frame_latest;

static _Atomic bool s_full_request; // clean the panel with a full refresh now
static uint8_t s_black[SSD1681_FRAME_BYTES], s_white[SSD1681_FRAME_BYTES];
static uint8_t s_shown[SSD1681_FRAME_BYTES]; // the last frame the display task took
static drive_counters_t s_counters;

typedef struct {
    char file[48]; // LIB file name, "FLY:<name>" for a Flycast VMU, empty for the saves VMU
    char title[TITLE_LEN];
} program_t;

static program_t *s_programs;
static unsigned s_program_count;
static char s_active_image[64];

/* ---------- frame drawing ---------- */

// 5x7 glyphs, one byte per column (LSB = top row), for ' '..'Z'
static const uint8_t s_font[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, // sp ! "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, // # $ %
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00}, // & ' (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08}, // ) * +
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, // , - .
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, // / 0 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, // 2 3 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, // 5 6 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, // 8 9 :
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, // ; < =
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E}, // > ? @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22}, // A B C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, // D E F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, // G H I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40}, // J K L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E}, // M N O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, // P Q R
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, // S T U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63}, // V W X
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},                                 // Y Z
};

static void px(uint8_t *frame, int x, int y, bool black)
{
    if (x < 0 || y < 0 || x >= SSD1681_WIDTH || y >= SSD1681_HEIGHT) return;
    uint8_t mask = 0x80u >> (x % 8);
    if (black) {
        frame[y * ROW_BYTES + x / 8] &= ~mask;
    } else {
        frame[y * ROW_BYTES + x / 8] |= mask;
    }
}

static void fill(uint8_t *frame, int x0, int y0, int w, int h, bool black)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) px(frame, x, y, black);
    }
}

/* Text with each font pixel drawn as a scale x scale block; 6 * scale pixels per character. */
static void text_scaled(uint8_t *frame, int x, int y, const char *s, bool black, int scale)
{
    for (; *s && x < SSD1681_WIDTH; s++, x += 6 * scale) {
        char c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (c < ' ' || c > 'Z') c = '?';
        const uint8_t *g = s_font[c - ' '];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1 << row)) fill(frame, x + col * scale, y + row * scale, scale, scale, black);
            }
        }
    }
}

static void text(uint8_t *frame, int x, int y, const char *s, bool black)
{
    text_scaled(frame, x, y, s, black, 1);
}

static uint8_t *next_frame(void)
{
    return s_frames[!atomic_load(&s_frame_latest)];
}

static void publish_frame(void)
{
    atomic_store(&s_frame_latest, !atomic_load(&s_frame_latest));
}

/* Draws a 48x32 screen (1 bit per pixel, MSB first, 1 = on), scaled and framed. */
static void render_bits(uint8_t *frame, const uint8_t *bits)
{
    memset(frame, 0xFF, SSD1681_FRAME_BYTES);
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            int i = y * LCD_W + x;
            if (bits[i / 8] & (0x80 >> (i % 8))) {
                fill(frame, ORIGIN_X + x * SCALE, ORIGIN_Y + y * SCALE, SCALE, SCALE, true);
            }
        }
    }
    // thin frame around the LCD area
    fill(frame, ORIGIN_X - 3, ORIGIN_Y - 3, LCD_W * SCALE + 6, 1, true);
    fill(frame, ORIGIN_X - 3, ORIGIN_Y + LCD_H * SCALE + 2, LCD_W * SCALE + 6, 1, true);
    fill(frame, ORIGIN_X - 3, ORIGIN_Y - 3, 1, LCD_H * SCALE + 6, true);
    fill(frame, ORIGIN_X + LCD_W * SCALE + 2, ORIGIN_Y - 3, 1, LCD_H * SCALE + 6, true);
}

static void render_lcd(uint8_t *frame, const EvmuDevice *dev)
{
    uint8_t bits[LCD_W * LCD_H / 8] = {0};
    if (EvmuLcd_screenEnabled(dev->pLcd)) {
        for (int i = 0; i < LCD_W * LCD_H; i++) {
            if (EvmuLcd_pixel(dev->pLcd, i % LCD_W, i / LCD_W)) bits[i / 8] |= 0x80 >> (i % 8);
        }
    }
    render_bits(frame, bits);
}

/* Last time a screen arrived from Flycast; while recent, it owns the display. */
static _Atomic int64_t s_mirror_last_us;

static bool mirror_active(void)
{
    int64_t last = atomic_load(&s_mirror_last_us);
    return last && esp_timer_get_time() - last < MIRROR_TIMEOUT_US;
}

void link_mirror_frame(const uint8_t bits[192])
{
    if (!mirror_active()) {
        ESP_LOGI(TAG, "showing Flycast's VMU screen");
        atomic_store(&s_full_request, true);
    }
    atomic_store(&s_mirror_last_us, esp_timer_get_time());
    render_bits(next_frame(), bits);
    publish_frame();
}

/* ---------- display ---------- */

/* Redraws the whole panel with the vendor waveform, clearing fast-drive residue. */
static void full_refresh(ssd1681_handle_t d, const uint8_t *frame)
{
    ssd1681_timing_t timing;
    ESP_ERROR_CHECK(ssd1681_vendor_full(d, frame, SSD1681_FRAME_BYTES, &timing));
    ESP_ERROR_CHECK(ssd1681_vendor_enable_mixed(d));
    ESP_ERROR_CHECK(ssd1681_vendor_set_mixed_frames(d, 1));
    ESP_ERROR_CHECK(ssd1681_vendor_set_fast_transport(d, true));
    drive_counters_seed(&s_counters, frame);
    memcpy(s_shown, frame, SSD1681_FRAME_BYTES);
    ESP_LOGI(TAG, "full refresh %lld us", timing.busy_us);
}

/* Panel pixels that differ between two frames. */
static unsigned changed_pixels(const uint8_t *a, const uint8_t *b)
{
    unsigned n = 0;
    for (size_t i = 0; i < SSD1681_FRAME_BYTES; i += sizeof(uint32_t)) {
        uint32_t x, y;
        memcpy(&x, a + i, sizeof(x));
        memcpy(&y, b + i, sizeof(y));
        n += __builtin_popcount(x ^ y);
    }
    return n;
}

static void display_task(void *arg)
{
    (void)arg;
    ssd1681_handle_t d = NULL;
    ssd1681_timing_t timing;
    ESP_ERROR_CHECK(ssd1681_create(&d));
    ESP_ERROR_CHECK(ssd1681_enable_continuous_play(d));
    ESP_ERROR_CHECK(ssd1681_power_on(d));
    atomic_store(&s_full_request, false);
    full_refresh(d, s_frames[atomic_load(&s_frame_latest)]);

    // "prepared": a fast scan has run since the full refresh; only then can the panel's
    // analog supply be switched off when idle and resumed later
    bool hold_end_pending = false, analog_on = true, prepared = false;
    int64_t last_work = esp_timer_get_time(), last_change = last_work, last_clean = last_work;
    unsigned dirty = 0; // pixels changed by the fast drive since the last full refresh
    static uint8_t work[SSD1681_FRAME_BYTES];
    for (;;) {
        int64_t start = esp_timer_get_time();
        // copy the frame once: the producer starts rewriting this buffer one frame after
        // publishing the other, so a pass reading it in place could see a torn picture
        memcpy(work, s_frames[atomic_load(&s_frame_latest)], SSD1681_FRAME_BYTES);
        const uint8_t *frame = work;
        unsigned changed = changed_pixels(frame, s_shown);
        if (changed) {
            memcpy(s_shown, frame, SSD1681_FRAME_BYTES);
            dirty += changed;
            last_change = start;
        }
        bool settled = !hold_end_pending && start - last_change >= CLEAN_QUIET_US;
        if (atomic_exchange(&s_full_request, false) ||
            (dirty >= CLEAN_MIN_PIXELS && (settled || start - last_clean >= CLEAN_MAX_US))) {
            full_refresh(d, frame);
            dirty = 0;
            hold_end_pending = prepared = false;
            analog_on = true;
            last_work = last_clean = start;
            continue;
        }
        unsigned driven = drive_counters_prepare(&s_counters, frame, DRIVE_PULSES, DRIVE_PULSES, s_black,
                                                 s_white);
        if (driven || hold_end_pending) {
            if (!analog_on) {
                ESP_ERROR_CHECK(ssd1681_vendor_mixed_resume(d, &timing));
                analog_on = true;
            }
            ESP_ERROR_CHECK(ssd1681_vendor_mixed_scan(d, s_black, s_white, sizeof(s_black), &timing));
            prepared = true;
            drive_counters_complete(&s_counters, s_black, s_white);
            // finish every burst with one all-HOLD scan so the waveform ends cleanly
            hold_end_pending = driven != 0;
            last_work = start;
        } else if (analog_on && prepared && start - last_work > IDLE_POWER_OFF_US) {
            ESP_ERROR_CHECK(ssd1681_analog_power(d, false, &timing));
            analog_on = false;
        }

        int64_t wait = start + DRIVE_PERIOD_US - esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(wait > 1000 ? (wait + 999) / 1000 : 1));
    }
}

/* ---------- board ---------- */

/* Appends a line to LOG.TXT, so what happens away from the PC can be read later (@@LOG). */
static void boot_log(const char *fmt, ...)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        remove("/data/LOG.OLD");
        rename(LOG_PATH, "/data/LOG.OLD");
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%8lld ", esp_timer_get_time() / 1000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void board_init(void)
{
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_VBAT_LATCH) | (1ULL << PIN_AUDIO_PWR),
    };
    gpio_set_level(PIN_VBAT_LATCH, 1); // keep the board on when running from battery
    gpio_set_level(PIN_AUDIO_PWR, 1);  // audio amplifier off
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BOOT) | (1ULL << PIN_PWR_BUTTON) | (1ULL << PIN_UP) |
                        (1ULL << PIN_DOWN) | (1ULL << PIN_LEFT) | (1ULL << PIN_RIGHT) |
                        (1ULL << PIN_A),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

static bool pressed(int pin) { return gpio_get_level(pin) == 0; }

/* Pressed and still pressed 50 ms later, so glitches during resets can't drive the launcher. */
static bool pressed_steady(int pin)
{
    if (!pressed(pin)) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    return pressed(pin);
}

static uint8_t from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Reads the PCF85063 RTC. If its time was never set or is behind the firmware build
 * time, sets it to the build time first. */
static bool rtc_read(GblDateTime *out)
{
    // the I2C peripherals are powered from the switched rails (both active low)
    gpio_config_t rails = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = (1ULL << PIN_EPD_PWR)};
    gpio_set_level(PIN_EPD_PWR, 0);
    gpio_config(&rails);
    gpio_set_level(PIN_AUDIO_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t rtc = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC: I2C bus init failed: %s", esp_err_to_name(err));
        gpio_set_level(PIN_AUDIO_PWR, 1);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 100000,
    };
    bool ok = i2c_master_bus_add_device(bus, &dev_cfg, &rtc) == ESP_OK;

    uint8_t reg = 0x04, t[7];
    err = ok ? i2c_master_transmit_receive(rtc, &reg, 1, t, sizeof(t), 100) : ESP_FAIL;
    if (err != ESP_OK) ESP_LOGW(TAG, "RTC: read failed: %s", esp_err_to_name(err));
    ok = err == ESP_OK;

    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    int month = (int)((strstr(months, mon) - months) / 3) + 1;
    long long rtc_stamp = ok ? ((((2000LL + from_bcd(t[6])) * 100 + from_bcd(t[5] & 0x1F)) * 100 +
                                 from_bcd(t[3] & 0x3F)) * 100 + from_bcd(t[2] & 0x3F)) * 100 +
                                   from_bcd(t[1] & 0x7F)
                             : 0;
    long long build_stamp = ((((long long)year * 100 + month) * 100 + day) * 100 + hh) * 100 + mm;
    if (ok && ((t[0] & 0x80) || rtc_stamp < build_stamp)) {
        uint8_t w[8] = {0x04,        to_bcd(ss), to_bcd(mm),    to_bcd(hh),
                        to_bcd(day), 0,          to_bcd(month), to_bcd(year % 100)};
        ok = i2c_master_transmit(rtc, w, sizeof(w), 100) == ESP_OK;
        ok = ok && i2c_master_transmit_receive(rtc, &reg, 1, t, sizeof(t), 100) == ESP_OK;
        ESP_LOGI(TAG, "RTC was unset or behind; set it from build time");
    }
    if (rtc) i2c_master_bus_rm_device(rtc);
    i2c_del_master_bus(bus);
    gpio_set_level(PIN_AUDIO_PWR, 1);
    if (!ok) return false;

    memset(out, 0, sizeof(*out));
    out->time.seconds = from_bcd(t[0] & 0x7F);
    out->time.minutes = from_bcd(t[1] & 0x7F);
    out->time.hours = from_bcd(t[2] & 0x3F);
    out->date.day = from_bcd(t[3] & 0x3F);
    out->date.month = from_bcd(t[5] & 0x1F);
    out->date.year = 2000 + from_bcd(t[6]);
    ESP_LOGI(TAG, "RTC time %04d-%02d-%02d %02d:%02d:%02d", (int)out->date.year,
             (int)out->date.month, (int)out->date.day, (int)out->time.hours,
             (int)out->time.minutes, (int)out->time.seconds);
    return true;
}

/* ---------- launcher ---------- */

static void load_programs(void)
{
    s_programs = heap_caps_calloc(MAX_PROGRAMS, sizeof(program_t), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_programs ? ESP_OK : ESP_ERR_NO_MEM);
    strcpy(s_programs[0].title, "My VMU (saves)");
    s_program_count = 1;
    // Flycast VMUs first, then the library; both indexes are "FILE|Title" lines
    static const struct { const char *index, *prefix; } sources[] = {
        {LINK_DIR "/INDEX.TXT", "FLY:"},
        {LIB_DIR "/INDEX.TXT", ""},
    };
    for (unsigned src = 0; src < 2; src++) {
        FILE *f = fopen(sources[src].index, "r");
        char line[96];
        while (f && s_program_count < MAX_PROGRAMS && fgets(line, sizeof(line), f)) {
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            bar[1 + strcspn(bar + 1, "\r\n")] = 0;
            program_t *p = &s_programs[s_program_count++];
            snprintf(p->file, sizeof(p->file), "%s%.40s", sources[src].prefix, line);
            snprintf(p->title, sizeof(p->title), "%.31s", bar + 1);
        }
        if (f) fclose(f);
    }
    ESP_LOGI(TAG, "launcher: %u entries", s_program_count);
}

static unsigned current_choice(void)
{
    char name[48] = {0};
    FILE *f = fopen(CURRENT_PATH, "r");
    if (f) {
        if (!fgets(name, sizeof(name), f)) name[0] = 0;
        fclose(f);
    }
    name[strcspn(name, "\r\n")] = 0;
    for (unsigned i = 1; i < s_program_count; i++) {
        if (!strcmp(s_programs[i].file, name)) return i;
    }
    // a Flycast VMU started with @@RUN before INDEX.TXT lists it: add it under its file name
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), LINK_DIR "/%.40s", name + 4);
    if (!strncmp(name, "FLY:", 4) && stat(path, &st) == 0 && s_program_count < MAX_PROGRAMS) {
        program_t *p = &s_programs[s_program_count];
        snprintf(p->file, sizeof(p->file), "%s", name);
        snprintf(p->title, sizeof(p->title), "%.31s", name + 4);
        return s_program_count++;
    }
    if (name[0]) ESP_LOGW(TAG, "%s (from CURRENT.TXT) is in no INDEX.TXT; using My VMU", name);
    return 0;
}

static void draw_launcher(unsigned selected)
{
    enum { ROW_H = 10, TOP = 14, ROWS = (SSD1681_HEIGHT - TOP) / ROW_H };
    uint8_t *frame = next_frame();
    memset(frame, 0xFF, SSD1681_FRAME_BYTES);
    fill(frame, 0, 0, SSD1681_WIDTH, 11, true);
    char header[40];
    snprintf(header, sizeof(header), "VMU LIBRARY  %u/%u", selected + 1, s_program_count);
    text(frame, 3, 2, header, false);

    unsigned first = selected >= ROWS / 2 ? selected - ROWS / 2 : 0;
    if (first + ROWS > s_program_count) first = s_program_count > ROWS ? s_program_count - ROWS : 0;
    for (unsigned r = 0; r < ROWS && first + r < s_program_count; r++) {
        unsigned i = first + r;
        int y = TOP + r * ROW_H;
        bool sel = i == selected;
        if (sel) fill(frame, 0, y - 1, SSD1681_WIDTH, ROW_H, true);
        text(frame, 4, y + 1, s_programs[i].title, !sel);
    }
    publish_frame();
}

/* Shows the launcher until a program is chosen; returns its index. */
static unsigned run_launcher(unsigned selected)
{
    // wait for the buttons that opened the launcher to be released
    while (pressed(PIN_BOOT) || pressed(PIN_PWR_BUTTON) || pressed(PIN_A)) vTaskDelay(pdMS_TO_TICKS(20));
    draw_launcher(selected);
    int64_t held_since = 0, last_step = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (pressed_steady(PIN_A) || pressed_steady(PIN_BOOT)) {
            ESP_LOGI(TAG, "launcher: start %u (%s pressed)", selected, pressed(PIN_A) ? "A" : "BOOT");
            while (pressed(PIN_A) || pressed(PIN_BOOT)) vTaskDelay(pdMS_TO_TICKS(20));
            return selected;
        }
        int dir = pressed_steady(PIN_UP)                                     ? -1
                  : (pressed_steady(PIN_DOWN) || pressed_steady(PIN_PWR_BUTTON)) ? 1
                                                                                  : 0;
        int64_t now = esp_timer_get_time();
        if (!dir) {
            held_since = 0;
            continue;
        }
        // first step at once, then auto-repeat after 400 ms at 8 steps/s
        bool step = !held_since || (now - held_since > 400000 && now - last_step > 125000);
        if (!held_since) held_since = now;
        if (!step) continue;
        last_step = now;
        selected = (selected + s_program_count + dir) % s_program_count;
        draw_launcher(selected);
    }
}

/* ---------- emulator ---------- */

typedef struct {
    EvmuDevice *dev;
    char image[64];     // VMU flash image backing this session
    bool fresh;         // image not written yet
    uint8_t *flash_buf; // scratch copy of the emulated flash
    uint32_t saved_crc; // CRC of the flash as last written to the image
} session_t;

static uint32_t flash_crc(EvmuDevice *dev, uint8_t *buf)
{
    size_t bytes = VMU_FLASH_BYTES;
    EvmuFlash_readBytes(dev->pFlash, 0, buf, &bytes);
    return esp_rom_crc32_le(0, buf, VMU_FLASH_BYTES);
}

static bool save_image(const char *path, const uint8_t *buf)
{
    char tmp[72];
    snprintf(tmp, sizeof(tmp), "%s.TMP", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, VMU_FLASH_BYTES, f) == VMU_FLASH_BYTES;
    if (f) ok = (fclose(f) == 0) && ok;
    if (ok) {
        // FAT can't rename over a file; recover_tmp_files() finishes this if power is lost here
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}

/* Finishes or discards the .TMP files a save left behind when power was lost: a .TMP is
 * only complete once its file has been removed, so it replaces a missing file and is
 * dropped otherwise. */
static void recover_tmp_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || strcasecmp(e->d_name + len - 4, ".TMP")) continue;
        char tmp[80], path[80];
        snprintf(tmp, sizeof(tmp), "%s/%.60s", dir_path, e->d_name);
        snprintf(path, sizeof(path), "%.*s", (int)strlen(tmp) - 4, tmp);
        struct stat st;
        bool keep = stat(path, &st) != 0;
        bool ok = keep ? rename(tmp, path) == 0 : remove(tmp) == 0;
        ESP_LOGW(TAG, "%s %s left by an interrupted save: %s", keep ? "restored" : "dropped", tmp,
                 ok ? "ok" : "FAILED");
    }
    if (dir) closedir(dir);
}

/* Writes the VMU flash back to its image if it changed since the last save. */
static void save_if_changed(session_t *s)
{
    if (!s->flash_buf) return;
    uint32_t crc = flash_crc(s->dev, s->flash_buf);
    if (crc == s->saved_crc) return;
    bool ok = save_image(s->image, s->flash_buf);
    if (ok) s->saved_crc = crc;
    ESP_LOGI(TAG, "VMU flash changed, saved %s: %s", s->image, ok ? "ok" : "FAILED");
}

/* Buttons held from the PC (link_key): a mask of KEY_* bits until s_key_until. */
enum { KEY_UP = 1, KEY_DOWN = 2, KEY_LEFT = 4, KEY_RIGHT = 8, KEY_A = 16, KEY_B = 32, KEY_MODE = 64, KEY_SLEEP = 128 };
static _Atomic unsigned s_key_mask;
static _Atomic int64_t s_key_until;

void link_key(const char *buttons, unsigned ms)
{
    static const char letters[] = "udlrabms";
    unsigned mask = 0;
    for (const char *c = buttons; *c; c++) {
        const char *p = strchr(letters, *c);
        if (p) mask |= 1u << (p - letters);
    }
    atomic_store(&s_key_until, esp_timer_get_time() + ms * 1000ll);
    atomic_store(&s_key_mask, mask);
}

const uint8_t *link_screen(void) { return s_frames[atomic_load(&s_frame_latest)]; }

static bool on_battery(void) { return !usb_serial_jtag_is_connected(); }

/* Sets the VMU's buttons from the pins and the PC; returns whether BOOT+PWR are held.
 * On battery that chord is kept from the VMU: a tap of it opens the clock instead. */
static bool read_buttons(EvmuDevice *dev, int64_t now)
{
    static int64_t boot_since, pwr_since;
    static bool chord_seen; // BOOT+PWR were held together; Mode/B stay off until both are up
    unsigned key = now < atomic_load(&s_key_until) ? atomic_load(&s_key_mask) : 0;
    bool boot = pressed(PIN_BOOT), pwr = pressed(PIN_PWR_BUTTON), chord = boot && pwr;
    boot_since = boot ? (boot_since ? boot_since : now) : 0;
    pwr_since = pwr ? (pwr_since ? pwr_since : now) : 0;
    chord_seen = chord || (chord_seen && (boot || pwr));

    EvmuGamepad *pad = dev->pGamepad;
    pad->up = pressed(PIN_UP) || (key & KEY_UP);
    pad->down = pressed(PIN_DOWN) || (key & KEY_DOWN);
    pad->left = pressed(PIN_LEFT) || (key & KEY_LEFT);
    pad->right = pressed(PIN_RIGHT) || (key & KEY_RIGHT);
    pad->a = pressed(PIN_A) || (key & KEY_A);
    pad->sleep = (chord && !on_battery()) || (key & KEY_SLEEP);
    pad->mode = (boot && !chord_seen && now - boot_since >= CHORD_US) || (key & KEY_MODE);
    pad->b = (pwr && !chord_seen && now - pwr_since >= CHORD_US) || (key & KEY_B);
    return chord;
}

static bool any_button(void)
{
    if (esp_timer_get_time() < atomic_load(&s_key_until) && atomic_load(&s_key_mask)) return true;
    return pressed(PIN_BOOT) || pressed(PIN_PWR_BUTTON) || pressed(PIN_UP) || pressed(PIN_DOWN) ||
           pressed(PIN_LEFT) || pressed(PIN_RIGHT) || pressed(PIN_A);
}

static RTC_NOINIT_ATTR uint32_t s_open_launcher;
#define OPEN_LAUNCHER_MAGIC 0x564d554c

bool link_is_active_image(const char *path)
{
    return !strcmp(path, s_active_image);
}

/* Halting the emulator for a restart: link_halt_emulator() asks, the emulator task
 * saves (HALT_SAVE) or not (HALT_DISCARD), answers HALT_DONE and stops for good. */
enum { HALT_NONE, HALT_SAVE, HALT_DISCARD, HALT_DONE };
static _Atomic int s_halt;
static _Atomic bool s_emulating;

void link_halt_emulator(bool save)
{
    if (!atomic_load(&s_emulating)) return;
    atomic_store(&s_halt, save ? HALT_SAVE : HALT_DISCARD);
    // a save writes 128 KB to wear-levelled flash, which can take a couple of seconds
    for (int i = 0; i < 500 && atomic_load(&s_halt) != HALT_DONE; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

static void check_halt(session_t *s)
{
    int halt = atomic_load(&s_halt);
    if (halt != HALT_SAVE && halt != HALT_DISCARD) return;
    if (halt == HALT_SAVE) save_if_changed(s);
    atomic_store(&s_emulating, false);
    atomic_store(&s_halt, HALT_DONE);
    for (;;) vTaskDelay(portMAX_DELAY);
}

void link_restart(const char *name)
{
    link_halt_emulator(true); // keep what was played since the last periodic save
    if (name) {
        FILE *f = fopen(CURRENT_PATH, "w");
        if (f) {
            fprintf(f, "FLY:%s\n", name);
            fclose(f);
        }
    } else {
        s_open_launcher = OPEN_LAUNCHER_MAGIC;
    }
    esp_restart();
}

/* ---------- clock ---------- */

/* Wall time: the RTC as read at boot plus the time since, in seconds since 1970 with the
 * RTC's local time treated as UTC. 0 when the RTC could not be read. */
static time_t s_clock_base;
static int64_t s_clock_base_us;

static time_t datetime_to_time(const GblDateTime *dt)
{
    struct tm tm = {
        .tm_year = dt->date.year - 1900,
        .tm_mon = dt->date.month - 1,
        .tm_mday = dt->date.day,
        .tm_hour = dt->time.hours,
        .tm_min = dt->time.minutes,
        .tm_sec = dt->time.seconds,
    };
    return mktime(&tm); // TZ is unset, so this is UTC
}

static time_t clock_now(void)
{
    return s_clock_base + (esp_timer_get_time() - s_clock_base_us) / 1000000;
}

static void clock_set_base(const GblDateTime *dt)
{
    s_clock_base = datetime_to_time(dt);
    s_clock_base_us = esp_timer_get_time();
}

static void time_to_datetime(time_t t, GblDateTime *out)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    memset(out, 0, sizeof(*out));
    out->date.year = tm.tm_year + 1900;
    out->date.month = tm.tm_mon + 1;
    out->date.day = tm.tm_mday;
    out->time.hours = tm.tm_hour;
    out->time.minutes = tm.tm_min;
    out->time.seconds = tm.tm_sec;
}

/* The clock's creature, 16x16, facing right, drawn at 3x. */
enum { SPRITE_W = 16, SPRITE_SCALE = 3 };
static const char *const s_sprite[3][16] = {
    { // walking, left foot forward
        "................",
        ".....######.....",
        "...##......##...",
        "..#..........#..",
        ".#........##..#.",
        ".#........##..#.",
        ".#............#.",
        ".#.........#..#.",
        ".#..........###.",
        ".#............#.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "....#......#....",
        "...##.......##..",
        "................",
    },
    { // walking, right foot forward
        "................",
        "................",
        ".....######.....",
        "...##......##...",
        "..#..........#..",
        ".#........##..#.",
        ".#........##..#.",
        ".#............#.",
        ".#.........#..#.",
        ".#..........###.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "......#..#......",
        ".....##.##......",
        "................",
    },
    { // asleep
        "................",
        "................",
        "................",
        "................",
        ".....######.....",
        "...##......##...",
        "..#..........#..",
        ".#............#.",
        ".#.......###..#.",
        ".#............#.",
        ".#...........##.",
        ".#............#.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "................",
    },
};

static void draw_sprite(uint8_t *frame, int x, int y, int which, bool facing_left)
{
    for (int row = 0; row < SPRITE_W; row++) {
        for (int col = 0; col < SPRITE_W; col++) {
            if (s_sprite[which][row][facing_left ? SPRITE_W - 1 - col : col] != '#') continue;
            fill(frame, x + col * SPRITE_SCALE, y + row * SPRITE_SCALE, SPRITE_SCALE, SPRITE_SCALE, true);
        }
    }
}

static void draw_clock(time_t t, int step, bool asleep)
{
    enum { GROUND = 186, SIZE = SPRITE_W * SPRITE_SCALE, MIN_X = 6, MAX_X = SSD1681_WIDTH - 6 - SIZE, STRIDE = 6 };
    static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t *frame = next_frame();
    memset(frame, 0xFF, SSD1681_FRAME_BYTES);

    char line[24];
    snprintf(line, sizeof(line), "%s %d %s %d", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900);
    int w = (int)strlen(line) * 12 - 2;
    text_scaled(frame, (SSD1681_WIDTH - w) / 2, 22, line, true, 2);
    snprintf(line, sizeof(line), "%02d:%02d", tm.tm_hour, tm.tm_min);
    text_scaled(frame, (SSD1681_WIDTH - (5 * 36 - 6)) / 2, 58, line, true, 6);
    fill(frame, 0, GROUND, SSD1681_WIDTH, 2, true);

    // walk back and forth along the ground, bobbing with each step
    int span = (MAX_X - MIN_X) / STRIDE, pos = step % (2 * span);
    bool left = pos >= span;
    int x = MIN_X + STRIDE * (left ? 2 * span - pos : pos);
    if (asleep) {
        draw_sprite(frame, x, GROUND - SIZE, 2, left);
        text_scaled(frame, x + (left ? -14 : SIZE + 2), GROUND - SIZE - 6, "Z", true, 2);
        text(frame, x + (left ? -20 : SIZE + 14), GROUND - SIZE - 14, "Z", true);
    } else {
        draw_sprite(frame, x, GROUND - SIZE, step & 1, left);
    }
    publish_frame();
}

static _Atomic bool s_clock_request; // the PC asked for the clock (@@CLOCK)

void link_clock(void) { atomic_store(&s_clock_request, true); }

/* Shows the clock until a button is pressed. The VMU is paused meanwhile; its own clock is
 * set to the wall time when it resumes. */
static void run_clock(session_t *s)
{
    ESP_LOGI(TAG, "clock");
    boot_log("clock opened, battery %d", on_battery());
    save_if_changed(s);
    if (!s_clock_base) {
        GblDateTime dt;
        clock_set_base(EvmuRom_dateTime(s->dev->pRom, &dt)); // no RTC: trust the VMU's clock
    }
    atomic_store(&s_full_request, true);
    while (any_button()) vTaskDelay(pdMS_TO_TICKS(20)); // the chord that opened it

    int64_t opened = esp_timer_get_time(), next_step = opened;
    int step = 0;
    time_t shown = 0;
    bool asleep = false;
    for (;;) {
        check_halt(s);
        if (any_button()) break;
        int64_t now = esp_timer_get_time();
        time_t t = clock_now();
        bool walking = now - opened < CLOCK_WALK_US;
        if (walking ? now >= next_step : (!asleep || t / 60 != shown / 60)) {
            asleep = !walking;
            draw_clock(t, step, asleep);
            shown = t;
            if (walking) {
                step++;
                next_step += CLOCK_STEP_US;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (any_button()) vTaskDelay(pdMS_TO_TICKS(20)); // the button that closed it is not for the VMU

    GblDateTime now;
    time_to_datetime(clock_now(), &now);
    EvmuRom_setDateTime(s->dev->pRom, &now);
    atomic_store(&s_full_request, true);
    ESP_LOGI(TAG, "clock closed");
    boot_log("clock closed");
}

static void emulator_task(void *arg)
{
    session_t *s = arg;
    EvmuDevice *dev = s->dev;
    s->flash_buf = heap_caps_malloc(VMU_FLASH_BYTES, MALLOC_CAP_SPIRAM);
    s->saved_crc = s->fresh || !s->flash_buf ? 0 : flash_crc(dev, s->flash_buf);
    atomic_store(&s_emulating, true);
    int64_t next = esp_timer_get_time();
    int64_t busy_us = 0, last_log = next, last_save_check = s->fresh ? 0 : next, chord_since = 0;
    unsigned frames = 0;
    bool mirrored = false;
    for (;;) {
        check_halt(s);
        int64_t start = esp_timer_get_time();
        bool chord = read_buttons(dev, start);

        // BOOT+PWR: held 3 s goes back to the launcher; on battery a shorter tap opens the clock
        if (chord && !chord_since) chord_since = start;
        if (chord && start - chord_since >= LAUNCHER_HOLD_US) {
            save_if_changed(s);
            ESP_LOGI(TAG, "back to launcher");
            s_open_launcher = OPEN_LAUNCHER_MAGIC;
            esp_restart();
        }
        bool asked = atomic_exchange(&s_clock_request, false);
        if (!chord && chord_since) boot_log("BOOT+PWR tap, battery %d", on_battery());
        if (asked || (!chord && chord_since)) {
            chord_since = 0;
            if (asked || (on_battery() && !mirror_active())) {
                run_clock(s);
                next = esp_timer_get_time();
                continue;
            }
        }

        if (mirror_active()) {
            // Flycast owns the screen: the VMU is "in the controller", so pause it
            mirrored = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            next = esp_timer_get_time();
            continue;
        }
        if (mirrored) { // Flycast stopped: the VMU's own screen comes back on a clean panel
            mirrored = false;
            atomic_store(&s_full_request, true);
        }
        EvmuIBehavior_update(EVMU_IBEHAVIOR(dev), FRAME_NS);
        render_lcd(next_frame(), dev);
        publish_frame();

        int64_t now = esp_timer_get_time();
        busy_us += now - start;
        frames++;
        if (now - last_log >= 10000000) {
            ESP_LOGI(TAG, "emulator: %.1f%% of realtime budget used",
                     100.0 * busy_us / (frames * (FRAME_NS / 1000.0)));
            busy_us = 0;
            frames = 0;
            last_log = now;
            if (on_battery()) boot_log("running on battery");
        }

        // persist the VMU flash (saves, deleted files, minigame data) when it changes
        if (now - last_save_check >= SAVE_CHECK_US) {
            last_save_check = now;
            save_if_changed(s);
        }

        next += FRAME_NS / 1000;
        now = esp_timer_get_time();
        if (next > now) {
            vTaskDelay(pdMS_TO_TICKS((next - now + 999) / 1000));
        } else {
            next = now; // running behind: don't try to catch up
            vTaskDelay(1);
        }
    }
}

/* Creates the VMU for a program: its own image in G/, built from the LIB file on first use. */
static session_t *start_session(const program_t *p)
{
    session_t *s = calloc(1, sizeof(*s));
    s->dev = EvmuDevice_create();
    struct stat st;
    bool have_bios = stat(BIOS_PATH, &st) == 0;
    if (have_bios) {
        EvmuRom_loadBios(s->dev->pRom, BIOS_PATH);

        /* BIOS.BIN is reassembled from the US v1.05 disassembly, so its CRC32 doesn't
         * match the official dump and libevmu classifies it as an unknown BIOS. Run
         * tools/enable_bios_setup_skip.sh to opt into treating it as the real thing,
         * which is what lets skipBiosSetup() below actually take effect. */
        if (stat(BIOS_FORCE_TYPE_PATH, &st) == 0) {
            EvmuRom_setBiosType(s->dev->pRom, EVMU_BIOS_TYPE_AMERICAN_IMAGE_V1_05);
        }

        EvmuRom_skipBiosSetup(s->dev->pRom, GBL_TRUE);
    }

    const char *load = s->image;
    char lib_path[48];
    if (!p->file[0]) {
        snprintf(s->image, sizeof(s->image), "%s", SAVES_PATH);
    } else if (!strncmp(p->file, "FLY:", 4)) {
        snprintf(s->image, sizeof(s->image), LINK_DIR "/%.40s", p->file + 4);
    } else {
        snprintf(s->image, sizeof(s->image), GAMES_DIR "/%.8s.BIN", p->file);
        if (stat(s->image, &st) != 0) {
            snprintf(lib_path, sizeof(lib_path), LIB_DIR "/%.36s", p->file);
            load = lib_path;
            s->fresh = true;
        }
    }
    EVMU_RESULT result = EvmuFileManager_load(s->dev->pFileMgr, load);
    ESP_LOGI(TAG, "%s: BIOS %s, loaded %s (result %d, %u files)", p->title,
             have_bios ? "real" : "emulated", load, (int)result,
             (unsigned)EvmuFileManager_count(s->dev->pFileMgr));

    snprintf(s_active_image, sizeof(s_active_image), "%s", s->image);
    EvmuIBehavior_reset(EVMU_IBEHAVIOR(s->dev));
    GblDateTime now;
    if (rtc_read(&now)) {
        EvmuRom_setDateTime(s->dev->pRom, &now); // after reset, which clears the BIOS clock
        clock_set_base(&now);
    }
    return s;
}

void app_main(void)
{
    board_init();

    const esp_vfs_fat_mount_config_t mount = {.max_files = 4};
    wl_handle_t wl;
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/data", "storage", &mount, &wl));
    mkdir(GAMES_DIR, 0775);
    link_start();
    recover_tmp_files("/data");
    recover_tmp_files(GAMES_DIR);
    recover_tmp_files(LINK_DIR);
    vTaskDelay(pdMS_TO_TICKS(20)); // the USB monitor needs a few ticks to notice no host
    boot_log("boot: reset reason %d, battery %d", (int)esp_reset_reason(), on_battery());
    load_programs();

    ESP_LOGI(TAG, "buttons at boot: BOOT=%d PWR=%d UP=%d DOWN=%d LEFT=%d RIGHT=%d A=%d (0 = pressed)",
             gpio_get_level(PIN_BOOT), gpio_get_level(PIN_PWR_BUTTON), gpio_get_level(PIN_UP),
             gpio_get_level(PIN_DOWN), gpio_get_level(PIN_LEFT), gpio_get_level(PIN_RIGHT),
             gpio_get_level(PIN_A));
    unsigned choice = current_choice();
    bool launcher = s_open_launcher == OPEN_LAUNCHER_MAGIC || access(CURRENT_PATH, F_OK) != 0 ||
                    pressed(PIN_PWR_BUTTON);
    ESP_LOGI(TAG, "current entry %u (%s), launcher %s", choice, s_programs[choice].title,
             launcher ? "yes" : "no");
    s_open_launcher = 0;

    // the first frame the display shows is the launcher, or a blank VMU screen
    memset(s_frames, 0xFF, sizeof(s_frames));
    if (launcher) draw_launcher(choice);
    xTaskCreatePinnedToCore(display_task, "epd", 8 * 1024, NULL, 4, NULL, 0);

    if (launcher) {
        choice = run_launcher(choice);
        FILE *f = fopen(CURRENT_PATH, "w");
        if (f) {
            fprintf(f, "%s\n", s_programs[choice].file);
            fclose(f);
        }
    }

    session_t *s = start_session(&s_programs[choice]);
    xTaskCreatePinnedToCore(emulator_task, "evmu", 32 * 1024, s, 5, NULL, 1);
}
