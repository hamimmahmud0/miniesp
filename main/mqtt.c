#include "mqtt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "wifi_mgr.h"

static const char *TAG = "mqtt";
#define NS "mqtt"
#define MAX_SUBS 12
#define MAX_CACHE 16
#define TOPIC_MAX 64
#define PAYLOAD_MAX 192

static esp_mqtt_client_handle_t s_cli;
static volatile int s_state;
static SemaphoreHandle_t s_mu;
static char s_will_topic[80], s_uri[80], s_user[48], s_pass[64], s_cid[40];
static char s_subs[MAX_SUBS][TOPIC_MAX];
static struct { char topic[TOPIC_MAX]; char payload[PAYLOAD_MAX]; int len; int64_t us; bool used; } s_cache[MAX_CACHE];

static void lock(void) { if (!s_mu) s_mu = xSemaphoreCreateMutex(); xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

/* ---------------- NVS ---------------- */
static bool nget(const char *k, char *out, size_t n)
{
    nvs_handle_t h; size_t l = n;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, k, out, &l) == ESP_OK;
    nvs_close(h);
    return ok;
}
static void nset(const char *k, const char *v) { nvs_handle_t h; if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, k, v); nvs_commit(h); nvs_close(h); } }

static void save_subs(void)
{
    char all[MAX_SUBS * (TOPIC_MAX + 1) + 1]; all[0] = 0;
    for (int i = 0; i < MAX_SUBS; i++) if (s_subs[i][0]) { strlcat(all, s_subs[i], sizeof all); strlcat(all, "\n", sizeof all); }
    nset("subs", all);
}
static void load_subs(void)
{
    char all[MAX_SUBS * (TOPIC_MAX + 1) + 1] = "";
    memset(s_subs, 0, sizeof s_subs);
    if (!nget("subs", all, sizeof all)) return;
    int i = 0;
    for (char *p = strtok(all, "\n"); p && i < MAX_SUBS; p = strtok(NULL, "\n")) strlcpy(s_subs[i++], p, TOPIC_MAX);
}

/* ---------------- events ---------------- */
static void cache_put(const char *topic, const char *data, int len, bool first, int total)
{
    lock();
    int slot = -1;
    for (int i = 0; i < MAX_CACHE; i++) if (s_cache[i].used && !strcmp(s_cache[i].topic, topic)) { slot = i; break; }
    if (slot < 0) {
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < MAX_CACHE; i++) { if (!s_cache[i].used) { slot = i; break; } if (s_cache[i].us < oldest) { oldest = s_cache[i].us; slot = i; } }
        strlcpy(s_cache[slot].topic, topic, TOPIC_MAX); s_cache[slot].used = true; first = true;
    }
    if (first) s_cache[slot].len = 0;
    int room = PAYLOAD_MAX - 1 - s_cache[slot].len;
    int c = len < room ? len : room;
    if (c > 0) { memcpy(s_cache[slot].payload + s_cache[slot].len, data, c); s_cache[slot].len += c; }
    s_cache[slot].payload[s_cache[slot].len] = 0;
    s_cache[slot].us = esp_timer_get_time();
    unlock();
    (void)total;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_state = MQTT_STATE_CONNECTED;
        ESP_LOGI(TAG, "connected to %s", s_uri);
        esp_mqtt_client_publish(s_cli, s_will_topic, "online", 0, 1, 1);
        for (int i = 0; i < MAX_SUBS; i++) if (s_subs[i][0]) esp_mqtt_client_subscribe(s_cli, s_subs[i], 1);
        break;
    case MQTT_EVENT_DISCONNECTED: s_state = MQTT_STATE_CONNECTING; ESP_LOGW(TAG, "disconnected"); break;
    case MQTT_EVENT_DATA:
        if (e->topic_len > 0 && e->topic_len < TOPIC_MAX) {                  // first chunk carries the topic
            static char cur[TOPIC_MAX];
            memcpy(cur, e->topic, e->topic_len); cur[e->topic_len] = 0;
            cache_put(cur, e->data, e->data_len, true, e->total_data_len);
        } else if (e->current_data_offset > 0) {                              // continuation of a long payload: append to the newest entry
            lock();
            int slot = -1; int64_t best = -1;
            for (int i = 0; i < MAX_CACHE; i++) if (s_cache[i].used && s_cache[i].us > best) { best = s_cache[i].us; slot = i; }
            unlock();
            if (slot >= 0) cache_put(s_cache[slot].topic, e->data, e->data_len, false, e->total_data_len);
        }
        break;
    default: break;
    }
}

/* ---------------- control ---------------- */
void mqtt_stop(void)
{
    if (s_cli) { esp_mqtt_client_stop(s_cli); esp_mqtt_client_destroy(s_cli); s_cli = NULL; }
    s_state = MQTT_STATE_OFF;
}

void mqtt_start(void)
{
    char host[64] = "", port[8] = "1883", cid[40] = "";
    if (!nget("host", host, sizeof host) || !host[0]) { ESP_LOGW(TAG, "not configured (mqtt set HOST ...)"); return; }
    nget("port", port, sizeof port); nget("user", s_user, sizeof s_user); nget("pass", s_pass, sizeof s_pass);
    if (!nget("cid", cid, sizeof cid) || !cid[0]) strlcpy(cid, net_hostname(), sizeof cid);
    strlcpy(s_cid, cid, sizeof s_cid);
    snprintf(s_uri, sizeof s_uri, "mqtt://%s:%s", host, port);
    snprintf(s_will_topic, sizeof s_will_topic, "%s/status", cid);
    load_subs();
    mqtt_stop();
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_user[0] ? s_pass : NULL,
        .credentials.client_id = s_cid,
        .session.last_will = { .topic = s_will_topic, .msg = "offline", .qos = 1, .retain = 1 },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 5000,
        .task.stack_size = 4096,
        .buffer.size = 1024, .buffer.out_size = 1024,
    };
    s_cli = esp_mqtt_client_init(&cfg);
    if (!s_cli) { ESP_LOGE(TAG, "client init failed"); return; }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, on_event, NULL);
    s_state = MQTT_STATE_CONNECTING;
    esp_mqtt_client_start(s_cli);
}

int mqtt_running(void) { return s_cli != NULL; }
int mqtt_state(void) { return s_state; }

int mqtt_set_config(const char *host, int port, const char *user, const char *pass, const char *client_id)
{
    if (!host || !host[0] || strlen(host) > 62 || port < 1 || port > 65535) return -1;
    char p[8]; snprintf(p, sizeof p, "%d", port);
    nset("host", host); nset("port", p); nset("user", user ? user : ""); nset("pass", pass ? pass : ""); nset("cid", client_id ? client_id : "");
    mqtt_start();
    return 0;
}

void mqtt_get_config(char *host, char *user, char *client_id, size_t n, int *port)
{
    char p[8] = "1883";
    host[0] = user[0] = client_id[0] = 0;
    nget("host", host, n); nget("user", user, n); nget("cid", client_id, n); nget("port", p, sizeof p);
    if (!client_id[0]) strlcpy(client_id, net_hostname(), n);
    *port = atoi(p);
}

int mqtt_publish(const char *topic, const void *payload, int len, int retain, int qos)
{
    if (!s_cli || s_state != MQTT_STATE_CONNECTED || !topic[0] || len < 0 || len > 1000) return -1;
    return esp_mqtt_client_publish(s_cli, topic, payload, len, qos < 0 ? 0 : qos > 1 ? 1 : qos, retain != 0) >= 0 ? 0 : -1;
}

int mqtt_subscribe(const char *filter, int qos)
{
    if (!filter[0] || strlen(filter) >= TOPIC_MAX) return -1;
    lock();
    int free_slot = -1, have = 0;
    for (int i = 0; i < MAX_SUBS; i++) { if (!strcmp(s_subs[i], filter)) have = 1; else if (!s_subs[i][0] && free_slot < 0) free_slot = i; }
    if (!have) {
        if (free_slot < 0) { unlock(); return -1; }
        strlcpy(s_subs[free_slot], filter, TOPIC_MAX);
        save_subs();
    }
    unlock();
    if (s_cli && s_state == MQTT_STATE_CONNECTED) esp_mqtt_client_subscribe(s_cli, filter, qos ? 1 : 0);
    return 0;
}

int mqtt_unsubscribe(const char *filter)
{
    lock();
    for (int i = 0; i < MAX_SUBS; i++) if (!strcmp(s_subs[i], filter)) { s_subs[i][0] = 0; save_subs(); }
    unlock();
    if (s_cli && s_state == MQTT_STATE_CONNECTED) esp_mqtt_client_unsubscribe(s_cli, filter);
    return 0;
}

int mqtt_get(const char *topic, void *buf, size_t n)
{
    int r = -1;
    lock();
    for (int i = 0; i < MAX_CACHE; i++) if (s_cache[i].used && !strcmp(s_cache[i].topic, topic)) {
        size_t c = (size_t)s_cache[i].len < n ? (size_t)s_cache[i].len : n;
        if (buf && c) memcpy(buf, s_cache[i].payload, c);
        r = s_cache[i].len;
        break;
    }
    unlock();
    return r;
}

int mqtt_age_ms(const char *topic)
{
    int r = -1;
    lock();
    for (int i = 0; i < MAX_CACHE; i++) if (s_cache[i].used && !strcmp(s_cache[i].topic, topic)) { r = (int)((esp_timer_get_time() - s_cache[i].us) / 1000); break; }
    unlock();
    return r;
}

void mqtt_cache_list(mqtt_cache_cb cb, void *ctx)
{
    lock();
    for (int i = 0; i < MAX_CACHE; i++) if (s_cache[i].used) cb(s_cache[i].topic, s_cache[i].payload, s_cache[i].len, (int)((esp_timer_get_time() - s_cache[i].us) / 1000), ctx);
    unlock();
}

int mqtt_sub_list(char *out, size_t n)
{
    out[0] = 0;
    lock();
    for (int i = 0; i < MAX_SUBS; i++) if (s_subs[i][0]) { strlcat(out, s_subs[i], n); strlcat(out, "\n", n); }
    unlock();
    return (int)strlen(out);
}
