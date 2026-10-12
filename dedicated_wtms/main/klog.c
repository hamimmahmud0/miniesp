#include "klog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "fsx.h"
#include "mqttc.h"
#include <time.h>
#include "timex.h"

static const char *TAG = "klog";
#define DIR_V  FS_BASE "/log"
#define FILE_A FS_BASE "/log/kernel.log"
#define FILE_B FS_BASE "/log/kernel.1"
#define RING 8192                              // the decision trace writes ~2 KB per control cycle in a burst
#define LINE 288                                // longer lines are cut (the writer never sees more than this)
#define CHUNK 900                               // MQTT payload per message (mqtt_publish limit is 1000)

static char s_ring[RING];
#define LIVE 4096
static char s_live[LIVE];                       // the last 4 KB of log text, for the SSH viewer
static volatile uint32_t s_live_total;          // bytes ever written into it
static int s_head, s_used;
static unsigned s_dropped;
static volatile int s_trace_persist;            // 1: the decision trace is also written to the log files (it is ~3 lines per second: off by default, live view only)
static volatile int s_urgent, s_pending, s_force_rotate;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_fmu;                 // one flush at a time (the writer task and `klog` in the shell)

static int fsize(const char *p) { struct stat st; return stat(p, &st) ? 0 : (int)st.st_size; }

static int cap_bytes(void)
{
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    int avail = (int)total - (int)used + fsize(FILE_A) + fsize(FILE_B) - 96 * 1024;
    int cap = 512 * 1024;
    if (avail < cap) cap = avail;
    return cap < 16384 ? 16384 : cap;
}

// esp_log output hook: print as before (UART) and keep a copy (colour codes removed) in a small ring buffer; a task writes it to the files.
static int klog_vprintf(const char *fmt, va_list ap)
{
    char line[LINE];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof line) { n = sizeof line - 1; line[n - 1] = '\n'; }
    line[n] = 0;
    // wall-clock stamp "[2026-10-12 11:20:59] PM" (local time, 12-hour, AM/PM after the bracket) once the clock is NTP-synced; the ESP prefix "I (12345) tag:" is the uptime in ms
    char stamp[32]; int sl = 0;
    if (time_state() == 2) {
        time_t tt = time(NULL); struct tm tm; localtime_r(&tt, &tm);
        int h12 = tm.tm_hour % 12; if (!h12) h12 = 12;
        sl = snprintf(stamp, sizeof stamp, "[%04d-%02d-%02d %02d:%02d:%02d] %s ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, h12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
    }
    if (sl) fwrite(stamp, 1, sl, stdout);
    fwrite(line, 1, n, stdout);                         // the console (UART)
    int r = n;
    char out[LINE + 32]; int o = 0, urgent = 0;
    for (int i = 0; i < sl; i++) out[o++] = stamp[i];
    for (int i = 0; i < n; i++) {                       // drop ESC [ ... m
        if (line[i] == 27 && i + 1 < n && line[i + 1] == '[') { while (i < n && line[i] != 'm') i++; continue; }
        out[o++] = line[i];
    }
    if (n && (line[0] == 'E' || line[0] == 'W')) urgent = 1;
    int is_trace = 0;                                   // "I (123) trace: ..." (the stamp, if any, comes first)
    for (int i = sl; i < sl + 16 && i + 8 < o; i++) if (out[i] == ')') { is_trace = !memcmp(out + i + 2, "trace:", 6); break; }
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < o; i++) s_live[(s_live_total + i) % LIVE] = out[i];
    s_live_total += o;
    if (is_trace && !s_trace_persist) { portEXIT_CRITICAL(&s_mux); return r; }       // live view only
    if (o <= RING - s_used) {
        int t = (s_head + s_used) % RING;
        for (int i = 0; i < o; i++) { s_ring[t] = out[i]; if (++t == RING) t = 0; }
        s_used += o;
    } else s_dropped++;
    if (urgent) s_urgent = 1;
    portEXIT_CRITICAL(&s_mux);
    if ((urgent || s_used > 1024) && s_task) xTaskNotifyGive(s_task);          // wake the writer for errors and when the buffer fills up
    return r;
}

static int ring_take(char *dst, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_used < max ? s_used : max;
    for (int i = 0; i < n; i++) { dst[i] = s_ring[s_head]; if (++s_head == RING) s_head = 0; }
    s_used -= n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static void rotate(void)
{
    remove(FILE_B);
    rename(FILE_A, FILE_B);
    s_pending = 1;                                      // the finished file goes to the home server
}

static void flush(void)
{
    char buf[256];
    xSemaphoreTake(s_fmu, portMAX_DELAY);
    FILE *f = fopen(FILE_A, "a");
    if (!f) { xSemaphoreGive(s_fmu); return; }                                     // no filesystem: lines stay in the ring (and are dropped when it is full)
    int n;
    while ((n = ring_take(buf, sizeof buf)) > 0) fwrite(buf, 1, n, f);
    fclose(f);
    if (s_force_rotate || fsize(FILE_A) >= cap_bytes() / 2) { s_force_rotate = 0; if (fsize(FILE_A) > 0) rotate(); }
    xSemaphoreGive(s_fmu);
}

static void ship(void)
{
    if (!s_pending || mqtt_state() != MQTT_STATE_CONNECTED) return;
    char host[48], user[32], cid[48]; int port;
    mqtt_get_config(host, user, cid, sizeof cid, &port);
    char topic[80]; snprintf(topic, sizeof topic, "%s/log/kernel", cid[0] ? cid : "miniesp");
    FILE *f = fopen(FILE_B, "r");
    if (!f) { s_pending = 0; return; }
    char buf[CHUNK + 1];
    int total = 0, ok = 1;
    int hn = snprintf(buf, sizeof buf, "--- kernel log (%d bytes) sent after %us uptime, clock %u ---\n", fsize(FILE_B), (unsigned)(esp_timer_get_time() / 1000000), (unsigned)time_now());
    if (mqtt_publish(topic, buf, hn, 0, 0) < 0) ok = 0;
    for (;;) {
        if (!ok) break;
        int n = (int)fread(buf, 1, CHUNK, f);
        if (n <= 0) break;
        if (n == CHUNK) {                               // end on a line boundary
            int k = n; while (k > 0 && buf[k - 1] != '\n') k--;
            if (k > 0) { fseek(f, k - n, SEEK_CUR); n = k; }
        }
        if (mqtt_publish(topic, buf, n, 0, 0) < 0) { ok = 0; break; }
        total += n;
        vTaskDelay(pdMS_TO_TICKS(40));                  // do not flood the broker / the heap
    }
    fclose(f);
    if (ok) { s_pending = 0; ESP_LOGI(TAG, "kernel log (%d bytes) sent to %s", total, topic); }
    else ESP_LOGW(TAG, "kernel log not sent (broker unreachable), will retry");
}

static void klog_task(void *arg)
{
    unsigned last_flush = 0, last_mark = 0, last_try = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        unsigned now = (unsigned)(esp_timer_get_time() / 1000000);
        portENTER_CRITICAL(&s_mux);
        int used = s_used, urgent = s_urgent;
        portEXIT_CRITICAL(&s_mux);
        if (time_state() > 0 && now - last_mark >= 600) {   // wall-clock marker: the lines themselves only carry the uptime
            last_mark = now;
            uint32_t t = time_now();
            ESP_LOGI(TAG, "clock %u (epoch)", (unsigned)t);
            used = 1;
        }
        // few, larger writes: flash wear. Errors and warnings are written within 10 s, the rest when 768 bytes are waiting or after 2 minutes.
        if (s_force_rotate || (used && ((urgent && now - last_flush >= 10) || used >= 768 || now - last_flush >= 120))) {
            portENTER_CRITICAL(&s_mux); s_urgent = 0; portEXIT_CRITICAL(&s_mux);
            flush(); last_flush = now;
        }
        if (s_pending && now - last_try >= 60) { last_try = now; ship(); }
    }
}

void klog_init(void)
{
    s_fmu = xSemaphoreCreateMutex();
    mkdir(DIR_V, 0777);
    if (fsize(FILE_A) >= cap_bytes() / 2) rotate();     // after a reboot with a full file
    esp_log_set_vprintf(klog_vprintf);
    const char *why = "?";
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: why = "power on"; break;       case ESP_RST_SW: why = "software reset"; break;
    case ESP_RST_PANIC: why = "PANIC (crash)"; break;    case ESP_RST_INT_WDT: why = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: why = "task watchdog"; break; case ESP_RST_WDT: why = "other watchdog"; break;
    case ESP_RST_BROWNOUT: why = "BROWNOUT (supply dip)"; break; case ESP_RST_DEEPSLEEP: why = "deep sleep"; break;
    case ESP_RST_EXT: why = "reset pin"; break;          default: break;
    }
    ESP_LOGW(TAG, "---- boot: reset reason: %s ----", why);
    xTaskCreatePinnedToCore(klog_task, "klog", 4096, NULL, 1, &s_task, 0);
}

void klog_send_soon(void) { s_force_rotate = 1; if (s_task) xTaskNotifyGive(s_task); }

void klog_info(int *cap_kb, int *size_cur, int *size_old, int *pending, unsigned *dropped)
{
    *cap_kb = cap_bytes() / 1024; *size_cur = fsize(FILE_A); *size_old = fsize(FILE_B); *pending = s_pending; *dropped = s_dropped;
}

// last `lines` lines of kernel.1 + kernel.log (oldest first); the ring's unwritten part is flushed first
static int count_lines(const char *p)
{
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char b[256]; int n = 0, r;
    while ((r = (int)fread(b, 1, sizeof b, f)) > 0) for (int i = 0; i < r; i++) if (b[i] == '\n') n++;
    fclose(f);
    return n;
}
static int print_from(const char *p, int skip, void (*out)(const char *, int, void *), void *ctx)
{
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char b[256]; int line = 0, printed = 0, r;
    while ((r = (int)fread(b, 1, sizeof b, f)) > 0) {
        int from = 0;
        for (int i = 0; i < r; i++) if (b[i] == '\n') { if (line >= skip) { out(b + from, i + 1 - from, ctx); printed++; } line++; from = i + 1; }
        if (from < r && line >= skip) out(b + from, r - from, ctx);
    }
    fclose(f);
    return printed;
}
int klog_tail(int lines, void (*out)(const char *, int, void *), void *ctx)
{
    portENTER_CRITICAL(&s_mux); int pend = s_used; portEXIT_CRITICAL(&s_mux);
    if (pend) { flush(); }
    int a = count_lines(FILE_B), b = count_lines(FILE_A), skip = a + b - lines;
    if (skip < 0) skip = 0;
    int n = 0;
    if (skip < a) n += print_from(FILE_B, skip, out, ctx);
    n += print_from(FILE_A, skip > a ? skip - a : 0, out, ctx);
    return n;
}

// Live view for the SSH session: copies new log text after *cursor (starts at the end of the current text when *cursor is 0 / too old).
int klog_live_read(uint32_t *cursor, char *dst, int max)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t total = s_live_total, c = *cursor;
    if (total - c > LIVE) c = total - LIVE;                  // fell behind: skip what was overwritten
    int n = (int)(total - c); if (n > max) n = max;
    for (int i = 0; i < n; i++) dst[i] = s_live[(c + i) % LIVE];
    *cursor = c + n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
uint32_t klog_live_end(void) { return s_live_total; }

void klog_flush_now(void) { if (s_fmu) flush(); }

void klog_trace_persist(int on) { s_trace_persist = on != 0; }
