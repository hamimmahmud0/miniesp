#include "wd.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs.h"
#include "lwip/stats.h"
#include "nvs.h"
#include "ssh_server.h"
#include "wifi_mgr.h"
#include "www.h"

static const char *TAG = "wd";
#define PERIOD_MS 20000
#define FAILS_TO_ACT 3                       // 3 failed probes in a row (about a minute)
#define STUCK_SESSION_MS 90000               // a session loop that stopped turning for this long
#define MAX_AUTO_REBOOTS 3
static volatile bool s_run;
static TaskHandle_t s_task;

static bool probe(const char *ip, int port)            // can a TCP connection be established? (connect, then close)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_aton(ip, &a.sin_addr);
    bool ok = false;
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r == 0) ok = true;
    else if (errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = { 4, 0 };
        if (select(s + 1, NULL, &w, NULL, &tv) > 0) { int e = 0; socklen_t l = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l); ok = e == 0; }
    }
    close(s);
    return ok;
}

static void dumpf(FILE *f, const char *fmt, ...)
{
    char b[200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    ESP_LOGW(TAG, "%s", b);                             // serial
    if (f && n > 0) { fwrite(b, 1, n, f); fputc('\n', f); }
}

void wd_diagnostics(const char *why);
static void diagnostics(const char *why) { wd_diagnostics(why); }
void wd_diagnostics(const char *why)
{
    char host[100]; snprintf(host, sizeof host, "%s/www/stall.log", FS_BASE);
    FILE *f = fopen(host, "a");
    long sz = f ? (fseek(f, 0, SEEK_END), ftell(f)) : 0;
    if (f && sz > 12000) { fclose(f); f = fopen(host, "w"); }                  // keep it small
    dumpf(f, "==== watchdog: %s (uptime %u s) ====", why, (unsigned)(esp_timer_get_time() / 1000000));
    uint32_t age; const char *wh = ssh_where(&age);
    dumpf(f, "sshd: running=%d session=%d where='%s' (%u ms ago)", ssh_server_running(), ssh_session_active(), wh, (unsigned)age);
    dumpf(f, "heap: free %u, min %u, largest %u | iram8 free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT));
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = malloc(n * sizeof *st);
    if (st) {
        n = uxTaskGetSystemState(st, n, NULL);
        for (UBaseType_t i = 0; i < n; i++)
            dumpf(f, "task %-10s state %d prio %u stack-free %u", st[i].pcTaskName, (int)st[i].eCurrentState, (unsigned)st[i].uxCurrentPriority, (unsigned)st[i].usStackHighWaterMark);
        free(st);
    }
#if LWIP_STATS && MEMP_STATS
    dumpf(f, "lwip tcp pcb: used %u max %u avail %u errors %u", (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used, (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->max,
          (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->avail, (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->err);
    dumpf(f, "lwip tcp listen pcb: used %u max %u errors %u | netconn: used %u max %u", (unsigned)lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used,
          (unsigned)lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->max, (unsigned)lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->err,
          (unsigned)lwip_stats.memp[MEMP_NETCONN]->used, (unsigned)lwip_stats.memp[MEMP_NETCONN]->max);
#endif
    if (f) fclose(f);
}

static int nvs_get_reboots(void) { nvs_handle_t h; uint8_t v = 0; if (nvs_open("wd", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "reboots", &v); nvs_close(h); } return v; }
static void nvs_set_reboots(int v) { nvs_handle_t h; if (nvs_open("wd", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "reboots", (uint8_t)v); nvs_commit(h); nvs_close(h); } }

static void wd_task(void *arg)
{
    int ssh_fails = 0, www_fails = 0, stage = 0;
    int64_t stable_since = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(30000));                   // let the system settle after boot
    while (s_run) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        if (esp_timer_get_time() - stable_since > 600LL * 1000000 && nvs_get_reboots()) nvs_set_reboots(0);   // healthy for 10 min
        char ip[20]; wifi_mgr_ip(ip, sizeof ip);
        if (!ip[0]) continue;

        // 1) a session whose loop stopped turning (blocked inside a call): cut its socket
        uint32_t age; const char *wh = ssh_where(&age);
        if (ssh_session_active() && age > STUCK_SESSION_MS && strcmp(wh, "io-loop")) {
            diagnostics("sshd session stuck");
            ssh_kill_session();
            continue;
        }
        // 2) listener probes (skipped while a session is being served: the task is legitimately busy)
        if (ssh_server_running() && !ssh_session_active()) {
            if (probe(ip, 22)) { ssh_fails = 0; stage = 0; } else ssh_fails++;
        } else ssh_fails = 0;
        if (www_running()) { if (probe(ip, 80)) www_fails = 0; else www_fails++; } else www_fails = 0;

        if (www_fails >= FAILS_TO_ACT) {
            diagnostics("web server not accepting connections: restarting it");
            www_stop(); vTaskDelay(pdMS_TO_TICKS(500)); www_start(); www_fails = 0;
        }
        if (ssh_fails >= FAILS_TO_ACT) {
            if (stage == 0) {
                diagnostics("sshd not accepting connections: restarting the listener");
                ssh_kill_session(); ssh_server_stop(); vTaskDelay(pdMS_TO_TICKS(500)); ssh_server_start();
                stage = 1; ssh_fails = 0;
            } else {
                diagnostics("sshd still not accepting after a listener restart");
                int n = nvs_get_reboots();
                if (n < MAX_AUTO_REBOOTS) { nvs_set_reboots(n + 1); ESP_LOGE(TAG, "rebooting (auto reboot %d of %d)", n + 1, MAX_AUTO_REBOOTS); vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
                ESP_LOGE(TAG, "giving up: %d automatic reboots did not help", n);
                ssh_fails = 0;
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void wd_start(void) { if (s_run) return; s_run = true; if (!s_task) xTaskCreate(wd_task, "wd", 4096, NULL, 3, &s_task); }
void wd_stop(void) { s_run = false; }
int wd_running(void) { return s_run; }
