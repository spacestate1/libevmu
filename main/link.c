/* USB serial link for syncing VMU images with a PC (tools/flycast_sync.py).
 *
 * Shares the USB serial port with the console log. Requests and replies are single
 * lines starting with "@@" so log lines can be told apart; file data is sent as
 * base64 "@@D" lines and checked with a CRC32 of the whole file.
 *
 *   @@LIST                 -> @@FILE <name> <size> <crc32> ... @@END
 *   @@GET <name>           -> @@SIZE <size> <crc32>, @@D <base64>..., @@END
 *   @@PUT <name> <size> <crc32>, then @@D <base64> lines, each answered with @@A <bytes>
 *                          -> @@OK | @@ERR <reason>
 *                          Replacing the running VMU's image stops it, and the board
 *                          restarts into the new image once the PC sends nothing for 1 s.
 *   @@RUN <name>           -> @@OK, then restarts into FLY/<name>
 *   @@MENU                 -> @@OK, then restarts into the launcher
 *   @@KEY <buttons> <ms>   -> @@OK   hold VMU buttons (letters udlrabms) for ms
 *   @@CLOCK                -> @@OK   open the clock screen (any button or @@KEY closes it)
 *   @@LOG                  -> @@T <line>... @@END   /data/LOG.OLD then /data/LOG.TXT
 *   @@SHOT                 -> @@D <base64>... @@END   the frame on the panel (5000 bytes)
 *   @@L <base64>           live VMU screen (192 bytes, no reply); see link_mirror_frame
 * Names are plain file names inside /data/FLY. */
#include "link.h"
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link";

#define LINE_MAX_LEN 512
#define CHUNK_BYTES 288 // 384 base64 characters per line
#define MAX_FILE_BYTES (128 * 1024)

// FLY image to restart into once the PC goes quiet (after replacing the running one)
static char s_restart_into[48];

static const char s_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void send_line(const char *fmt, ...)
{
    char line[LINE_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    usb_serial_jtag_write_bytes(line, n, portMAX_DELAY);
}

static size_t b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = s_b64[v >> 18 & 63];
        out[o++] = s_b64[v >> 12 & 63];
        out[o++] = i + 1 < len ? s_b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? s_b64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Returns decoded length, or -1 on a bad character. */
static int b64_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in && *in != '='; in++) {
        const char *p = strchr(s_b64, *in);
        if (!p || !*in) return -1;
        v = v << 6 | (uint32_t)(p - s_b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return (int)o;
}

static bool read_line(char *line, size_t cap, TickType_t timeout)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, timeout) != 1) return false;
        if (c == '\n') break;
        if (c != '\r' && n + 1 < cap) line[n++] = c;
    }
    line[n] = 0;
    return true;
}

static bool valid_name(const char *name)
{
    if (!*name || strlen(name) > 40) return false;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("-_.", *p)) return false;
    }
    return name[0] != '.';
}

static void cmd_list(void)
{
    DIR *dir = opendir(LINK_DIR);
    struct dirent *e;
    uint8_t *buf = heap_caps_malloc(MAX_FILE_BYTES, MALLOC_CAP_SPIRAM);
    while (dir && buf && (e = readdir(dir))) {
        char path[80];
        snprintf(path, sizeof(path), LINK_DIR "/%.60s", e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t size = fread(buf, 1, MAX_FILE_BYTES, f);
        fclose(f);
        send_line("@@FILE %s %u %08lx", e->d_name, (unsigned)size,
                  (unsigned long)esp_rom_crc32_le(0, buf, size));
    }
    if (dir) closedir(dir);
    free(buf);
    send_line("@@END");
}

static void cmd_get(const char *name)
{
    char path[80];
    snprintf(path, sizeof(path), LINK_DIR "/%s", name);
    uint8_t *buf = heap_caps_malloc(MAX_FILE_BYTES, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    if (!f) {
        send_line("@@ERR cannot open %s", name);
        free(buf);
        return;
    }
    size_t size = fread(buf, 1, MAX_FILE_BYTES, f);
    fclose(f);
    send_line("@@SIZE %u %08lx", (unsigned)size, (unsigned long)esp_rom_crc32_le(0, buf, size));
    char enc[CHUNK_BYTES / 3 * 4 + 1];
    for (size_t off = 0; off < size; off += CHUNK_BYTES) {
        size_t n = size - off < CHUNK_BYTES ? size - off : CHUNK_BYTES;
        b64_encode(buf + off, n, enc);
        send_line("@@D %s", enc);
    }
    send_line("@@END");
    free(buf);
}

static void cmd_put(const char *name, unsigned size, unsigned long crc)
{
    if (size > MAX_FILE_BYTES) {
        send_line("@@ERR too large");
        return;
    }
    uint8_t *buf = heap_caps_malloc(size ? size : 1, MALLOC_CAP_SPIRAM);
    char *line = malloc(LINE_MAX_LEN);
    size_t got = 0;
    while (buf && line && got < size) {
        if (!read_line(line, LINE_MAX_LEN, pdMS_TO_TICKS(5000))) break;
        if (strncmp(line, "@@D ", 4)) continue;
        int n = b64_decode(line + 4, buf + got, size - got);
        if (n < 0) break;
        got += n;
        send_line("@@A %u", (unsigned)got); // flow control: one line in flight
    }
    free(line);
    if (!buf || got != size || esp_rom_crc32_le(0, buf, size) != crc) {
        send_line("@@ERR transfer failed (%u/%u bytes)", (unsigned)got, size);
        free(buf);
        return;
    }
    char path[80], tmp[84];
    snprintf(path, sizeof(path), LINK_DIR "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.TMP", path);
    // the running VMU would overwrite the new image with its old flash: stop it first
    bool active = link_is_active_image(path);
    if (active) {
        link_halt_emulator(false);
        snprintf(s_restart_into, sizeof(s_restart_into), "%s", name);
    }
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, size, f) == size;
    if (f) ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    free(buf);
    send_line(ok ? "@@OK" : "@@ERR write failed");
    ESP_LOGI(TAG, "received %s (%u bytes): %s%s", name, size, ok ? "ok" : "FAILED",
             active ? "; restarting into it when the PC is done" : "");
}

static void link_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX_LEN];
    for (;;) {
        // while a restart is pending, wait for the PC to finish (a push sends INDEX.TXT last)
        bool pending = s_restart_into[0];
        if (!read_line(line, sizeof(line), pending ? pdMS_TO_TICKS(1000) : portMAX_DELAY)) {
            if (pending) link_restart(s_restart_into);
            continue;
        }
        if (strncmp(line, "@@", 2)) continue;
        if (!strncmp(line, "@@L ", 4)) {
            uint8_t bits[192];
            if (b64_decode(line + 4, bits, sizeof(bits)) == sizeof(bits)) link_mirror_frame(bits);
            continue;
        }
        char cmd[8] = {0}, name[48] = {0};
        unsigned size = 0;
        unsigned long crc = 0;
        int fields = sscanf(line + 2, "%7s %47s %u %lx", cmd, name, &size, &crc);
        bool named = fields >= 2 && valid_name(name);
        if (!strcmp(cmd, "LIST")) {
            cmd_list();
        } else if (!strcmp(cmd, "GET") && named) {
            cmd_get(name);
        } else if (!strcmp(cmd, "PUT") && named && fields == 4) {
            cmd_put(name, size, crc);
        } else if (!strcmp(cmd, "RUN") && named) {
            send_line("@@OK");
            vTaskDelay(pdMS_TO_TICKS(100));
            link_restart(name);
        } else if (!strcmp(cmd, "MENU")) {
            send_line("@@OK");
            vTaskDelay(pdMS_TO_TICKS(100));
            link_restart(NULL);
        } else if (!strcmp(cmd, "KEY") && fields == 3) {
            link_key(name, size);
            send_line("@@OK");
        } else if (!strcmp(cmd, "LOG")) {
            static const char *const logs[] = {"/data/LOG.OLD", "/data/LOG.TXT"};
            for (int i = 0; i < 2; i++) {
                FILE *f = fopen(logs[i], "r");
                char text[160];
                while (f && fgets(text, sizeof(text), f)) {
                    text[strcspn(text, "\r\n")] = 0;
                    send_line("@@T %s", text);
                }
                if (f) fclose(f);
            }
            send_line("@@END");
        } else if (!strcmp(cmd, "CLOCK")) {
            link_clock();
            send_line("@@OK");
        } else if (!strcmp(cmd, "SHOT")) {
            const uint8_t *frame = link_screen();
            char enc[CHUNK_BYTES / 3 * 4 + 1];
            for (size_t off = 0; off < 5000; off += CHUNK_BYTES) {
                b64_encode(frame + off, 5000 - off < CHUNK_BYTES ? 5000 - off : CHUNK_BYTES, enc);
                send_line("@@D %s", enc);
            }
            send_line("@@END");
        } else {
            send_line("@@ERR bad command");
        }
    }
}

void link_start(void)
{
    mkdir(LINK_DIR, 0775);
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;
    cfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver(); // console log goes through the same driver
    xTaskCreatePinnedToCore(link_task, "link", 6 * 1024, NULL, 3, NULL, 0);
}
