#include "httpget.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "httpget";

typedef struct {
    FILE *f;
    int max, got, err;                       // err: 0 ok, else the negative return code
    bool (*cancel)(void *); void *arg;
} dl_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    dl_t *d = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || d->err) return ESP_OK;
    if (esp_http_client_get_status_code(e->client) != 200) return ESP_OK;     // redirect or error body: ignore
    if (d->cancel && d->cancel(d->arg)) d->err = -4;
    else if (d->got + e->data_len > d->max) d->err = -3;
    else if (fwrite(e->data, 1, e->data_len, d->f) != (size_t)e->data_len) d->err = -2;
    else { d->got += e->data_len; return ESP_OK; }
    esp_http_client_cancel_request(e->client);
    return ESP_OK;
}

int http_download(const char *url, const char *host_path, int max_bytes, int timeout_ms,
                  bool (*cancel)(void *), void *arg)
{
    char part[300];
    snprintf(part, sizeof part, "%s.part", host_path);
    dl_t d = { .max = max_bytes > 0 ? max_bytes : 0x7fffffff, .cancel = cancel, .arg = arg };
    d.f = fopen(part, "wb");
    if (!d.f) return -2;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 10000,
        .event_handler = on_event,
        .user_data = &d,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    int rc;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { fclose(d.f); remove(part); return -1; }
    esp_err_t e = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (fclose(d.f)) d.err = d.err ? d.err : -2;

    if (d.err) rc = d.err;
    else if (e != ESP_OK) { ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(e)); rc = -1; }
    else if (status != 200) rc = -status;
    else rc = d.got;
    if (rc < 0) { remove(part); return rc; }
    remove(host_path);                                // LittleFS rename needs the target gone
    if (rename(part, host_path)) { remove(part); return -2; }
    return rc;
}
