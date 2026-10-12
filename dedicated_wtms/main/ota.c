#include "ota.h"
#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "fsx.h"
#include "net.h"
#include "wtms_core.h"

static const char *TAG = "ota";
static volatile int s_busy;

static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "update refused: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

esp_err_t ota_http_handler(httpd_req_t *req)
{
    char key[80] = "", tok[48] = "", q[100] = "", sha[70] = "";
    httpd_req_get_hdr_value_str(req, "X-Ota-Key", key, sizeof key);
    httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof tok);
    bool ok = (key[0] && auth_verify_password(key)) || (tok[0] && wtms_token_ok(tok));
    if (!ok) { vTaskDelay(pdMS_TO_TICKS(1200)); return fail(req, "401 Unauthorized", "unauthorized\n"); }
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "sha", sha, sizeof sha) != ESP_OK || strlen(sha) != 64) return fail(req, "400 Bad Request", "sha=<sha256 hex> required\n");
    int total = (int)req->content_len;
    if (total < 100000 || total > 0x160000) return fail(req, "400 Bad Request", "bad image size\n");
    if (s_busy) return fail(req, "409 Conflict", "an update is already running\n");
    s_busy = 1;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) { s_busy = 0; return fail(req, "500 Internal Server Error", "cannot start the update\n"); }
    ESP_LOGW(TAG, "update: %d bytes into %s", total, part->label);
    mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    char *buf = malloc(1024);
    int got = 0; bool bad = !buf;
    while (!bad && got < total) {
        int r = httpd_req_recv(req, buf, total - got < 1024 ? total - got : 1024);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { bad = true; break; }
        mbedtls_sha256_update(&c, (unsigned char *)buf, r);
        if (esp_ota_write(h, buf, r) != ESP_OK) { bad = true; break; }
        got += r;
    }
    free(buf);
    unsigned char dg[32]; mbedtls_sha256_finish(&c, dg); mbedtls_sha256_free(&c);
    char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", dg[i]);
    if (bad || got != total) { esp_ota_abort(h); s_busy = 0; return fail(req, "500 Internal Server Error", "transfer failed\n"); }
    if (strcasecmp(hex, sha)) { esp_ota_abort(h); s_busy = 0; return fail(req, "400 Bad Request", "SHA-256 does not match\n"); }
    if (esp_ota_end(h) != ESP_OK) { s_busy = 0; return fail(req, "400 Bad Request", "image rejected (corrupt or not an app image)\n"); }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { s_busy = 0; return fail(req, "500 Internal Server Error", "cannot select the new image\n"); }
    ESP_LOGW(TAG, "update installed (%d bytes, sha256 verified): rebooting", total);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok: installed and verified, rebooting into the new image (it rolls back by itself if it does not come up healthy)\n");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void healthcheck(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60000));
    for (int i = 0; i < 12; i++) {
        if (net_up() && wtms_cycles() >= 3) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "new image confirmed healthy");
            fs_migrate_legacy();                          // only now: until then a rollback to the previous firmware must find its files
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
    bool pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (!pending) fs_migrate_legacy();
    if (pending) {
        ESP_LOGW(TAG, "running a new image: waiting to confirm it is healthy");
        xTaskCreatePinnedToCore(healthcheck, "otacheck", 8192, NULL, 3, NULL, 0);
    }
}
