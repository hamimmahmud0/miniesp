#include "timesync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "time";
static volatile int s_state;
static char s_tz[40] = "UTC0";

static bool nvs_str_get(const char *key, char *out, size_t n)
{
    nvs_handle_t h; size_t l = n;
    if (nvs_open("time", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, key, out, &l) == ESP_OK;
    nvs_close(h);
    return ok;
}
static void nvs_str_set(const char *key, const char *v)
{
    nvs_handle_t h;
    if (nvs_open("time", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, key, v); nvs_commit(h); nvs_close(h); }
}
static void save_time(void)
{
    nvs_handle_t h; time_t now; time(&now);
    if (s_state && nvs_open("time", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u32(h, "last", (uint32_t)now); nvs_commit(h); nvs_close(h); }
}

static void synced(struct timeval *tv)
{
    s_state = 2;
    ESP_LOGI(TAG, "synced with NTP: %u", (unsigned)tv->tv_sec);
    save_time();
}

static void saver(void *arg)                          // keep the fallback time fresh (about 1 flash write per 30 min)
{
    for (;;) { vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000)); save_time(); }
}

bool time_set_ntp(const char *s1, const char *s2)
{
    if (!s1 || !*s1 || strlen(s1) > 60 || (s2 && strlen(s2) > 60)) return false;
    nvs_str_set("ntp1", s1);
    nvs_str_set("ntp2", s2 && *s2 ? s2 : "");
    esp_sntp_stop();
    static char a[64], b[64];
    strlcpy(a, s1, sizeof a); strlcpy(b, s2 ? s2 : "", sizeof b);
    esp_sntp_setservername(0, a);
    esp_sntp_setservername(1, b[0] ? b : "time.google.com");
    esp_sntp_init();
    return true;
}

void time_ntp_servers(char *s1, char *s2, size_t n)
{
    if (!nvs_str_get("ntp1", s1, n) || !s1[0]) strlcpy(s1, "pool.ntp.org", n);
    if (!nvs_str_get("ntp2", s2, n) || !s2[0]) strlcpy(s2, "time.google.com", n);
}

bool time_set_tz_spec(const char *spec)
{
    char tz[40];
    // "+6" / "-5:30" / "UTC+6" mean hours east/west of UTC; POSIX strings invert the sign, so build one.
    const char *p = spec; if (!strncasecmp(p, "UTC", 3) || !strncasecmp(p, "GMT", 3)) p += 3;
    if ((*p == '+' || *p == '-') && (p[1] >= '0' && p[1] <= '9')) {
        int sign = *p == '+' ? 1 : -1, h = 0, m = 0;
        if (sscanf(p + 1, "%d:%d", &h, &m) < 1 || h > 14 || m > 59) return false;
        char name[12]; snprintf(name, sizeof name, "%c%02d%s%02d", sign > 0 ? '+' : '-', h, m ? ":" : "", m);
        if (m) snprintf(tz, sizeof tz, "<%c%02d%02d>%c%d:%02d", sign > 0 ? '+' : '-', h, m, sign > 0 ? '-' : '+', h, m);
        else snprintf(tz, sizeof tz, "<%s>%c%d", name, sign > 0 ? '-' : '+', h);
        if (h == 0 && m == 0) strcpy(tz, "UTC0");
    } else if (strlen(spec) >= sizeof tz || strlen(spec) < 3) return false;
    else strlcpy(tz, spec, sizeof tz);                 // assume a POSIX TZ string such as "CET-1CEST,M3.5.0,M10.5.0/3"
    strlcpy(s_tz, tz, sizeof s_tz);
    setenv("TZ", s_tz, 1); tzset();
    nvs_str_set("tz", s_tz);
    return true;
}

const char *time_tz(void) { return s_tz; }
int time_state(void) { return s_state; }
uint32_t time_now(void) { if (!s_state) return 0; time_t t; time(&t); return (uint32_t)t; }

int time_local(uint32_t epoch, int out[8])
{
    time_t t = (time_t)epoch; struct tm tm;
    localtime_r(&t, &tm);
    out[0] = tm.tm_sec; out[1] = tm.tm_min; out[2] = tm.tm_hour; out[3] = tm.tm_mday; out[4] = tm.tm_mon;
    out[5] = tm.tm_year + 1900; out[6] = tm.tm_wday; out[7] = tm.tm_yday;
    return 0;
}

void time_init(void)
{
    char tz[40];
    if (nvs_str_get("tz", tz, sizeof tz) && tz[0]) strlcpy(s_tz, tz, sizeof s_tz);
    setenv("TZ", s_tz, 1); tzset();
    nvs_handle_t h; uint32_t last = 0;
    if (nvs_open("time", NVS_READONLY, &h) == ESP_OK) { nvs_get_u32(h, "last", &last); nvs_close(h); }
    if (last > 1700000000u) {                          // a plausible time (after Nov 2023): use it until NTP answers
        struct timeval tv = { .tv_sec = (time_t)last + 5, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_state = 1;
        ESP_LOGW(TAG, "restored saved time (approximate until NTP syncs)");
    }
    char s1[64], s2[64];
    time_ntp_servers(s1, s2, sizeof s1);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    static char a[64], b[64];
    strlcpy(a, s1, sizeof a); strlcpy(b, s2, sizeof b);
    esp_sntp_setservername(0, a);
    esp_sntp_setservername(1, b);
    sntp_set_time_sync_notification_cb(synced);
    sntp_set_sync_interval(3600 * 1000);
    esp_sntp_init();
    xTaskCreatePinnedToCore(saver, "timesave", 2048, NULL, 1, NULL, 0);
}
