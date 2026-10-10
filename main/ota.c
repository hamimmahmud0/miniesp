#include "ota.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "ssh_server.h"
#include "wifi_mgr.h"

static const char *TAG = "ota";

static void say(io_t *o, const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (n > 0) io_write(o, b, n);
}

static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(2500)); esp_restart(); }   // let the SSH reply flush first

static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new (not booted yet)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verification";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted (rolled back)";
    default: return "undefined (factory or first flash)";
    }
}

static void status(io_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition(), *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d; esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    if (esp_ota_get_partition_description(run, &d) == ESP_OK)
        say(out, "running:  %s (0x%x, %u KB)  version %s  built %s %s  ESP-IDF %s\n", run->label, (unsigned)run->address, (unsigned)(run->size / 1024), d.version, d.date, d.time, d.idf_ver);
    say(out, "state:    %s\n", state_name(st));
    if (next) say(out, "next:     %s (0x%x, %u KB) receives the next update\n", next->label, (unsigned)next->address, (unsigned)(next->size / 1024));
}

static bool hex_eq(const unsigned char *h, const char *hex)
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        if ((unsigned)strtoul(b, NULL, 16) != h[i]) return false;
    }
    return true;
}

int ota_command(int argc, char **argv, io_t *in, io_t *out, io_t *err)
{
    if (argc >= 2 && !strcmp(argv[1], "status")) { status(out); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "confirm")) { esp_ota_mark_app_valid_cancel_rollback(); say(out, "running image marked valid\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "rollback")) {
        say(out, "rolling back to the previous image and rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) { say(err, "ota: no previous image to roll back to\n"); return 1; }
        return 0;
    }
    const char *want = NULL; bool reboot = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sha256") && i + 1 < argc) want = argv[++i];
        else if (!strcmp(argv[i], "--no-reboot")) reboot = false;
        else { say(err, "usage: ota status | confirm | rollback | [--sha256 HEX] [--no-reboot]   (firmware image on stdin)\n"); return 2; }
    }
    if (in->kind == IO_TERM && in->t && in->t->pty) { say(err, "ota: send the image on stdin:  ssh esp@host \"ota --sha256 HEX\" < esp32_unix.bin\n"); return 2; }

    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p) { say(err, "ota: no update partition (is this the OTA partition layout?)\n"); return 1; }
    esp_ota_handle_t h;
    ESP_LOGI(TAG, "update to %s: erasing", p->label);
    esp_err_t be = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h);
    ESP_LOGI(TAG, "begin: %s", esp_err_to_name(be));
    if (be != ESP_OK) { say(err, "ota: cannot start the update (%s)\n", esp_err_to_name(be)); return 1; }
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
    uint8_t *buf = malloc(2048);
    if (!buf) { esp_ota_abort(h); say(err, "ota: out of memory\n"); return 1; }
    size_t total = 0; int r; esp_err_t e = ESP_OK;
    while ((r = io_read(in, buf, 2048)) > 0) {
        mbedtls_sha256_update(&sha, buf, r);
        if ((e = esp_ota_write(h, buf, r)) != ESP_OK) break;
        total += r;
        if ((total & 0x1FFFF) < 2048) ESP_LOGI(TAG, "received %u KB", (unsigned)(total / 1024));
    }
    ESP_LOGI(TAG, "stream ended: %u bytes, r=%d, e=%s", (unsigned)total, r, esp_err_to_name(e));
    free(buf);
    unsigned char digest[32]; mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha);
    if (e != ESP_OK || r < 0) { esp_ota_abort(h); say(err, "ota: write failed after %u bytes (%s)\n", (unsigned)total, esp_err_to_name(e)); return 1; }
    if (total < 200 * 1024) { esp_ota_abort(h); say(err, "ota: image too small (%u bytes): refusing\n", (unsigned)total); return 1; }
    if (want && !hex_eq(digest, want)) { esp_ota_abort(h); say(err, "ota: SHA-256 mismatch: the transfer was corrupted, nothing was changed\n"); return 1; }
    if ((e = esp_ota_end(h)) != ESP_OK) { say(err, "ota: the image is not a valid ESP32 firmware (%s)\n", esp_err_to_name(e)); return 1; }
    if (esp_ota_set_boot_partition(p) != ESP_OK) { say(err, "ota: cannot select the new image\n"); return 1; }
    say(out, "ota: %u bytes written to %s, SHA-256 ", (unsigned)total, p->label);
    for (int i = 0; i < 32; i++) say(out, "%02x", digest[i]);
    say(out, "%s\n", want ? " (verified)" : "");
    if (reboot) { say(out, "ota: rebooting into the new image (it rolls back by itself if it does not come up healthy)\n"); xTaskCreatePinnedToCore(reboot_task, "otareboot", 2048, NULL, 5, NULL, 0); }
    else say(out, "ota: new image selected; run 'reboot' to start it\n");
    return 0;
}

// A freshly updated image boots "pending verification". It is confirmed once the system is demonstrably up
// (network up, sshd listening). A crash or watchdog reset before that makes the bootloader roll back.
static void healthcheck(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60000));
    for (int i = 0; i < 12; i++) {
        if (wifi_mgr_state() != WIFI_MODE_NONE_ && ssh_server_running()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "new image confirmed healthy");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    ESP_LOGE(TAG, "image never became healthy: rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    vTaskDelete(NULL);
}

void ota_healthcheck_start(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running a new image: waiting to confirm it is healthy");
        xTaskCreatePinnedToCore(healthcheck, "otacheck", 3072, NULL, 3, NULL, 0);
    }
}
