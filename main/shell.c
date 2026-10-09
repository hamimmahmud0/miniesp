#include "shell.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aot_run.h"
#include "auth.h"
#include "drivers.h"
#include "ota.h"
#include "kv.h"
#include "mqtt.h"
#include "timesync.h"
#include "svc.h"
#include "www.h"
#include "io.h"
#include "creds.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "fs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_mgr.h"
#include "esp_netif.h"
#include "mbedtls/sha256.h"

#define MAXARGS 32
#define MAXSTAGES 8

typedef struct {
    term_t *t;
    char cwd[128];
    io_t *in, *out, *err;   // standard streams of the command being run
    bool quit;
} sh_t;

static char s_cwd[128] = FS_HOME;   // survives across commands within one login

static void sh_vprintf(io_t *io, const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (n > 0) io_write(io, buf, n);
}
static void sh_printf(sh_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sh_printf(sh_t *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sh_vprintf(s->out, fmt, ap); va_end(ap); }
static void sh_eprintf(sh_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sh_eprintf(sh_t *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sh_vprintf(s->err, fmt, ap); va_end(ap); }

static void host_of(sh_t *s, const char *arg, char *host, size_t n)
{
    char v[200];
    fs_resolve(s->cwd, arg, v, sizeof v);
    fs_host_path(v, host, n);
}

typedef int (*cmd_fn)(sh_t *, int, char **);

/* ---------------- built-ins ---------------- */
static int cmd_pwd(sh_t *s, int c, char **v) { sh_printf(s, "%s\n", s->cwd); return 0; }

static int cmd_cd(sh_t *s, int c, char **v)
{
    char vp[200], host[300];
    fs_resolve(s->cwd, c > 1 ? v[1] : FS_HOME, vp, sizeof vp);       // plain "cd" goes home
    fs_host_path(vp, host, sizeof host);
    struct stat st;
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) { sh_eprintf(s, "cd: %s: not a directory\n", c > 1 ? v[1] : "~"); return 1; }
    strlcpy(s->cwd, vp, sizeof s->cwd);
    return 0;
}

typedef struct { char name[72]; bool dir; long size; } ls_ent_t;

static int ls_cmp(const void *a, const void *b) { return strcmp(((const ls_ent_t *)a)->name, ((const ls_ent_t *)b)->name); }

static void ls_print(sh_t *s, const ls_ent_t *e, bool lng)
{
    if (!lng) { sh_printf(s, "%s%s\n", e->name, e->dir ? "/" : ""); return; }
    size_t l = strlen(e->name);
    bool exe = !e->dir && l > 4 && !strcmp(e->name + l - 4, ".aot");
    sh_printf(s, "%s 1 %s %s %8ld %s%s\n", e->dir ? "drwxr-xr-x" : exe ? "-rwxr-xr-x" : "-rw-r--r--", CRED_SSH_USER,
              CRED_SSH_USER, e->size, e->name, e->dir ? "/" : "");
}

static int cmd_ls(sh_t *s, int c, char **v)
{
    bool lng = false, all = false;
    const char *targets[MAXARGS]; int nt = 0;
    for (int i = 1; i < c; i++) {
        if (v[i][0] == '-' && v[i][1]) {
            for (const char *p = v[i] + 1; *p; p++) {
                if (*p == 'l') lng = true;
                else if (*p == 'a' || *p == 'A') all = true;
                else if (*p == '1' || *p == 'h' || *p == 'F') {}                  // accepted, no effect
                else { sh_eprintf(s, "ls: invalid option -- '%c'\nusage: ls [-la] [file...]\n", *p); return 2; }
            }
        } else if (nt < MAXARGS) targets[nt++] = v[i];
    }
    if (!nt) targets[nt++] = ".";
    int rc = 0;
    for (int ti = 0; ti < nt; ti++) {
        char host[300]; struct stat st;
        host_of(s, targets[ti], host, sizeof host);
        if (stat(host, &st)) { sh_eprintf(s, "ls: %s: %s\n", targets[ti], strerror(errno)); rc = 1; continue; }
        if (nt > 1) sh_printf(s, "%s%s:\n", ti ? "\n" : "", targets[ti]);
        if (!S_ISDIR(st.st_mode)) {
            ls_ent_t e = { .dir = false, .size = st.st_size };
            strlcpy(e.name, targets[ti], sizeof e.name);
            ls_print(s, &e, lng);
            continue;
        }
        DIR *d = opendir(host);
        if (!d) { sh_eprintf(s, "ls: %s: %s\n", targets[ti], strerror(errno)); rc = 1; continue; }
        int cap = 32, n = 0;
        ls_ent_t *ents = malloc(cap * sizeof *ents);
        if (!ents) { closedir(d); return 1; }
        if (all) {
            ents[n++] = (ls_ent_t){ .name = ".", .dir = true };
            ents[n++] = (ls_ent_t){ .name = "..", .dir = true };
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!all && e->d_name[0] == '.') continue;
            if (n == cap) {
                ls_ent_t *g = realloc(ents, (cap *= 2) * sizeof *ents);
                if (!g) break;
                ents = g;
            }
            ls_ent_t *x = &ents[n++];
            memset(x, 0, sizeof *x);
            strlcpy(x->name, e->d_name, sizeof x->name);
            x->dir = e->d_type == DT_DIR;
            char p[400]; struct stat es;
            snprintf(p, sizeof p, "%s/%s", host, e->d_name);
            if (!x->dir && !stat(p, &es)) x->size = es.st_size;
        }
        closedir(d);
        qsort(ents, n, sizeof *ents, ls_cmp);
        for (int i = 0; i < n; i++) ls_print(s, &ents[i], lng);
        free(ents);
    }
    return rc;
}

static int cmd_cat(sh_t *s, int c, char **v)
{
    char buf[512];
    if (c < 2) {                                          // no files: copy stdin to stdout
        int n;
        while ((n = io_read(s->in, buf, sizeof buf)) > 0)
            if (io_write(s->out, buf, n)) break;
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < c; i++) {
        char host[300]; host_of(s, v[i], host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (!f) { sh_eprintf(s, "cat: %s: %s\n", v[i], strerror(errno)); rc = 1; continue; }
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            if (io_write(s->out, buf, n)) break;
        fclose(f);
    }
    return rc;
}

static int cmd_echo(sh_t *s, int c, char **v)
{
    int i = 1; bool nl = true;
    if (c > 1 && !strcmp(v[1], "-n")) { nl = false; i = 2; }
    for (; i < c; i++) sh_printf(s, "%s%s", i > (nl ? 1 : 2) ? " " : "", v[i]);
    if (nl) sh_printf(s, "\n");
    return 0;
}

static int cmd_mkdir(sh_t *s, int c, char **v)
{
    bool parents = false; int rc = 0;
    for (int i = 1; i < c; i++) {
        if (!strcmp(v[i], "-p")) { parents = true; continue; }
        char host[300]; host_of(s, v[i], host, sizeof host);
        if (parents) {                                    // create each missing path component
            for (char *p = host + strlen(FS_BASE) + 1; *p; p++)
                if (*p == '/') { *p = 0; mkdir(host, 0777); *p = '/'; }
        }
        if (mkdir(host, 0777) && !(parents && errno == EEXIST)) { sh_eprintf(s, "mkdir: %s: %s\n", v[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

static int rm_rec(const char *host)
{
    struct stat st;
    if (stat(host, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(host);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char p[400]; snprintf(p, sizeof p, "%s/%s", host, e->d_name);
                rm_rec(p);
            }
            closedir(d);
        }
        return rmdir(host);
    }
    return unlink(host);
}

static int cmd_rm(sh_t *s, int c, char **v)
{
    bool rec = false; int rc = 0;
    for (int i = 1; i < c; i++) {
        if (!strcmp(v[i], "-r") || !strcmp(v[i], "-rf")) { rec = true; continue; }
        char host[300]; host_of(s, v[i], host, sizeof host);
        if ((rec ? rm_rec(host) : unlink(host)) && !(rec && errno == ENOENT)) {
            sh_eprintf(s, "rm: %s: %s\n", v[i], strerror(errno)); rc = 1;
        }
    }
    return rc;
}

static int cmd_rmdir(sh_t *s, int c, char **v)
{
    int rc = 0;
    for (int i = 1; i < c; i++) {
        char host[300]; host_of(s, v[i], host, sizeof host);
        if (rmdir(host)) { sh_eprintf(s, "rmdir: %s: %s\n", v[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

static int cmd_mv(sh_t *s, int c, char **v)
{
    if (c != 3) { sh_eprintf(s, "usage: mv src dst\n"); return 1; }
    char a[300], b[300]; host_of(s, v[1], a, sizeof a); host_of(s, v[2], b, sizeof b);
    struct stat st;
    if (!stat(b, &st) && S_ISDIR(st.st_mode)) {         // mv file dir/ -> dir/file
        const char *base = strrchr(a, '/'); base = base ? base + 1 : a;
        size_t l = strlen(b); snprintf(b + l, sizeof b - l, "/%s", base);
    }
    if (rename(a, b)) { sh_eprintf(s, "mv: %s\n", strerror(errno)); return 1; }
    return 0;
}

static int cmd_cp(sh_t *s, int c, char **v)
{
    if (c != 3) { sh_eprintf(s, "usage: cp src dst\n"); return 1; }
    char a[300], b[300]; host_of(s, v[1], a, sizeof a); host_of(s, v[2], b, sizeof b);
    struct stat st;
    if (!stat(b, &st) && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(a, '/'); base = base ? base + 1 : a;
        size_t l = strlen(b); snprintf(b + l, sizeof b - l, "/%s", base);
    }
    FILE *in = fopen(a, "rb");
    if (!in) { sh_eprintf(s, "cp: %s: %s\n", v[1], strerror(errno)); return 1; }
    FILE *out = fopen(b, "wb");
    if (!out) { sh_eprintf(s, "cp: %s: %s\n", v[2], strerror(errno)); fclose(in); return 1; }
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    return 0;
}

// put <path>: store stdin into a file. Use from a host:  ssh host "put ~/.local/bin/x.aot" < x.aot
static int cmd_put(sh_t *s, int c, char **v)
{
    if (c != 2) { sh_eprintf(s, "usage: ssh <host> \"put <path>\" < localfile\n"); return 1; }
    if (s->in->kind == IO_TERM && s->t->pty) {
        sh_eprintf(s, "put: binary upload needs a non-interactive session:\n  ssh <host> \"put %s\" < localfile\n", v[1]);
        return 1;
    }
    char host[300]; host_of(s, v[1], host, sizeof host);
    FILE *f = fopen(host, "wb");
    if (!f) { sh_eprintf(s, "put: %s: %s\n", v[1], strerror(errno)); return 1; }
    unsigned char buf[512]; int r;
    while ((r = io_read(s->in, buf, sizeof buf)) > 0) fwrite(buf, 1, r, f);
    fclose(f);
    // No success message: by now the client has sent EOF and wolfSSH has half-closed the channel.
    return r < 0 ? 1 : 0;
}

static void sha_print(sh_t *s, mbedtls_sha256_context *ctx, const char *name)
{
    unsigned char out[32];
    mbedtls_sha256_finish(ctx, out);
    mbedtls_sha256_free(ctx);
    for (int k = 0; k < 32; k++) sh_printf(s, "%02x", out[k]);
    sh_printf(s, "  %s\n", name);
}

static int cmd_sha256sum(sh_t *s, int c, char **v)
{
    int rc = 0;
    unsigned char buf[512];
    mbedtls_sha256_context ctx;
    if (c < 2) {                                          // stdin
        int n;
        mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
        while ((n = io_read(s->in, buf, sizeof buf)) > 0) mbedtls_sha256_update(&ctx, buf, n);
        sha_print(s, &ctx, "-");
        return 0;
    }
    for (int i = 1; i < c; i++) {
        char host[300]; host_of(s, v[i], host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (!f) { sh_eprintf(s, "sha256sum: %s: %s\n", v[i], strerror(errno)); rc = 1; continue; }
        size_t n;
        mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);
        fclose(f);
        sha_print(s, &ctx, v[i]);
    }
    return rc;
}

static int cmd_df(sh_t *s, int c, char **v)
{
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    sh_printf(s, "Filesystem  Size     Used     Avail   Use%%  Mounted on\n");
    sh_printf(s, "littlefs    %-8u %-8u %-8u %3u%%  /\n", (unsigned)total, (unsigned)used,
              (unsigned)(total - used), total ? (unsigned)(used * 100 / total) : 0);
    return 0;
}

static int cmd_free(sh_t *s, int c, char **v)
{
    sh_printf(s, "              total       free    min-free  largest\n");
    sh_printf(s, "Heap:    %10u %10u %10u %8u\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    sh_printf(s, "IRAM-8:  %10u %10u %10u %8u   (program files)\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_IRAM_8BIT),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_IRAM_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT));
    sh_printf(s, "Exec:    %10u %10u %10u %8u   (IRAM: AOT code)\n", (unsigned)heap_caps_get_total_size(MALLOC_CAP_EXEC),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_EXEC),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC));
    return 0;
}

static int cmd_uptime(sh_t *s, int c, char **v)
{
    long sec = (long)(esp_timer_get_time() / 1000000);
    sh_printf(s, "up %ld days, %02ld:%02ld:%02ld\n", sec / 86400, sec / 3600 % 24, sec / 60 % 60, sec % 60);
    return 0;
}

static int cmd_ps(sh_t *s, int c, char **v)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *a = malloc(n * sizeof *a);
    if (!a) return 1;
    n = uxTaskGetSystemState(a, n, NULL);
    sh_printf(s, "PID  STATE PRIO STACK-FREE NAME\n");
    static const char st[] = "RrBSD?";
    for (UBaseType_t i = 0; i < n; i++)
        sh_printf(s, "%-4u %c     %-4u %-10u %s\n", (unsigned)a[i].xTaskNumber, st[a[i].eCurrentState < 5 ? a[i].eCurrentState : 5],
                  (unsigned)a[i].uxCurrentPriority, (unsigned)a[i].usStackHighWaterMark, a[i].pcTaskName);
    free(a);
    return 0;
}

static int cmd_uname(sh_t *s, int c, char **v)
{
    sh_printf(s, "%s 0.1 ESP-IDF %s Xtensa LX6 (ESP32) WAMR-AOT\n", net_hostname(), esp_get_idf_version());
    return 0;
}
static int cmd_hostname(sh_t *s, int c, char **v)
{
    if (c < 2) { sh_printf(s, "%s\n", net_hostname()); return 0; }
    if (!net_hostname_valid(v[1])) { sh_eprintf(s, "hostname: use 1-31 chars: a-z, 0-9, '-' (no dots, no leading/trailing '-')\n"); return 1; }
    net_cfg_t n; net_cfg_load(&n);
    strlcpy(n.hostname, v[1], sizeof n.hostname);
    if (!net_cfg_save(&n) || !net_apply_hostname(v[1])) { sh_eprintf(s, "hostname: save failed\n"); return 1; }
    sh_eprintf(s, "hostname set; reachable as %s.local (mDNS)\n", v[1]);
    return 0;
}
static int cmd_whoami(sh_t *s, int c, char **v) { sh_printf(s, "%s\n", CRED_SSH_USER); return 0; }
static int cmd_clear(sh_t *s, int c, char **v) { term_puts(s->t, "\x1b[2J\x1b[H"); return 0; }
static int cmd_exit(sh_t *s, int c, char **v) { s->quit = true; return 0; }

static int cmd_sleep(sh_t *s, int c, char **v)
{
    int ms = c > 1 ? (int)(atof(v[1]) * 1000) : 1000;
    while (ms > 0 && !s->t->sigint && !s->t->closed) { vTaskDelay(pdMS_TO_TICKS(50)); ms -= 50; }
    s->t->sigint = false;
    return 0;
}

static int cmd_reboot(sh_t *s, int c, char **v)
{
    sh_printf(s, "rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return 0;
}

static int cmd_ifconfig(sh_t *s, int c, char **v)
{
    if (c >= 2 && !strcmp(v[1], "dhcp")) {
        net_cfg_t n; net_cfg_load(&n);
        n.use_static = false;
        if (!net_cfg_save(&n)) { sh_eprintf(s, "ifconfig: save failed\n"); return 1; }
        sh_printf(s, "address mode: DHCP; run 'reboot' to apply\n");
        return 0;
    }
    if (c >= 2 && !strcmp(v[1], "static")) {
        if (c < 5) { sh_eprintf(s, "usage: ifconfig static <ip> <netmask> <gateway> [dns]\n"); return 1; }
        esp_ip4_addr_t t;
        for (int i = 2; i < c; i++)
            if (esp_netif_str_to_ip4(v[i], &t) != ESP_OK) { sh_eprintf(s, "ifconfig: invalid address '%s'\n", v[i]); return 1; }
        net_cfg_t n; net_cfg_load(&n);
        n.use_static = true;
        strlcpy(n.ip, v[2], sizeof n.ip); strlcpy(n.mask, v[3], sizeof n.mask); strlcpy(n.gw, v[4], sizeof n.gw);
        strlcpy(n.dns, c > 5 ? v[5] : "", sizeof n.dns);
        if (!net_cfg_save(&n)) { sh_eprintf(s, "ifconfig: save failed\n"); return 1; }
        sh_printf(s, "static address saved; run 'reboot' to apply\n");
        return 0;
    }
    char ip[20]; wifi_mgr_ip(ip, sizeof ip);
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(wifi_mgr_state() == WIFI_MODE_AP_ ? WIFI_IF_AP : WIFI_IF_STA, mac);
    const char *m = wifi_mgr_state() == WIFI_MODE_STA_ ? "station" : wifi_mgr_state() == WIFI_MODE_AP_ ? "access point" : "down";
    net_cfg_t n; net_cfg_load(&n);
    sh_printf(s, "wlan0: mode %s  ssid \"%s\"\n       inet %s  ether %02x:%02x:%02x:%02x:%02x:%02x\n", m, wifi_mgr_ssid(),
              ip[0] ? ip : "-", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n.use_static) sh_printf(s, "       configured: static %s netmask %s gw %s dns %s\n", n.ip, n.mask, n.gw, n.dns[0] ? n.dns : "(gateway)");
    else sh_printf(s, "       configured: DHCP\n");
    sh_printf(s, "       name: %s.local (mDNS)\n", net_hostname());
    return 0;
}

static int cmd_wifi(sh_t *s, int c, char **v)
{
    if (c >= 2 && !strcmp(v[1], "status")) return cmd_ifconfig(s, 1, v);
    if (c >= 2 && !strcmp(v[1], "scan")) {
        char *buf = malloc(1024);
        if (!buf) return 1;
        int n = wifi_mgr_scan(buf, 1024);
        sh_printf(s, "%s", n ? buf : "no networks found\n");
        free(buf);
        return 0;
    }
    if (c >= 3 && !strcmp(v[1], "set")) {
        if (!wifi_mgr_save(v[2], c > 3 ? v[3] : "")) { sh_eprintf(s, "wifi: save failed\n"); return 1; }
        sh_printf(s, "saved \"%s\"; run 'reboot' to connect\n", v[2]);
        return 0;
    }
    if (c >= 2 && !strcmp(v[1], "clear")) {
        wifi_mgr_clear(); sh_printf(s, "cleared; built-in defaults used after reboot\n"); return 0;
    }
    sh_eprintf(s, "usage: wifi status | scan | set <ssid> [password] | clear\n");
    return 1;
}

/* ---------------- driver access: gpio adc pwm dac i2c, and web server status ---------------- */
static bool parse_int(const char *s, long *out)
{
    char *end; long v = strtol(s, &end, 0);
    if (end == s || *end) return false;
    *out = v; return true;
}

static int cmd_gpio(sh_t *s, int c, char **v)
{
    long pin, lvl;
    if (c < 2 || !parse_int(v[1], &pin)) { sh_eprintf(s, "usage: gpio <pin> [in | out | 0 | 1]   (no value: read the pin)\n"); return 2; }
    if (c == 2) {
        int r = drv_gpio_read((int)pin);
        if (r < 0) { sh_eprintf(s, "gpio: pin %ld not usable (6-11 are flash)\n", pin); return 1; }
        sh_printf(s, "gpio %ld = %d\n", pin, r);
        return 0;
    }
    int rc;
    if (!strcmp(v[2], "in")) rc = drv_gpio_mode((int)pin, 0);
    else if (!strcmp(v[2], "out")) rc = drv_gpio_mode((int)pin, 1);
    else if (parse_int(v[2], &lvl) && (lvl == 0 || lvl == 1)) rc = drv_gpio_mode((int)pin, 1) < 0 ? -1 : drv_gpio_write((int)pin, (int)lvl);
    else { sh_eprintf(s, "gpio: value must be in, out, 0 or 1\n"); return 2; }
    if (rc < 0) { sh_eprintf(s, "gpio: pin %ld cannot do that (6-11 are flash, 34-39 are input-only)\n", pin); return 1; }
    return 0;
}

static int cmd_adc(sh_t *s, int c, char **v)
{
    long pin;
    if (c != 2 || !parse_int(v[1], &pin)) { sh_eprintf(s, "usage: adc <pin>   (ADC1 pins: 32-39)\n"); return 2; }
    int raw = drv_adc_read((int)pin);
    if (raw < 0) { sh_eprintf(s, "adc: pin %ld is not an ADC1 pin (use 32-39)\n", pin); return 1; }
    sh_printf(s, "adc %ld: raw %d / 4095   about %d mV (rough: the ADC is not linear)\n", pin, raw, raw * 3100 / 4095);
    return 0;
}

static int cmd_pwm(sh_t *s, int c, char **v)
{
    long pin, f, d;
    if (c == 3 && parse_int(v[1], &pin) && !strcmp(v[2], "off")) return drv_pwm((int)pin, 0, -1) < 0 ? 1 : 0;
    if (c != 4 || !parse_int(v[1], &pin) || !parse_int(v[2], &f) || !parse_int(v[3], &d)) {
        sh_eprintf(s, "usage: pwm <pin> <freq Hz 1-40000> <duty %% 0-100>   |   pwm <pin> off\n");
        return 2;
    }
    if (drv_pwm((int)pin, (int)f, (int)(d * 10)) < 0) { sh_eprintf(s, "pwm: cannot (check pin, 1-40000 Hz, duty 0-100, max 8 channels / 4 frequencies)\n"); return 1; }
    sh_printf(s, "pwm on gpio %ld: %ld Hz, %ld%%\n", pin, f, d);
    return 0;
}

static int cmd_dac(sh_t *s, int c, char **v)
{
    long pin, val;
    if (c != 3 || !parse_int(v[1], &pin) || !parse_int(v[2], &val)) { sh_eprintf(s, "usage: dac <25|26> <0-255>\n"); return 2; }
    if (drv_dac_write((int)pin, (int)val) < 0) { sh_eprintf(s, "dac: only GPIO25/26, value 0-255\n"); return 1; }
    sh_printf(s, "dac gpio %ld = %ld (about %ld mV)\n", pin, val, val * 3300 / 255);
    return 0;
}

static int cmd_i2c(sh_t *s, int c, char **v)
{
    if (c >= 2 && !strcmp(v[1], "scan")) {
        long sda = 0, scl = 0, hz = 0;
        if (c >= 4 && (!parse_int(v[2], &sda) || !parse_int(v[3], &scl))) { sh_eprintf(s, "usage: i2c scan [sda scl [hz]]\n"); return 2; }
        if (c >= 5) parse_int(v[4], &hz);
        if (c >= 4 && drv_i2c_init((int)sda, (int)scl, (int)hz) < 0) { sh_eprintf(s, "i2c: cannot start the bus on those pins\n"); return 1; }
        int found = 0;
        sh_printf(s, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
        for (int row = 0; row < 8; row++) {
            sh_printf(s, "%02x:", row * 16);
            for (int col = 0; col < 16; col++) {
                int a = row * 16 + col;
                if (a < 8 || a > 0x77) { sh_printf(s, "   "); continue; }
                if (drv_i2c_probe(a) == 0) { sh_printf(s, " %02x", a); found++; } else sh_printf(s, " --");
            }
            sh_printf(s, "\n");
        }
        sh_printf(s, "%d device(s) found (bus: SDA 21 / SCL 22 unless you gave pins)\n", found);
        return 0;
    }
    long addr, n;
    if (c >= 4 && !strcmp(v[1], "read") && parse_int(v[2], &addr) && parse_int(v[3], &n) && n >= 1 && n <= 64) {
        uint8_t b[64];
        if (drv_i2c_read((int)addr, b, (int)n) < 0) { sh_eprintf(s, "i2c: no answer from 0x%lx\n", addr); return 1; }
        for (long i = 0; i < n; i++) sh_printf(s, "%02x%s", b[i], i + 1 < n ? " " : "\n");
        return 0;
    }
    if (c >= 4 && !strcmp(v[1], "write") && parse_int(v[2], &addr)) {
        uint8_t b[32]; int k = 0;
        for (int i = 3; i < c && k < 32; i++) { long x; if (!parse_int(v[i], &x) || x < 0 || x > 255) { sh_eprintf(s, "i2c: bytes are 0-255\n"); return 2; } b[k++] = (uint8_t)x; }
        if (drv_i2c_write((int)addr, b, k) < 0) { sh_eprintf(s, "i2c: no answer from 0x%lx\n", addr); return 1; }
        return 0;
    }
    sh_eprintf(s, "usage: i2c scan [sda scl [hz]] | i2c read <addr> <n> | i2c write <addr> <byte>...\n");
    return 2;
}

static int cmd_ota(sh_t *s, int c, char **v) { return ota_command(c, v, s->in, s->out, s->err); }

static int cmd_date(sh_t *s, int c, char **v)
{
    uint32_t t = time_now();
    int l[8];
    if (!t || time_local(t, l) < 0) { sh_printf(s, "clock not set (no NTP yet)\n"); return 1; }
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    sh_printf(s, "%s %s %2d %02d:%02d:%02d %d  [%s, tz %s]\n", wd[l[6] % 7], mo[l[4] % 12], l[3], l[2], l[1], l[0], l[5],
              time_state() == 2 ? "ntp synced" : "approximate", time_tz());
    return 0;
}

static int cmd_tz(sh_t *s, int c, char **v)
{
    if (c < 2) { sh_printf(s, "%s\n", time_tz()); return 0; }
    if (!time_set_tz_spec(v[1])) { sh_eprintf(s, "tz: bad zone (use +6, -5:30 or a POSIX TZ string)\n"); return 2; }
    return 0;
}

static int cmd_ntp(sh_t *s, int c, char **v)
{
    char a[64], b[64];
    if (c >= 2) return time_set_ntp(v[1], c > 2 ? v[2] : "") ? 0 : 2;
    time_ntp_servers(a, b, sizeof a);
    sh_printf(s, "servers: %s %s\nstate: %d (0 none, 1 approximate, 2 synced)\n", a, b, time_state());
    return 0;
}

static void kv_print(const char *key, size_t len, void *ctx) { sh_printf((sh_t *)ctx, "%s (%u bytes)\n", key, (unsigned)len); }

static int cmd_kv(sh_t *s, int c, char **v)
{
    char buf[1001];
    if (c >= 2 && !strcmp(v[1], "list")) { kv_list(kv_print, s); return 0; }
    if (c >= 3 && !strcmp(v[1], "get")) {
        int n = kv_get(v[2], buf, sizeof buf - 1);
        if (n < 0) return 1;
        buf[n] = 0; sh_printf(s, "%s\n", buf); return 0;
    }
    if (c >= 4 && !strcmp(v[1], "set")) return kv_set(v[2], v[3], strlen(v[3])) == 0 ? 0 : 1;
    if (c >= 3 && !strcmp(v[1], "del")) return kv_del(v[2]) == 0 ? 0 : 1;
    sh_eprintf(s, "usage: kv list | kv get KEY | kv set KEY VALUE | kv del KEY\n");
    return 2;
}

static void mq_print(const char *t, const char *p, int len, int age, void *ctx) { sh_printf((sh_t *)ctx, "%s = %.*s  (%d s ago)\n", t, len > 80 ? 80 : len, p, age / 1000); }

static int cmd_mqtt(sh_t *s, int c, char **v)
{
    if (c >= 2 && !strcmp(v[1], "config") && c >= 4) {
        return mqtt_set_config(v[2], atoi(v[3]), c > 4 ? v[4] : "", c > 5 ? v[5] : "", c > 6 ? v[6] : "") == 0 ? 0 : 1;
    }
    if (c >= 2 && !strcmp(v[1], "start")) { mqtt_start(); return 0; }
    if (c >= 2 && !strcmp(v[1], "stop")) { mqtt_stop(); return 0; }
    if (c >= 4 && !strcmp(v[1], "pub")) return mqtt_publish(v[2], v[3], strlen(v[3]), c > 4 && !strcmp(v[4], "-r"), 0) >= 0 ? 0 : 1;
    if (c >= 3 && !strcmp(v[1], "sub")) return mqtt_subscribe(v[2], 0) == 0 ? 0 : 1;
    if (c >= 3 && !strcmp(v[1], "unsub")) return mqtt_unsubscribe(v[2]) == 0 ? 0 : 1;
    if (c >= 2 && !strcmp(v[1], "cache")) { mqtt_cache_list(mq_print, s); return 0; }
    if (c >= 2 && !strcmp(v[1], "status")) {
        char h[64], u[32], id[48]; int port;
        mqtt_get_config(h, u, id, sizeof h, &port);
        static const char *st[] = { "off", "connecting", "connected" };
        sh_printf(s, "%s  broker %s:%d user '%s' client '%s'\n", st[mqtt_state() % 3], h, port, u, id);
        return 0;
    }
    sh_eprintf(s, "usage: mqtt status | config HOST PORT [USER PASS [CLIENT_ID]] | start | stop | pub TOPIC MSG [-r] | sub FILTER | unsub FILTER | cache\n");
    return 2;
}

static int cmd_service(sh_t *s, int c, char **v) { return svc_command(c, v, s->out, s->err); }

static int cmd_www(sh_t *s, int c, char **v)
{
    char ip[20]; wifi_mgr_ip(ip, sizeof ip);
    sh_printf(s, "web server: running on port 80, %u request(s) served\n", (unsigned)www_requests());
    sh_printf(s, "  site:  http://%s.local/   or   http://%s/\n", net_hostname(), ip[0] ? ip : "<ip>");
    sh_printf(s, "  files: /www (index.html, assets)     programs: /www/cgi-bin/NAME.aot  ->  /cgi-bin/NAME?arg+arg\n");
    return 0;
}

// One line of secret input. pty: prompt without echo. exec session: a line from stdin.
static int read_secret(sh_t *s, const char *prompt, char *buf, size_t n)
{
    if (s->t->pty) {
        s->t->noecho = true;
        int r = term_readline(s->t, prompt, buf, n, false);
        s->t->noecho = false;
        return r;
    }
    size_t l = 0; char ch; int r = 0;
    while (l < n - 1 && (r = io_read(s->in, &ch, 1)) > 0) {
        if (ch == '\n') break;
        if (ch != '\r') buf[l++] = ch;
    }
    buf[l] = 0;
    return (r <= 0 && l == 0) ? -1 : (int)l;
}

// passwd           change the login password (asks for the current one first)
// passwd --reset   go back to the password built into the firmware
// Non-interactive:  printf 'old\nnew\n' | ssh host passwd
static int cmd_passwd(sh_t *s, int c, char **v)
{
    bool reset = c > 1 && !strcmp(v[1], "--reset");
    if (c > 1 && !reset) { sh_eprintf(s, "usage: passwd [--reset]\n"); return 2; }
    char cur[AUTH_MAX_PW + 2], n1[AUTH_MAX_PW + 2], n2[AUTH_MAX_PW + 2];
    if (read_secret(s, "Current password: ", cur, sizeof cur) < 0) return 1;
    if (!auth_verify_password(cur)) { vTaskDelay(pdMS_TO_TICKS(1500)); sh_eprintf(s, "passwd: wrong password\n"); return 1; }
    if (reset) {
        if (!auth_reset_password()) { sh_eprintf(s, "passwd: could not save\n"); return 1; }
        sh_printf(s, "password reset to the firmware default\n");
        return 0;
    }
    if (read_secret(s, "New password: ", n1, sizeof n1) < 0) return 1;
    if (strlen(n1) < AUTH_MIN_PW || strlen(n1) > AUTH_MAX_PW) {
        sh_eprintf(s, "passwd: password must be %d-%d characters\n", AUTH_MIN_PW, AUTH_MAX_PW);
        return 1;
    }
    if (s->t->pty) {
        if (read_secret(s, "Retype new password: ", n2, sizeof n2) < 0) return 1;
        if (strcmp(n1, n2)) { sh_eprintf(s, "passwd: passwords do not match\n"); return 1; }
    }
    if (!auth_set_password(n1)) { sh_eprintf(s, "passwd: could not save\n"); return 1; }
    sh_printf(s, "password updated\n");
    return 0;
}

static int cmd_help(sh_t *s, int c, char **v);

static const struct { const char *name; cmd_fn fn; const char *help; } CMDS[] = {
    { "help", cmd_help, "list commands" },
    { "ls", cmd_ls, "ls [-la] [path...]" },
    { "cd", cmd_cd, "cd [dir]  (no argument: home, ~)" },
    { "pwd", cmd_pwd, "print working directory" },
    { "cat", cmd_cat, "cat [file...]  (stdin if no file)" },
    { "echo", cmd_echo, "echo [-n] text" },
    { "mkdir", cmd_mkdir, "mkdir [-p] dir..." },
    { "rmdir", cmd_rmdir, "rmdir dir..." },
    { "rm", cmd_rm, "rm [-r] path..." },
    { "mv", cmd_mv, "mv src dst" },
    { "cp", cmd_cp, "cp src dst" },
    { "put", cmd_put, "store stdin to file (ssh host \"put f\" < local)" },
    { "sha256sum", cmd_sha256sum, "sha256sum [file...]" },
    { "df", cmd_df, "filesystem usage" },
    { "free", cmd_free, "memory usage" },
    { "uptime", cmd_uptime, "time since boot" },
    { "ps", cmd_ps, "list tasks" },
    { "uname", cmd_uname, "system info" },
    { "hostname", cmd_hostname, "hostname [name]  (name.local via mDNS)" },
    { "whoami", cmd_whoami, "current user" },
    { "ifconfig", cmd_ifconfig, "ifconfig [dhcp | static ip mask gw [dns]]" },
    { "wifi", cmd_wifi, "wifi status|scan|set|clear" },
    { "sleep", cmd_sleep, "sleep seconds" },
    { "clear", cmd_clear, "clear screen" },
    { "gpio", cmd_gpio, "gpio <pin> [in|out|0|1]" },
    { "adc", cmd_adc, "adc <pin 32-39>" },
    { "pwm", cmd_pwm, "pwm <pin> <Hz> <duty%> | pwm <pin> off" },
    { "dac", cmd_dac, "dac <25|26> <0-255>" },
    { "i2c", cmd_i2c, "i2c scan | read | write" },
    { "www", cmd_www, "web server status and URLs" },
    { "date", cmd_date, "show the time" },
    { "tz", cmd_tz, "tz [+6 | -5:30 | POSIX]" },
    { "ntp", cmd_ntp, "ntp [server1 [server2]]" },
    { "kv", cmd_kv, "kv list|get|set|del  (persistent settings)" },
    { "mqtt", cmd_mqtt, "mqtt status|config|start|stop|pub|sub|unsub|cache" },
    { "ota", cmd_ota, "ota status|confirm|rollback | ota [--sha256 H] < image  (firmware update)" },
    { "service", cmd_service, "service [list|status|start|stop|restart|enable|disable|log|new|rm]" },
    { "systemctl", cmd_service, "alias of service" },
    { "passwd", cmd_passwd, "change the login password" },
    { "reboot", cmd_reboot, "restart the device" },
    { "exit", cmd_exit, "log out" },
    { "logout", cmd_exit, "log out" },
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

static int cmd_help(sh_t *s, int c, char **v)
{
    sh_printf(s, "Built-in commands:\n");
    for (size_t i = 0; i < NCMDS; i++) sh_printf(s, "  %-9s %s\n", CMDS[i].name, CMDS[i].help);
    sh_printf(s, "\nShell syntax: cmd1 | cmd2 | ..., < in, > out, >> append, 2> err, 2>&1\n");
    sh_printf(s, "Programs: *.aot in ~/.local/bin (yours) and /bin (system), run by name. Try: sh  (bash-like shell)\n");
    sh_printf(s, "Home is /esp (~).  Install a program:  ssh host \"put ~/.local/bin/x.aot\" < x.aot\n");
    return 0;
}

/* ---------------- program lookup + dispatch ---------------- */
static bool is_file(const char *vpath)
{
    char host[300]; struct stat st;
    fs_host_path(vpath, host, sizeof host);
    return !stat(host, &st) && S_ISREG(st.st_mode);
}

static bool find_program(sh_t *s, const char *name, char *out, size_t n)
{
    char tmp[200];
    if (strchr(name, '/')) {                           // explicit path
        fs_resolve(s->cwd, name, out, n);
        if (is_file(out)) return true;
        snprintf(tmp, sizeof tmp, "%s.aot", out);
        if (is_file(tmp)) { snprintf(out, n, "%s", tmp); return true; }
        return false;
    }
    static const char *const dirs[] = { FS_HOME "/.local/bin", "/bin" };     // PATH
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(tmp, sizeof tmp, "%s/%s.aot", dirs[i], name);
        if (is_file(tmp)) { snprintf(out, n, "%s", tmp); return true; }
        snprintf(tmp, sizeof tmp, "%s/%s", dirs[i], name);
        if (is_file(tmp)) { snprintf(out, n, "%s", tmp); return true; }
    }
    return false;
}

// Run one command (built-in or program) with the streams currently in `s`.
static int run_argv(sh_t *s, int argc, char **argv)
{
    for (size_t i = 0; i < NCMDS; i++)
        if (!strcmp(argv[0], CMDS[i].name)) return CMDS[i].fn(s, argc, argv);
    char path[200];
    if (find_program(s, argv[0], path, sizeof path)) return aot_run(s->t, s->cwd, path, argc, argv, s->in, s->out, s->err);
    sh_eprintf(s, "%s: command not found\n", argv[0]);
    return 127;
}

// Entry point for sys_run (a program running another command).
int shell_run_argv(term_t *t, const char *cwd, int argc, char **argv, io_t *in, io_t *out, io_t *err)
{
    sh_t s = { .t = t, .in = in, .out = out, .err = err };
    strlcpy(s.cwd, cwd, sizeof s.cwd);
    return run_argv(&s, argc, argv);
}

/* ---------------- command line: words, pipes, redirects ---------------- */
typedef struct { char *w; bool op; } tok_t;

// Split a line into words and operators (| < > >> 2> 2>&1). Quotes group words and stop operators.
// Words are copied into `wbuf` (at least strlen(line)+max bytes) so the input is left untouched.
static int tokenize(const char *line, char *wbuf, tok_t *tk, int max)
{
    int n = 0; const char *p = line; char *o = wbuf;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '|') { p++; tk[n++] = (tok_t){ "|", true }; continue; }
        if (*p == '<') { p++; tk[n++] = (tok_t){ "<", true }; continue; }
        if (*p == '>') {
            if (p[1] == '>') { p += 2; tk[n++] = (tok_t){ ">>", true }; }
            else { p++; tk[n++] = (tok_t){ ">", true }; }
            continue;
        }
        if (p[0] == '2' && p[1] == '>') {
            if (p[2] == '&' && p[3] == '1') { p += 4; tk[n++] = (tok_t){ "2>&1", true }; continue; }
            p += 2; tk[n++] = (tok_t){ "2>", true }; continue;
        }
        char *w = o; char q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t' && *p != '|' && *p != '<' && *p != '>'))) {
            if (q) { if (*p == q) q = 0; else *o++ = *p; }
            else if (*p == '"' || *p == '\'') q = *p;
            else *o++ = *p;
            p++;
        }
        *o++ = 0;
        tk[n++] = (tok_t){ w, false };
    }
    return n;
}

typedef struct {
    int argc;
    char *argv[MAXARGS];
    const char *in_path, *out_path, *err_path;
    bool out_app, err_to_out;
} stage_t;

static int run_line(sh_t *s, char *line)
{
    tok_t tk[MAXARGS * 2];
    char *wbuf = malloc(strlen(line) + MAXARGS * 2 + 2);
    stage_t *st = calloc(MAXSTAGES, sizeof *st);
    if (!wbuf || !st) { free(wbuf); free(st); return 1; }
    int nt = tokenize(line, wbuf, tk, MAXARGS * 2);
    if (!nt) { free(wbuf); free(st); return 0; }
    int ns = 1, rc = 0;
    for (int i = 0; i < nt && rc == 0; i++) {
        stage_t *c = &st[ns - 1];
        if (!tk[i].op) { if (c->argc < MAXARGS - 1) c->argv[c->argc++] = tk[i].w; continue; }
        const char *op = tk[i].w;
        if (!strcmp(op, "|")) {
            if (!c->argc || ns == MAXSTAGES) { sh_eprintf(s, "syntax error near '|'\n"); rc = 2; }
            else ns++;
        } else if (!strcmp(op, "2>&1")) {
            c->err_to_out = true;
        } else {
            if (i + 1 >= nt || tk[i + 1].op) { sh_eprintf(s, "syntax error: missing file after '%s'\n", op); rc = 2; break; }
            const char *f = tk[++i].w;
            if (!strcmp(op, "<")) c->in_path = f;
            else if (!strcmp(op, ">")) { c->out_path = f; c->out_app = false; }
            else if (!strcmp(op, ">>")) { c->out_path = f; c->out_app = true; }
            else if (!strcmp(op, "2>")) c->err_path = f;
        }
    }
    if (rc == 0 && !st[ns - 1].argc) { sh_eprintf(s, "syntax error: missing command\n"); rc = 2; }
    if (rc) { free(wbuf); free(st); return rc; }

    io_t *save_in = s->in, *save_out = s->out, *save_err = s->err;
    io_t *pipe_in = NULL;                              // read side of the previous stage's output
    for (int i = 0; i < ns; i++) {
        stage_t *c = &st[i];
        c->argv[c->argc] = NULL;
        io_t *in = pipe_in ? pipe_in : save_in, *out = save_out, *err = save_err;
        io_t *f_in = NULL, *f_out = NULL, *f_err = NULL, *p_out = NULL;
        char host[300];
        if (c->in_path) {
            host_of(s, c->in_path, host, sizeof host);
            if (!(f_in = io_open_file(host, "rb"))) { sh_eprintf(s, "%s: %s\n", c->in_path, strerror(errno)); rc = 1; break; }
            in = f_in;
        }
        if (i < ns - 1) { p_out = io_new_mem(); if (!p_out) { rc = 1; break; } out = p_out; }
        if (c->out_path) {
            host_of(s, c->out_path, host, sizeof host);
            if (!(f_out = io_open_file(host, c->out_app ? "ab" : "wb"))) { sh_eprintf(s, "%s: %s\n", c->out_path, strerror(errno)); io_close(f_in); io_close(p_out); rc = 1; break; }
            out = f_out;
        }
        if (c->err_to_out) err = out;
        if (c->err_path) {
            host_of(s, c->err_path, host, sizeof host);
            if ((f_err = io_open_file(host, "wb"))) err = f_err;
        }
        s->in = in; s->out = out; s->err = err;
        rc = run_argv(s, c->argc, c->argv);
        s->in = save_in; s->out = save_out; s->err = save_err;
        io_close(f_in); io_close(f_out); io_close(f_err);
        if (pipe_in) { io_close(pipe_in); pipe_in = NULL; }
        if (p_out) { io_rewind(p_out); pipe_in = p_out; }
        if (s->t->closed || s->quit) break;
    }
    io_close(pipe_in);
    s->in = save_in; s->out = save_out; s->err = save_err;
    free(wbuf);
    free(st);
    return rc;
}

int shell_exec(term_t *t, const char *line)
{
    io_t term; io_init_term(&term, t);
    sh_t s = { .t = t, .in = &term, .out = &term, .err = &term };
    strlcpy(s.cwd, FS_HOME, sizeof s.cwd);
    char *copy = strdup(line);
    if (!copy) return 1;
    int rc = run_line(&s, copy);
    free(copy);
    return rc;
}

static void pretty_cwd(const char *cwd, char *out, size_t n)
{
    size_t hl = strlen(FS_HOME);
    if (!strcmp(cwd, FS_HOME)) snprintf(out, n, "~");
    else if (!strncmp(cwd, FS_HOME "/", hl + 1)) snprintf(out, n, "~%s", cwd + hl);
    else snprintf(out, n, "%s", cwd);
}

void shell_run(term_t *t, const char *user)
{
    io_t term; io_init_term(&term, t);
    sh_t s = { .t = t, .in = &term, .out = &term, .err = &term };
    strlcpy(s.cwd, s_cwd, sizeof s.cwd);

    FILE *m = fopen(FS_BASE "/etc/motd", "r");
    if (m) { char b[200]; size_t n; while ((n = fread(b, 1, sizeof b, m)) > 0) term_write(t, b, n); fclose(m); }

    char line[TERM_LINE], prompt[128], pc[100];

    // ~/.profile: one command per line ('#' comments and blank lines skipped), run once per interactive login
    FILE *pf = fopen(FS_BASE FS_HOME "/.profile", "r");
    if (pf) {
        while (!s.quit && !t->closed && fgets(line, sizeof line, pf)) {
            line[strcspn(line, "\r\n")] = 0;
            char *p = line + strspn(line, " \t");
            if (!*p || *p == '#') continue;
            run_line(&s, p);
        }
        fclose(pf);
    }
    while (!s.quit && !t->closed) {
        pretty_cwd(s.cwd, pc, sizeof pc);
        snprintf(prompt, sizeof prompt, "%s@%s:%s$ ", user, net_hostname(), pc);
        int n = term_readline(t, prompt, line, sizeof line, true);
        if (n == -1) break;                 // Ctrl-D / disconnect
        if (n <= 0) continue;
        run_line(&s, line);
    }
    strlcpy(s_cwd, s.cwd, sizeof s_cwd);
    term_puts(t, "logout\n");
}
