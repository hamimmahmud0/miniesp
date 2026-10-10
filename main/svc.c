#include "svc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aot_run.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fs.h"
#include "nvs.h"
#include "mqtt.h"
#include "ssh_server.h"
#include "term.h"
#include "wifi_mgr.h"
#include "wd.h"
#include "www.h"

static const char *TAG = "svc";
#define UNIT_DIR "/etc/services"
#define MAX_SVC 8
#define LOG_SIZE 1024
#define LOCK_WAIT_MS 15000

typedef enum { ST_STOPPED, ST_RUNNING, ST_WAITING, ST_FAILED } state_t;

typedef struct {
    char name[24];
    bool native;
    bool in_use;
    // unit file
    char desc[64], exec[160], output[80];
    int interval, restart_policy /* 0 no, 1 always, 2 on-failure */, restart_sec;
    bool enabled;
    // runtime
    state_t state;
    bool want_run;                         // the user (or boot) wants it running
    volatile bool busy;                    // a runner thread is active
    volatile bool started;                 // ... and the program itself is running (holds the runtime)
    uint32_t runs;
    int last_rc;
    int64_t next_us;                       // when the next run is due
    char *log;                             // allocated for unit-file services only (RAM is scarce)
    int log_len;
    bool line_start;
    FILE *outf;
} svc_t;

static svc_t S[MAX_SVC];
static SemaphoreHandle_t s_mu;
static bool s_up;

/* ---------------- native services ---------------- */
typedef struct { const char *name, *desc; int (*running)(void); void (*start)(void); void (*stop)(void); } native_t;
static int mdns_running(void) { return wifi_mgr_mdns_running(); }
static void mdns_start(void) { wifi_mgr_mdns(true); }
static void mdns_stop(void) { wifi_mgr_mdns(false); }
static const native_t NATIVE[] = {
    { "sshd", "SSH server (port 22)", ssh_server_running, ssh_server_start, ssh_server_stop },
    { "www", "web server (HTTP)", www_running, www_start, www_stop },
    { "mdns", "<hostname>.local name responder", mdns_running, mdns_start, mdns_stop },
    { "mqtt", "MQTT client (see: mqtt status)", mqtt_running, mqtt_start, mqtt_stop },
    { "watchdog", "probes sshd/www, self-heals, writes /www/stall.log", wd_running, wd_start, wd_stop },
};
#define NNATIVE ((int)(sizeof NATIVE / sizeof *NATIVE))

static bool nvs_enabled(const char *name)             // native services: enabled unless switched off
{
    nvs_handle_t h; uint8_t v = 1;
    if (nvs_open("svc", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, name, &v); nvs_close(h); }
    return v != 0;
}
static void nvs_set_enabled(const char *name, bool on)
{
    nvs_handle_t h;
    if (nvs_open("svc", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, name, on ? 1 : 0); nvs_commit(h); nvs_close(h); }
}

/* ---------------- log ring + output file ---------------- */
static void log_put(svc_t *s, const char *d, size_t n)
{
    if (!s->log) { s->log = malloc(LOG_SIZE); s->log_len = 0; s->line_start = true; }
    if (!s->log) return;
    for (size_t i = 0; i < n; i++) {
        if (s->line_start) {                               // timestamp each line (uptime seconds; there is no RTC)
            char ts[16]; int l = snprintf(ts, sizeof ts, "[%u] ", (unsigned)(esp_timer_get_time() / 1000000));
            for (int k = 0; k < l; k++) {
                if (s->log_len == LOG_SIZE) { memmove(s->log, s->log + 1, LOG_SIZE - 1); s->log_len--; }
                s->log[s->log_len++] = ts[k];
            }
            if (s->outf) fwrite(ts, 1, l, s->outf);
            s->line_start = false;
        }
        char c = d[i];
        if (s->log_len == LOG_SIZE) { memmove(s->log, s->log + 1, LOG_SIZE - 1); s->log_len--; }   // drop the oldest byte
        s->log[s->log_len++] = c;
        if (s->outf) fputc(c, s->outf);
        if (c == '\n') s->line_start = true;
    }
    if (s->outf) fflush(s->outf);
}
static void log_sink(void *ctx, const void *d, size_t n) { log_put((svc_t *)ctx, d, n); }
static void log_line(svc_t *s, const char *fmt, ...)
{
    char b[160]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    if (!s->line_start) log_put(s, "\n", 1);
    b[n++] = '\n';
    log_put(s, b, n);
}

/* ---------------- unit files ---------------- */
static svc_t *find(const char *name)
{
    for (int i = 0; i < MAX_SVC; i++) if (S[i].in_use && !strcmp(S[i].name, name)) return &S[i];
    return NULL;
}
static svc_t *alloc_slot(const char *name)
{
    svc_t *s = find(name);
    if (s) return s;
    for (int i = 0; i < MAX_SVC; i++) if (!S[i].in_use) { free(S[i].log); memset(&S[i], 0, sizeof S[i]); S[i].in_use = true; strlcpy(S[i].name, name, sizeof S[i].name); S[i].line_start = true; return &S[i]; }
    return NULL;
}
static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (!l || l >= 24) return false;
    for (size_t i = 0; i < l; i++) if (!isalnum((unsigned char)n[i]) && n[i] != '-' && n[i] != '_') return false;
    return true;
}
static void unit_path(const char *name, char *host, size_t n) { snprintf(host, n, "%s%s/%s.service", FS_BASE, UNIT_DIR, name); }

static bool truthy(const char *v) { return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "on"); }

static bool load_unit(const char *name)
{
    char host[200], line[260];
    unit_path(name, host, sizeof host);
    FILE *f = fopen(host, "r");
    if (!f) return false;
    svc_t *s = alloc_slot(name);
    if (!s) { fclose(f); return false; }
    s->native = false; s->desc[0] = s->exec[0] = s->output[0] = 0; s->interval = 0; s->restart_policy = 0; s->restart_sec = 5; s->enabled = true;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '[' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *val = eq + 1;
        for (char *e = val + strlen(val); e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
        if (!strcasecmp(p, "Description")) strlcpy(s->desc, val, sizeof s->desc);
        else if (!strcasecmp(p, "ExecStart")) strlcpy(s->exec, val, sizeof s->exec);
        else if (!strcasecmp(p, "Interval")) s->interval = atoi(val);
        else if (!strcasecmp(p, "RestartSec")) s->restart_sec = atoi(val) > 0 ? atoi(val) : 1;
        else if (!strcasecmp(p, "Restart")) s->restart_policy = !strcasecmp(val, "always") ? 1 : !strcasecmp(val, "on-failure") ? 2 : 0;
        else if (!strcasecmp(p, "Output")) strlcpy(s->output, val, sizeof s->output);
        else if (!strcasecmp(p, "Enabled")) s->enabled = truthy(val);
    }
    fclose(f);
    return s->exec[0] != 0;
}

// Rewrite one key in a unit file (used by enable/disable).
static bool set_unit_key(const char *name, const char *key, const char *value)
{
    char host[200], tmp[210], line[260];
    unit_path(name, host, sizeof host);
    snprintf(tmp, sizeof tmp, "%s.tmp", host);
    FILE *in = fopen(host, "r"), *out = fopen(tmp, "w");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return false; }
    bool done = false;
    while (fgets(line, sizeof line, in)) {
        if (!strncasecmp(line, key, strlen(key)) && line[strlen(key)] == '=') { fprintf(out, "%s=%s\n", key, value); done = true; }
        else fputs(line, out);
    }
    if (!done) fprintf(out, "%s=%s\n", key, value);
    fclose(in); fclose(out);
    remove(host);
    return rename(tmp, host) == 0;
}

static void scan_units(void)
{
    char host[100]; snprintf(host, sizeof host, "%s%s", FS_BASE, UNIT_DIR);
    DIR *d = opendir(host);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 8, ".service")) continue;
        char name[24]; size_t nl = l - 8; if (nl >= sizeof name) continue;
        memcpy(name, e->d_name, nl); name[nl] = 0;
        if (valid_name(name)) load_unit(name);
    }
    closedir(d);
}

/* ---------------- running programs in the background ---------------- */
static bool resolve_program(const char *prog, char *out, size_t n)
{
    char tmp[200]; struct stat st; char host[300];
    const char *cand[4]; int nc = 0; char c0[200], c1[200], c2[200], c3[200];
    if (strchr(prog, '/') || prog[0] == '~') {
        fs_resolve("/", prog, c0, sizeof c0); cand[nc++] = c0;
        snprintf(c1, sizeof c1, "%s.aot", c0); cand[nc++] = c1;
    } else {
        snprintf(c0, sizeof c0, FS_HOME "/.local/bin/%s.aot", prog); cand[nc++] = c0;
        snprintf(c1, sizeof c1, "/bin/%s.aot", prog); cand[nc++] = c1;
        snprintf(c2, sizeof c2, FS_HOME "/.local/bin/%s", prog); cand[nc++] = c2;
        snprintf(c3, sizeof c3, "/bin/%s", prog); cand[nc++] = c3;
    }
    (void)tmp;
    for (int i = 0; i < nc; i++) { fs_host_path(cand[i], host, sizeof host); if (!stat(host, &st) && S_ISREG(st.st_mode)) { strlcpy(out, cand[i], n); return true; } }
    return false;
}

static void *runner(void *arg)
{
    svc_t *s = arg;
    char cmd[160], path[200]; char *argv[12]; int argc = 0;
    strlcpy(cmd, s->exec, sizeof cmd);
    for (char *p = strtok(cmd, " \t"); p && argc < 11; p = strtok(NULL, " \t")) argv[argc++] = p;
    argv[argc] = NULL;
    int rc = 127;
    term_t *t = term_new(false);
    if (argc == 0 || !t) log_line(s, "cannot start: empty command or out of memory");
    else if (!resolve_program(argv[0], path, sizeof path)) log_line(s, "program not found: %s", argv[0]);
    else if (!aot_lock(LOCK_WAIT_MS)) { log_line(s, "runtime busy: skipped this run"); rc = 0; }
    else {
        s->started = true;
        io_t in = { .kind = IO_MEM };
        io_t *lg = io_new_log(log_sink, s);
        if (s->output[0]) {
            char v[100], host[200]; fs_resolve("/", s->output, v, sizeof v); fs_host_path(v, host, sizeof host);
            struct stat st;
            if (!stat(host, &st) && st.st_size > 32 * 1024) remove(host);        // keep the file small
            s->outf = fopen(host, "a");
        }
        rc = aot_run_nolock(t, FS_HOME, path, argc, argv, &in, lg, lg);
        if (s->outf) { fclose(s->outf); s->outf = NULL; }
        io_close(lg);
        s->started = false;
        aot_unlock();
    }
    term_free(t);
    s->last_rc = rc;
    s->runs++;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s->want_run) { s->state = ST_STOPPED; }                                 // stopped while running
    else if (s->interval > 0) { s->state = ST_WAITING; s->next_us = now + (int64_t)s->interval * 1000000; }
    else if (s->restart_policy == 1 || (s->restart_policy == 2 && rc != 0)) { s->state = ST_WAITING; s->next_us = now + (int64_t)s->restart_sec * 1000000; }
    else { s->state = rc == 0 ? ST_STOPPED : ST_FAILED; s->want_run = false; }
    s->busy = false;
    xSemaphoreGive(s_mu);
    return NULL;
}

static void launch(svc_t *s)           // caller holds s_mu
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 0;
    cfg.stack_size = 10240; cfg.thread_name = "svc"; cfg.prio = 4;
    pthread_t th;
    s->busy = true; s->state = ST_RUNNING;
    if (esp_pthread_set_cfg(&cfg) != ESP_OK || pthread_create(&th, NULL, runner, s) != 0) {
        s->busy = false;
        if (s->last_rc != 126) log_line(s, "cannot create a thread (out of memory)%s", s->interval > 0 || s->restart_policy ? ": will retry" : "");
        s->last_rc = 126;                                                           // log the first failure of a streak only
        if (s->interval > 0 || s->restart_policy) { s->state = ST_WAITING; s->next_us = esp_timer_get_time() + 5000000; }   // a periodic service must not stay dead after one OOM
        else { s->state = ST_FAILED; s->want_run = false; }
        return;
    }
    pthread_detach(th);
}

static void svcd(void *arg)            // scheduler: starts due runs
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_mu, portMAX_DELAY);
        for (int i = 0; i < MAX_SVC; i++) {
            svc_t *s = &S[i];
            if (!s->in_use || s->native || !s->want_run || s->busy) continue;
            if (s->state == ST_WAITING && now < s->next_us) continue;
            launch(s);
        }
        xSemaphoreGive(s_mu);
    }
}

/* ---------------- control ---------------- */
static int do_start(svc_t *s)
{
    if (s->native) {
        for (int i = 0; i < NNATIVE; i++) if (!strcmp(NATIVE[i].name, s->name)) { NATIVE[i].start(); return 0; }
        return -1;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s->want_run = true;
    if (!s->busy) { s->state = ST_WAITING; s->next_us = 0; }
    xSemaphoreGive(s_mu);
    return 0;
}
static int do_stop(svc_t *s)
{
    if (s->native) {
        for (int i = 0; i < NNATIVE; i++) if (!strcmp(NATIVE[i].name, s->name)) { NATIVE[i].stop(); return 0; }
        return -1;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s->want_run = false;
    bool kill = s->busy && s->started;
    if (!s->busy) s->state = ST_STOPPED;
    xSemaphoreGive(s_mu);
    if (kill) aot_kill();                                         // only ever the service's own program (it holds the lock)
    return 0;
}

void svc_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    for (int i = 0; i < NNATIVE; i++) {
        svc_t *s = alloc_slot(NATIVE[i].name);
        s->native = true; strlcpy(s->desc, NATIVE[i].desc, sizeof s->desc);
        s->enabled = nvs_enabled(NATIVE[i].name);
        if (s->enabled) NATIVE[i].start();
        else if (NATIVE[i].running()) NATIVE[i].stop();              // e.g. mdns is brought up by the WiFi manager
    }
    scan_units();
    for (int i = 0; i < MAX_SVC; i++) if (S[i].in_use && !S[i].native && S[i].enabled) { S[i].want_run = true; S[i].state = ST_WAITING; S[i].next_us = esp_timer_get_time() + 3000000; }
    xTaskCreatePinnedToCore(svcd, "svcd", 3072, NULL, 4, NULL, 0);
    s_up = true;
    ESP_LOGI(TAG, "service manager up");
}

/* ---------------- the `service` command ---------------- */
static void say(io_t *o, const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (n > 0) io_write(o, b, n);
}

static const char *state_str(svc_t *s)
{
    if (s->native) { for (int i = 0; i < NNATIVE; i++) if (!strcmp(NATIVE[i].name, s->name)) return NATIVE[i].running() ? "running" : "stopped"; }
    switch (s->state) { case ST_RUNNING: return "running"; case ST_WAITING: return s->want_run ? "waiting" : "stopped"; case ST_FAILED: return "failed"; default: return "stopped"; }
}

static void print_list(io_t *o)
{
    say(o, "%-14s %-8s %-9s %-8s %5s %5s  %s\n", "SERVICE", "TYPE", "STATE", "BOOT", "RUNS", "EXIT", "DESCRIPTION");
    for (int i = 0; i < MAX_SVC; i++) {
        svc_t *s = &S[i];
        if (!s->in_use) continue;
        char type[12], runs[12] = "-", ex[12] = "-";
        if (s->native) strcpy(type, "native");
        else { snprintf(type, sizeof type, s->interval > 0 ? "every %ds" : "daemon", s->interval); snprintf(runs, sizeof runs, "%u", (unsigned)s->runs); if (s->runs) snprintf(ex, sizeof ex, "%d", s->last_rc); }
        say(o, "%-14s %-8s %-9s %-8s %5s %5s  %s\n", s->name, type, state_str(s), s->enabled ? "enabled" : "disabled", runs, ex, s->desc);
    }
}

static void print_status(io_t *o, svc_t *s)
{
    say(o, "* %s - %s\n", s->name, s->desc[0] ? s->desc : "(no description)");
    say(o, "   state:   %s\n   boot:    %s\n", state_str(s), s->enabled ? "enabled" : "disabled");
    if (!s->native) {
        say(o, "   exec:    %s\n", s->exec);
        if (s->interval > 0) say(o, "   every:   %d s\n", s->interval);
        else say(o, "   restart: %s (after %d s)\n", s->restart_policy == 1 ? "always" : s->restart_policy == 2 ? "on-failure" : "no", s->restart_sec);
        if (s->output[0]) say(o, "   output:  %s\n", s->output);
        say(o, "   runs:    %u   last exit status: %d\n", (unsigned)s->runs, s->last_rc);
        say(o, "   log (RAM, newest last):\n");
        // last ~8 lines
        if (!s->log) s->log_len = 0;
        const char *p = s->log ? s->log + s->log_len : ""; int lines = 0;
        while (s->log && p > s->log && lines < 9) { p--; if (*p == '\n' && p != s->log + s->log_len - 1) lines++; }
        if (s->log && p > s->log) p++;
        const char *q = p;
        while (s->log && q < s->log + s->log_len) {
            const char *nl = memchr(q, '\n', s->log + s->log_len - q);
            size_t l = nl ? (size_t)(nl - q) : (size_t)(s->log + s->log_len - q);
            say(o, "      %.*s\n", (int)l, q);
            q += l + (nl ? 1 : 0);
            if (!nl) break;
        }
    }
}

int svc_command(int argc, char **argv, io_t *out, io_t *err)
{
    if (!s_up) { say(err, "service: manager is not running\n"); return 1; }
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "list") || !strcmp(sub, "ls")) { print_list(out); return 0; }
    if (!strcmp(sub, "reload")) { scan_units(); say(out, "unit files reloaded\n"); return 0; }
    if (!strcmp(sub, "diag")) { wd_diagnostics("manual request"); say(out, "diagnostics written to the serial log and /www/stall.log (http://<host>/stall.log)\n"); return 0; }

    if (!strcmp(sub, "new")) {          // service new NAME [--every N] [--restart always|on-failure] [--output FILE] [--desc TEXT] -- command args...
        if (argc < 5 || !valid_name(argv[2])) { say(err, "usage: service new NAME [--every SEC] [--restart always|on-failure] [--output FILE] [--desc TEXT] -- COMMAND [ARGS]\n"); return 2; }
        char exec[160] = "", desc[64] = "", output[80] = ""; int every = 0, restart = 0, i = 3;
        for (; i < argc && strcmp(argv[i], "--"); i++) {
            if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--restart") && i + 1 < argc) restart = !strcmp(argv[++i], "always") ? 1 : 2;
            else if (!strcmp(argv[i], "--output") && i + 1 < argc) strlcpy(output, argv[++i], sizeof output);
            else if (!strcmp(argv[i], "--desc") && i + 1 < argc) strlcpy(desc, argv[++i], sizeof desc);
            else { say(err, "service new: unknown option %s\n", argv[i]); return 2; }
        }
        if (i >= argc - 1) { say(err, "service new: give the command after --\n"); return 2; }
        for (i++; i < argc; i++) { if (exec[0]) strlcat(exec, " ", sizeof exec); strlcat(exec, argv[i], sizeof exec); }
        char host[200], dir[100]; snprintf(dir, sizeof dir, "%s%s", FS_BASE, UNIT_DIR); mkdir(dir, 0777);
        unit_path(argv[2], host, sizeof host);
        FILE *f = fopen(host, "w");
        if (!f) { say(err, "service: cannot write %s: %s\n", host, strerror(errno)); return 1; }
        fprintf(f, "[Service]\nDescription=%s\nExecStart=%s\nInterval=%d\nRestart=%s\nRestartSec=5\n", desc[0] ? desc : argv[2], exec, every, restart == 1 ? "always" : restart == 2 ? "on-failure" : "no");
        if (output[0]) fprintf(f, "Output=%s\n", output);
        fprintf(f, "Enabled=false\n");
        fclose(f);
        load_unit(argv[2]);
        say(out, "created %s/%s.service (not enabled). Start it with: service start %s\n", UNIT_DIR, argv[2], argv[2]);
        return 0;
    }

    if (argc < 3) { say(err, "usage: service [list | status|start|stop|restart|enable|disable|log|rm NAME | new NAME ... | reload]\n"); return 2; }
    svc_t *s = find(argv[2]);
    if (!s) { say(err, "service: unknown service '%s' (try: service list)\n", argv[2]); return 1; }

    if (!strcmp(sub, "status")) { print_status(out, s); return 0; }
    if (!strcmp(sub, "log") || !strcmp(sub, "journal")) {
        if (s->native) { say(err, "service: %s is built in and logs to the serial console\n", s->name); return 1; }
        if (s->log && s->log_len) io_write(out, s->log, s->log_len); else say(out, "(no output yet)\n");
        return 0;
    }
    if (!strcmp(sub, "start")) { do_start(s); say(out, "started %s\n", s->name); return 0; }
    if (!strcmp(sub, "stop")) {
        if (s->native && !strcmp(s->name, "sshd")) say(out, "note: sshd stops accepting NEW connections; this session stays open.\n       Start it again with: service start sshd  (or reboot)\n");
        do_stop(s); say(out, "stopped %s\n", s->name); return 0;
    }
    if (!strcmp(sub, "restart")) {
        do_stop(s);
        for (int i = 0; i < 40 && !s->native && s->busy; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (s->native) vTaskDelay(pdMS_TO_TICKS(500));
        do_start(s); say(out, "restarted %s\n", s->name); return 0;
    }
    if (!strcmp(sub, "enable") || !strcmp(sub, "disable")) {
        bool on = !strcmp(sub, "enable");
        s->enabled = on;
        if (s->native) nvs_set_enabled(s->name, on);
        else if (!set_unit_key(s->name, "Enabled", on ? "true" : "false")) { say(err, "service: cannot update the unit file\n"); return 1; }
        say(out, "%s %s at boot\n", s->name, on ? "will start" : "will not start"); return 0;
    }
    if (!strcmp(sub, "rm")) {
        if (s->native) { say(err, "service: built-in services cannot be removed (use disable)\n"); return 1; }
        do_stop(s);
        char host[200]; unit_path(s->name, host, sizeof host); remove(host);
        free(s->log); s->log = NULL;
        s->in_use = false;
        say(out, "removed %s\n", argv[2]); return 0;
    }
    say(err, "service: unknown command '%s'\n", sub);
    return 2;
}
