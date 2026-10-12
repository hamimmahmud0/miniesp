#include "compat.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fsx.h"
#include "mqttc.h"
#include "net.h"
#include "nvs.h"
#include "sonar.h"
#include "timex.h"

static const char *TAG = "compat";

/* ---------------- output of a call ---------------- */
static __thread char *t_buf; static __thread int t_cap, t_len;
void compat_out_begin(char *buf, int cap) { t_buf = buf; t_cap = cap; t_len = 0; if (buf && cap) buf[0] = 0; }
int compat_out_end(void) { int n = t_len; t_buf = NULL; return n; }
void m_puts(const char *s)
{
    if (!t_buf) return;
    int n = (int)strlen(s);
    if (n > t_cap - 1 - t_len) n = t_cap - 1 - t_len;
    memcpy(t_buf + t_len, s, n); t_len += n; t_buf[t_len] = 0;
}
void m_printf(const char *fmt, ...)
{
    if (!t_buf) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(t_buf + t_len, t_cap - t_len, fmt, ap);
    va_end(ap);
    if (n > 0) { t_len += n; if (t_len > t_cap - 1) t_len = t_cap - 1; }
}

/* ---------------- files ---------------- */
// wtms uses the paths /www/tank/...: they live in /lfs/tank. state.json is kept in RAM only (it changes every cycle: no flash wear).
static char s_json[800];
static SemaphoreHandle_t s_jmu;
#define STATE_PATH "/www/tank/state.json"
#define NFD 6
static struct { FILE *f; int mem, mode, pos; char tmp[800]; int tl; } s_fd[NFD];

static void host_path(const char *p, char *out, int n)
{
    if (!strncmp(p, "/www/tank", 9)) snprintf(out, n, FS_BASE "/tank%s", p + 9);
    else snprintf(out, n, FS_BASE "%s", p);
}
int compat_state_json(char *out, int n)
{
    if (!s_jmu) return 0;
    xSemaphoreTake(s_jmu, portMAX_DELAY);
    int l = (int)strlen(s_json); if (l > n - 1) l = n - 1;
    memcpy(out, s_json, l); out[l] = 0;
    xSemaphoreGive(s_jmu);
    return l;
}
int sys_open(const char *path, int mode)
{
    if (!s_jmu) s_jmu = xSemaphoreCreateMutex();
    int i; for (i = 0; i < NFD && (s_fd[i].f || s_fd[i].mem); i++) {}
    if (i == NFD) return -1;
    memset(&s_fd[i], 0, sizeof s_fd[i]);
    if (!strcmp(path, STATE_PATH)) { s_fd[i].mem = 1; s_fd[i].mode = mode; if (mode == 0) { xSemaphoreTake(s_jmu, portMAX_DELAY); memcpy(s_fd[i].tmp, s_json, sizeof s_json); xSemaphoreGive(s_jmu); s_fd[i].tl = (int)strlen(s_fd[i].tmp); } return i + 3; }
    char h[160]; host_path(path, h, sizeof h);
    FILE *f = fopen(h, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab");
    if (!f) return -1;
    s_fd[i].f = f;
    return i + 3;
}
int sys_read(int fd, void *buf, int n)
{
    int i = fd - 3; if (i < 0 || i >= NFD) return -1;
    if (s_fd[i].mem) { int r = s_fd[i].tl - s_fd[i].pos; if (r > n) r = n; if (r < 0) r = 0; memcpy(buf, s_fd[i].tmp + s_fd[i].pos, r); s_fd[i].pos += r; return r; }
    return s_fd[i].f ? (int)fread(buf, 1, n, s_fd[i].f) : -1;
}
int sys_write(int fd, const void *buf, int n)
{
    int i = fd - 3; if (i < 0 || i >= NFD) return -1;
    if (s_fd[i].mem) { int r = (int)sizeof s_fd[i].tmp - 1 - s_fd[i].tl; if (n < r) r = n; memcpy(s_fd[i].tmp + s_fd[i].tl, buf, r); s_fd[i].tl += r; return r; }
    return s_fd[i].f ? (int)fwrite(buf, 1, n, s_fd[i].f) : -1;
}
int sys_close(int fd)
{
    int i = fd - 3; if (i < 0 || i >= NFD) return -1;
    if (s_fd[i].mem) {
        if (s_fd[i].mode == 1) { xSemaphoreTake(s_jmu, portMAX_DELAY); memcpy(s_json, s_fd[i].tmp, s_fd[i].tl); s_json[s_fd[i].tl] = 0; xSemaphoreGive(s_jmu); }
        s_fd[i].mem = 0;
    } else if (s_fd[i].f) { fclose(s_fd[i].f); s_fd[i].f = NULL; }
    return 0;
}
int sys_fsize(const char *path) { char h[160]; struct stat st; host_path(path, h, sizeof h); return stat(h, &st) ? -1 : (int)st.st_size; }
int sys_stat(const char *path) { char h[160]; struct stat st; host_path(path, h, sizeof h); return stat(h, &st) ? -1 : S_ISDIR(st.st_mode) ? 2 : 1; }
int sys_mkdir(const char *path) { char h[160]; host_path(path, h, sizeof h); return mkdir(h, 0777); }
int sys_rename(const char *a, const char *b) { char x[160], y[160]; host_path(a, x, sizeof x); host_path(b, y, sizeof y); remove(y); return rename(x, y); }

/* ---------------- persistent settings (NVS namespace "kv", shared with the miniesp OS) ---------------- */
#define KV "kv"
int sys_kv_get(const char *key, void *buf, int n)
{
    nvs_handle_t h; size_t l = 0;
    if (nvs_open(KV, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_get_blob(h, key, NULL, &l);
    if (e == ESP_OK && buf && n > 0) { char *tmp = malloc(l + 1); if (tmp) { size_t tl = l; if (nvs_get_blob(h, key, tmp, &tl) == ESP_OK) memcpy(buf, tmp, tl < (size_t)n ? tl : (size_t)n); free(tmp); } }
    nvs_close(h);
    return e == ESP_OK ? (int)l : -1;
}
static int kv_write(const char *key, const void *buf, int n)
{
    nvs_handle_t h;
    if (nvs_open(KV, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, key, buf, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
// The state blob "tank.st" changes every cycle (clock, smoothed level ...): write it at once only when something that matters changed
// (pump state, faults, force mode, ...), otherwise every 2 minutes. Saves thousands of flash writes a day.
static int volatile_line(const char *l)
{
    static const char *const v[] = { "lastrun=", "ema=", "gl=", "lasthist=", "pv=", "pps=", "urate=", "du=", "di=", "dps=", "good=", "jumps=", "lhb=", "lmem=", "lerr=", "lsup=", "dd=", "dr=", "bad=" };
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++) if (!strncmp(l, v[i], strlen(v[i]))) return 1;
    return 0;
}
static int important_differs(const char *a, const char *b)
{
    char ia[800], ib[800]; int na = 0, nb = 0;
    for (const char *p = a; *p;) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) + 1 : (int)strlen(p); if (!volatile_line(p) && na + l < (int)sizeof ia) { memcpy(ia + na, p, l); na += l; } p += l; }
    for (const char *p = b; *p;) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) + 1 : (int)strlen(p); if (!volatile_line(p) && nb + l < (int)sizeof ib) { memcpy(ib + nb, p, l); nb += l; } p += l; }
    return na != nb || memcmp(ia, ib, na);
}
int sys_kv_set(const char *key, const void *buf, int n)
{
    if (!strcmp(key, "tank.st") && n < 790) {
        static char last[800]; static int64_t last_us;
        char cur[800]; memcpy(cur, buf, n); cur[n] = 0;
        int64_t now = esp_timer_get_time();
        if (last_us && !important_differs(cur, last) && now - last_us < 120 * 1000000LL) return 0;
        memcpy(last, cur, n + 1); last_us = now;
    }
    return kv_write(key, buf, n);
}
int sys_kv_del(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(KV, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

/* ---------------- time ---------------- */
unsigned sys_time(void) { return time_now(); }
unsigned sys_millis(void) { return (unsigned)(esp_timer_get_time() / 1000); }
unsigned sys_random(void) { return esp_random(); }
int sys_localtime(unsigned epoch, int *out, int bytes) { return bytes < 32 ? -1 : time_local(epoch, out); }
int sys_tz(const char *spec) { return time_set_tz(spec); }
void sys_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ---------------- MQTT ---------------- */
int sys_mqtt_state(void) { return mqtt_state(); }
int sys_mqtt_pub(const char *t, const void *p, int n, int retain, int qos) { return mqtt_publish(t, p, n, retain, qos); }
int sys_mqtt_sub(const char *t, int qos) { return mqtt_subscribe(t, qos); }
int sys_mqtt_get(const char *t, void *buf, int n) { return mqtt_get(t, buf, n); }
int sys_mqtt_age(const char *t) { return mqtt_age_ms(t); }
int sys_mqtt_config(const char *host, int port, const char *user, const char *pass, const char *cid)
{
    return mqtt_set_config(host, port, user, pass, cid) == 0 ? 0 : -1;       // stores the settings and reconnects
}
int sys_mqtt_info(int what, char *buf, int n)
{
    char host[64] = "", user[48] = "", cid[48] = ""; int port = 1883;
    mqtt_get_config(host, user, cid, sizeof host < sizeof user ? sizeof host : sizeof user, &port);
    switch (what) {
    case 0: snprintf(buf, n, "%s", host); break;
    case 1: snprintf(buf, n, "%s", user); break;
    case 2: snprintf(buf, n, "%s", cid[0] ? cid : net_hostname()); break;
    case 3: snprintf(buf, n, "%d", port); break;
    default: return -1;
    }
    return (int)strlen(buf);
}
int sys_netinfo(int what, char *buf, int n)
{
    if (what == 0) snprintf(buf, n, "%s", net_hostname());
    else if (what == 1) net_ip(buf, n);
    else if (what == 2) snprintf(buf, n, "-");
    else return -1;
    return (int)strlen(buf);
}

/* ---------------- hardware, log, system ---------------- */
int sys_sonar_pulse(int trig, int echo, int timeout_us) { return sonar_pulse(trig, echo, timeout_us); }
int sys_ds18b20(int pin) { return -9999; }                       // no temperature probe support: the speed of sound uses temp_c

int sys_log(const char *m)
{
    if (m[0] == 'E' && m[1] == ' ') ESP_LOGE("wtms", "%s", m + 2);
    else if (m[0] == 'W' && m[1] == ' ') ESP_LOGW("wtms", "%s", m + 2);
    else ESP_LOGI("wtms", "%s", m[0] == 'I' && m[1] == ' ' ? m + 2 : m);
    return 0;
}

int sys_sysinfo(int what)
{
    size_t total = 0, used = 0;
    switch (what) {
    case SI_UPTIME_S: return (int)(esp_timer_get_time() / 1000000);
    case SI_DRAM_FREE: return (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    case SI_DRAM_TOTAL: return (int)heap_caps_get_total_size(MALLOC_CAP_8BIT);
    case SI_DRAM_MINFREE: return (int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    case SI_DRAM_LARGEST: return (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    case SI_FS_USED: case SI_FS_TOTAL: esp_littlefs_info("storage", &total, &used); return (int)(what == SI_FS_USED ? used : total);
    case SI_RSSI: return net_rssi();
    default: return 0;
    }
}
