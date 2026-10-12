#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Native MQTT client with a persistent connection (so short-lived AOT programs can use MQTT).
//   - configuration, enabled flag and subscriptions are stored in NVS and restored at boot
//   - last will "offline" (retained) on <client-id>/status; "online" is published on every (re)connect
//   - the latest message of every subscribed topic is cached, with its arrival time
#define MQTT_STATE_OFF 0
#define MQTT_STATE_CONNECTING 1
#define MQTT_STATE_CONNECTED 2

int mqtt_running(void);
void mqtt_start(void);                                      // (re)start with the stored configuration
void mqtt_stop(void);                                       // disconnect (configuration kept)
int mqtt_set_config(const char *host, int port, const char *user, const char *pass, const char *client_id);
void mqtt_get_config(char *host, char *user, char *client_id, size_t n, int *port);
int mqtt_state(void);
int mqtt_publish(const char *topic, const void *payload, int len, int retain, int qos);
int mqtt_subscribe(const char *filter, int qos);            // persistent; idempotent
int mqtt_unsubscribe(const char *filter);
int mqtt_get(const char *topic, void *buf, size_t n);       // latest payload of a received topic: length or -1
int mqtt_age_ms(const char *topic);                         // ms since it arrived, -1 if never
typedef void (*mqtt_cache_cb)(const char *topic, const char *payload, int len, int age_ms, void *ctx);
void mqtt_cache_list(mqtt_cache_cb cb, void *ctx);
int mqtt_sub_list(char *out, size_t n);                     // newline separated filters
