#include "kv.h"
#include <ctype.h>
#include <string.h>
#include "nvs.h"

#define NS "kv"
static int key_ok(const char *k)
{
    size_t l = strlen(k);
    if (l < 1 || l > 15) return 0;
    for (size_t i = 0; i < l; i++) if (!isalnum((unsigned char)k[i]) && k[i] != '_' && k[i] != '.' && k[i] != '-') return 0;
    return 1;
}
int kv_get(const char *key, void *buf, size_t n)
{
    nvs_handle_t h; size_t l = 0;
    if (!key_ok(key) || nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_get_blob(h, key, NULL, &l);
    if (e == ESP_OK && buf && n) { size_t c = l < n ? l : n; uint8_t tmp[1008]; size_t tl = sizeof tmp; if (nvs_get_blob(h, key, tmp, &tl) == ESP_OK) { memcpy(buf, tmp, c < tl ? c : tl); } }
    nvs_close(h);
    return e == ESP_OK ? (int)l : -1;
}
int kv_set(const char *key, const void *buf, size_t n)
{
    nvs_handle_t h;
    if (!key_ok(key) || n > 1000 || nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, key, buf, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
int kv_del(const char *key)
{
    nvs_handle_t h;
    if (!key_ok(key) || nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
void kv_list(kv_iter_cb cb, void *ctx)
{
    nvs_iterator_t it = NULL;
    if (nvs_entry_find("nvs", NS, NVS_TYPE_BLOB, &it) != ESP_OK) return;
    while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t l = 0; nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_blob(h, info.key, NULL, &l); nvs_close(h); }
        cb(info.key, l, ctx);
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);
}
