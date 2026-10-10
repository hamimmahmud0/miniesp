#include "aot_run.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bh_platform.h"
#include "drivers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs.h"
#include "shell.h"
#include "rom/ets_sys.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "tty_vfs.h"
#include "wifi_mgr.h"
#include "wasm_export.h"
#include <dirent.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "esp_random.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "httpget.h"
#include "bench.h"
#include "kv.h"
#include "mqtt.h"
#include "timesync.h"

static const char *TAG = "aot";
static volatile wasm_module_inst_t s_inst;       // innermost running program (target of Ctrl-C)

#define PROG_STACK 2048
#define PROG_HEAP  0
#define MAX_FDS 8

/* One of these per running program; nested programs (sys_run) form a chain. */
typedef struct proc {
    struct proc *parent;
    TaskHandle_t owner;                          // the task running this program
    term_t *t;
    char cwd[128];
    io_t *in, *out, *err;                        // standard streams (borrowed)
    int argc; char **argv;                       // this program's arguments (for sys_argc/sys_arg)
    bool catch_int;                              // shell mode: Ctrl-C is reported via sys_sigint instead of killing us
    io_t *fds[MAX_FDS];                          // user-opened streams (owned), fds 3..3+MAX_FDS-1
} proc_t;
static proc_t *P;
static volatile bool s_interactive;                 // the running top-level program is attached to a terminal (a shell, btop, ...)
static SemaphoreHandle_t s_run_lock;                // one top-level program at a time (SSH session or web request)

/* ---------------- syscall layer exposed to programs (module "env") ---------------- */
static bool interrupted(wasm_exec_env_t env)
{
    if (P && P->catch_int) return false;                 // the program handles Ctrl-C itself
    if (P && P->t && P->t->sigint) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "interrupted");
        return true;
    }
    return false;
}

static io_t *get_io(int fd)
{
    if (!P) return NULL;
    if (fd == 0) return P->in;
    if (fd == 1) return P->out;
    if (fd == 2) return P->err;
    return (fd >= 3 && fd < 3 + MAX_FDS) ? P->fds[fd - 3] : NULL;
}

static int add_fd(io_t *io)
{
    if (!io) return -1;
    for (int i = 0; i < MAX_FDS; i++)
        if (!P->fds[i]) { P->fds[i] = io; return 3 + i; }
    io_close(io);
    return -1;
}

static int sys_write_(wasm_exec_env_t env, int fd, const void *buf, int n)
{
    io_t *io = get_io(fd);
    if (n < 0 || !io) return -1;
    return io_write(io, buf, n) == 0 ? n : -1;
}

static int sys_read_(wasm_exec_env_t env, int fd, void *buf, int n)
{
    io_t *io = get_io(fd);
    if (n < 0 || !io) return -1;
    int r = io_read(io, buf, n);
    if (r < 0) { interrupted(env); return -1; }
    return r;
}

static int sys_open_(wasm_exec_env_t env, const char *path, int mode)    // 0 read, 1 write/truncate, 2 append
{
    char v[200], host[300];
    fs_resolve(P->cwd, path, v, sizeof v);
    fs_host_path(v, host, sizeof host);
    return add_fd(io_open_file(host, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab"));
}


/* ---------------- system information (for monitors like btop) ---------------- */
typedef struct { char name[12]; uint8_t state, prio; uint16_t stack, cpu10, pid; } task_rec_t;   // matches esp_task_t in mini.h

static int sys_sysinfo_(wasm_exec_env_t env, int what)
{
    size_t total = 0, used = 0;
    switch (what) {
    case 0: return (int)(esp_timer_get_time() / 1000000);                               // uptime, seconds
    case 1: return (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);                         // DRAM free
    case 2: return (int)heap_caps_get_total_size(MALLOC_CAP_8BIT);
    case 3: return (int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    case 4: return (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    case 5: return (int)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT);                    // IRAM pool (program files)
    case 6: return (int)heap_caps_get_total_size(MALLOC_CAP_IRAM_8BIT);
    case 7: return (int)heap_caps_get_free_size(MALLOC_CAP_EXEC);                         // IRAM for AOT code
    case 8: return (int)heap_caps_get_total_size(MALLOC_CAP_EXEC);
    case 9: case 10: esp_littlefs_info("storage", &total, &used); return (int)(what == 9 ? used : total);
    case 11: { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }
    case 12: return (int)ets_get_cpu_frequency();                                         // MHz
    case 13: return (int)uxTaskGetNumberOfTasks();
    case 16: return bench_cores();                                                        // CPU cores running FreeRTOS
    case 17: return (int)xPortGetCoreID();                                                // core this program runs on
    case 14: return P && P->t ? P->t->cols : 80;
    case 15: return P && P->t ? P->t->rows : 24;
    }
    return -1;
}

static int sys_netinfo_(wasm_exec_env_t env, int what, char *buf, int n)                  // 0 hostname, 1 ip, 2 ssid
{
    if (n <= 0) return -1;
    buf[0] = 0;
    if (what == 0) strlcpy(buf, net_hostname(), n);
    else if (what == 1) wifi_mgr_ip(buf, n);
    else if (what == 2) strlcpy(buf, wifi_mgr_ssid(), n);
    else return -1;
    return (int)strlen(buf);
}

#define MAXT 24
static struct { uint16_t pid; uint32_t last; } s_prev[MAXT];
static uint32_t s_prev_total;

// Fills buf with task_rec_t records (up to n bytes). CPU% is measured since the previous call.
// Returns the number of records.
static int sys_tasks_(wasm_exec_env_t env, void *buf, int n)
{
    int cap = n / (int)sizeof(task_rec_t);
    UBaseType_t nt = uxTaskGetNumberOfTasks();
    if (nt > MAXT) nt = MAXT;
    TaskStatus_t *st = malloc(nt * sizeof *st);
    if (!st || cap <= 0) { free(st); return -1; }
    uint32_t total = 0;
    nt = uxTaskGetSystemState(st, nt, &total);
    uint32_t dtotal = total - s_prev_total;
    task_rec_t *out = buf;
    int k = 0;
    for (UBaseType_t i = 0; i < nt && k < cap; i++) {
        uint32_t prev = 0; int slot = -1;
        for (int j = 0; j < MAXT; j++) if (s_prev[j].pid == st[i].xTaskNumber) { prev = s_prev[j].last; slot = j; break; }
        if (slot < 0) for (int j = 0; j < MAXT; j++) if (!s_prev[j].pid) { slot = j; break; }
        uint32_t dt = st[i].ulRunTimeCounter - prev;
        if (slot >= 0) { s_prev[slot].pid = st[i].xTaskNumber; s_prev[slot].last = st[i].ulRunTimeCounter; }
        task_rec_t *r = &out[k++];
        memset(r, 0, sizeof *r);
        strlcpy(r->name, st[i].pcTaskName, sizeof r->name);
        r->state = "RrBSD?"[st[i].eCurrentState < 5 ? st[i].eCurrentState : 5];
        r->prio = (uint8_t)st[i].uxCurrentPriority;
        r->stack = (uint16_t)(st[i].usStackHighWaterMark > 65535 ? 65535 : st[i].usStackHighWaterMark);
        r->cpu10 = dtotal ? (uint16_t)((uint64_t)dt * 1000 / dtotal) : 0;
        r->pid = (uint16_t)st[i].xTaskNumber;
    }
    s_prev_total = total;
    free(st);
    return k;
}

// Raw key read for full-screen programs: byte, -1 timeout, -2 end of input, -3 Ctrl-C.
static int sys_getkey_(wasm_exec_env_t env, int ms)
{
    io_t *io = P ? P->in : NULL;
    if (!io || io->kind != IO_TERM || !io->t) return -2;
    int c = term_getc(io->t, ms);
    if (c == -3) { io->t->sigint = false; return -3; }
    return c;
}

static int sys_argc_(wasm_exec_env_t env) { return P->argc; }

static int sys_arg_(wasm_exec_env_t env, int i, char *buf, int n)        // copies argv[i]; returns its length or -1
{
    if (i < 0 || i >= P->argc || n <= 0) return -1;
    size_t l = strlen(P->argv[i]);
    if ((int)l >= n) l = n - 1;
    memcpy(buf, P->argv[i], l);
    buf[l] = 0;
    return (int)l;
}

static int sys_isatty_(wasm_exec_env_t env, int fd)
{
    io_t *io = get_io(fd);
    return io && io->kind == IO_TERM && io->t && io->t->pty;
}

// op 1: catch Ctrl-C (reads/runs fail softly instead of killing the program); 2: stop catching;
// 0: return 1 and clear if a Ctrl-C is pending.
static int sys_sigint_(wasm_exec_env_t env, int op)
{
    if (op == 1) { P->catch_int = true; return 0; }
    if (op == 2) { P->catch_int = false; return 0; }
    int pend = P->t->sigint;
    P->t->sigint = false;
    return pend;
}

static int sys_membuf_(wasm_exec_env_t env) { return add_fd(io_new_mem()); }

static int sys_rewind_(wasm_exec_env_t env, int fd)
{
    io_t *io = get_io(fd);
    if (!io) return -1;
    io_rewind(io);
    return 0;
}

static int sys_close_(wasm_exec_env_t env, int fd)
{
    if (fd < 3 || fd >= 3 + MAX_FDS || !P->fds[fd - 3]) return -1;
    io_close(P->fds[fd - 3]);
    P->fds[fd - 3] = NULL;
    return 0;
}

static int sys_getcwd_(wasm_exec_env_t env, char *buf, int n)
{
    if (n <= (int)strlen(P->cwd)) return -1;
    strcpy(buf, P->cwd);
    return (int)strlen(P->cwd);
}

static int sys_stat_(wasm_exec_env_t env, const char *path)               // 1 file, 2 directory, -1 missing
{
    char v[200], host[300];
    struct stat st;
    fs_resolve(P->cwd, path, v, sizeof v);
    fs_host_path(v, host, sizeof host);
    if (stat(host, &st)) return -1;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static int sys_chdir_(wasm_exec_env_t env, const char *path)
{
    char v[200], host[300];
    struct stat st;
    fs_resolve(P->cwd, path, v, sizeof v);
    fs_host_path(v, host, sizeof host);
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) return -1;
    strlcpy(P->cwd, v, sizeof P->cwd);
    return 0;
}

// Run a command (built-in or program) and wait for it. `blob` holds argc NUL-terminated words.
// fd_in/out/err: 0..2 = inherit this program's streams, >=3 = a stream opened with sys_open/sys_membuf.
static int sys_run_(wasm_exec_env_t env, const char *blob, int blob_len, int argc, int fd_in, int fd_out, int fd_err)
{
    if (interrupted(env)) return 130;
    if (argc <= 0 || argc > 32 || blob_len <= 0) return 127;
    char *copy = malloc(blob_len + 1);
    char **argv = calloc(argc + 1, sizeof *argv);
    if (!copy || !argv) { free(copy); free(argv); return 126; }
    memcpy(copy, blob, blob_len);
    copy[blob_len] = 0;
    char *p = copy;
    int n = 0;
    while (n < argc && p < copy + blob_len) { argv[n++] = p; p += strlen(p) + 1; }
    io_t *i = get_io(fd_in), *o = get_io(fd_out), *e = get_io(fd_err);
    int rc = (i && o && e && n == argc) ? shell_run_argv(P->t, P->cwd, argc, argv, i, o, e) : 126;
    free(argv);
    free(copy);
    return rc;
}

static uint32_t sys_millis_(wasm_exec_env_t env) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void sys_sleep_ms_(wasm_exec_env_t env, uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
        if (interrupted(env)) return;
    }
}

static int sys_gpio_mode_(wasm_exec_env_t env, int pin, int out) { return drv_gpio_mode(pin, out); }
static int sys_gpio_write_(wasm_exec_env_t env, int pin, int level) { return drv_gpio_write(pin, level); }
static int sys_gpio_read_(wasm_exec_env_t env, int pin) { return drv_gpio_read(pin); }
static uint32_t sys_micros_(wasm_exec_env_t env) { return drv_micros(); }
static int sys_adc_read_(wasm_exec_env_t env, int pin) { return drv_adc_read(pin); }
static int sys_pwm_(wasm_exec_env_t env, int pin, int freq, int permille) { return drv_pwm(pin, freq, permille); }
static int sys_dac_write_(wasm_exec_env_t env, int pin, int v) { return drv_dac_write(pin, v); }
static int sys_i2c_init_(wasm_exec_env_t env, int sda, int scl, int hz) { return drv_i2c_init(sda, scl, hz); }
static int sys_i2c_probe_(wasm_exec_env_t env, int addr) { return drv_i2c_probe(addr); }
static int sys_i2c_write_(wasm_exec_env_t env, int addr, uint8_t *buf, int n) { return drv_i2c_write(addr, buf, n); }
static int sys_i2c_read_(wasm_exec_env_t env, int addr, uint8_t *buf, int n) { return drv_i2c_read(addr, buf, n); }
static int sys_i2c_wr_(wasm_exec_env_t env, int addr, uint8_t *w, int wn, uint8_t *r, int rn) { return drv_i2c_write_read(addr, w, wn, r, rn); }
static int sys_uart_open_(wasm_exec_env_t env, int port, int tx, int rx, int baud) { return drv_uart_open(port, tx, rx, baud); }
static int sys_uart_write_(wasm_exec_env_t env, int port, uint8_t *buf, int n) { return drv_uart_write(port, buf, n); }
static int sys_uart_read_(wasm_exec_env_t env, int port, uint8_t *buf, int n, int ms) { return drv_uart_read(port, buf, n, ms); }
static int sys_uart_close_(wasm_exec_env_t env, int port) { return drv_uart_close(port); }

/* ---------------- Phase 4: files, sockets, time, kv, mqtt, system ---------------- */
#define SYS_ABI 6                                    // bump when a syscall is added; programs may check sys_abi()

static int vhost(const char *path, char *host, size_t n)
{
    char v[200];
    fs_resolve(P->cwd, path, v, sizeof v);
    fs_host_path(v, host, n);
    return 0;
}
static int sys_unlink_(wasm_exec_env_t env, const char *path) { char h[300]; vhost(path, h, sizeof h); return unlink(h) ? -1 : 0; }
static int sys_mkdir_(wasm_exec_env_t env, const char *path) { char h[300]; vhost(path, h, sizeof h); return mkdir(h, 0777) ? -1 : 0; }
static int sys_rmdir_(wasm_exec_env_t env, const char *path) { char h[300]; vhost(path, h, sizeof h); return rmdir(h) ? -1 : 0; }
static int sys_rename_(wasm_exec_env_t env, const char *a, const char *b)
{
    char ha[300], hb[300];
    vhost(a, ha, sizeof ha); vhost(b, hb, sizeof hb);
    unlink(hb);                                      // POSIX rename replaces; LittleFS needs the target gone for files
    return rename(ha, hb) ? -1 : 0;
}
static int sys_fsize_(wasm_exec_env_t env, const char *path)
{
    char h[300]; struct stat st;
    vhost(path, h, sizeof h);
    return stat(h, &st) ? -1 : (int)st.st_size;
}
// Names of a directory, one per line ('/' appended to subdirectories). Returns bytes written (truncated to fit) or -1.
static int sys_listdir_(wasm_exec_env_t env, const char *path, char *buf, int n)
{
    char h[300]; vhost(path, h, sizeof h);
    DIR *d = opendir(h);
    if (!d || n < 2) return -1;
    int k = 0; struct dirent *e;
    while ((e = readdir(d))) {
        int l = (int)strlen(e->d_name);
        if (k + l + 2 >= n) break;
        memcpy(buf + k, e->d_name, l); k += l;
        if (e->d_type == DT_DIR) buf[k++] = '/';
        buf[k++] = '\n';
    }
    closedir(d);
    buf[k] = 0;
    return k;
}
static int sys_seek_(wasm_exec_env_t env, int fd, int off, int whence)
{
    io_t *io = get_io(fd);
    return io ? (int)io_seek(io, off, whence) : -1;
}

/* sockets: the returned value is an ordinary fd (sys_read / sys_write / sys_close work on it) */
static int sys_tcp_connect_(wasm_exec_env_t env, const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (timeout_ms <= 0) timeout_ms = 5000;
    // lwIP ignores SO_SNDTIMEO for connect(): a silent host would block for minutes. Connect non-blocking and wait with select(),
    // in slices so that Ctrl-C works.
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc && errno == EINPROGRESS) {
        TickType_t t0 = xTaskGetTickCount(), lim = pdMS_TO_TICKS(timeout_ms);
        rc = -1;
        for (;;) {
            fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
            struct timeval tv = { 0, 100000 };
            int r = select(s + 1, NULL, &ws, NULL, &tv);
            if (r > 0) { int err = 0; socklen_t el = sizeof err; getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el); rc = err ? -1 : 0; break; }
            if (r < 0 || (TickType_t)(xTaskGetTickCount() - t0) >= lim || interrupted(env)) break;
        }
    }
    if (rc) { close(s); return -1; }
    fcntl(s, F_SETFL, fl);
    return add_fd(io_new_sock(s, SOCK_STREAM, false));
}
static int sys_tcp_listen_(wasm_exec_env_t env, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 2)) { close(s); return -1; }
    return add_fd(io_new_sock(s, SOCK_STREAM, true));
}
static int sys_tcp_accept_(wasm_exec_env_t env, int fd, int timeout_ms)       // new fd, 0 timeout, -1 error
{
    io_t *io = get_io(fd);
    if (!io || io->kind != IO_SOCK || !io->listening) return -1;
    fd_set rs; FD_ZERO(&rs); FD_SET(io->sock, &rs);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(io->sock + 1, &rs, NULL, NULL, &tv);
    if (r <= 0) return r == 0 ? 0 : -1;
    int c = accept(io->sock, NULL, NULL);
    if (c < 0) return -1;
    int f = add_fd(io_new_sock(c, SOCK_STREAM, false));
    return f < 0 ? -1 : f;
}
static int sys_sock_timeout_(wasm_exec_env_t env, int fd, int ms)       // receive timeout; sys_read then returns -2 on expiry
{
    io_t *io = get_io(fd);
    return io ? io_sock_timeout(io, ms) : -1;
}
static int sys_udp_open_(wasm_exec_env_t env, int port)                  // port 0 = any
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    return add_fd(io_new_sock(s, SOCK_DGRAM, false));
}
static int sys_udp_sendto_(wasm_exec_env_t env, int fd, const char *host, int port, const void *buf, int n)
{
    io_t *io = get_io(fd);
    if (!io || io->kind != IO_SOCK || io->sock_type != SOCK_DGRAM) return -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
    int r = sendto(io->sock, buf, n, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return r;
}
static int sys_dns_(wasm_exec_env_t env, const char *host, char *buf, int n)   // resolve to a dotted IPv4 string
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (n < 16 || getaddrinfo(host, NULL, &hints, &res) || !res) return -1;
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, buf, n);
    freeaddrinfo(res);
    return (int)strlen(buf);
}

/* HTTPS/HTTP download straight into a file (TLS cannot live in a 64 KB program). ABI 4. */
static bool http_cancel_(void *env) { return interrupted((wasm_exec_env_t)env); }
static int sys_http_get_(wasm_exec_env_t env, const char *url, const char *path, int max_bytes, int timeout_ms)
{
    char h[300]; vhost(path, h, sizeof h);
    return http_download(url, h, max_bytes, timeout_ms, http_cancel_, env);
}

/* ICMP echo ("ping") through a raw socket. ABI 5.
 * Returns the round-trip time in microseconds (>= 0), or -1 timeout, -2 cannot send / no raw socket, -3 destination unreachable, -4 interrupted. */
static uint16_t icmp_sum(const uint8_t *d, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)(d[i] << 8 | d[i + 1]);
    if (n & 1) sum += (uint32_t)d[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static int sys_ping_(wasm_exec_env_t env, const char *host, int timeout_ms, int seq, int size)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_RAW }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return -2;
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    if (size < 0) size = 0;
    if (size > 400) size = 400;
    if (timeout_ms < 50) timeout_ms = 50;
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return -2;
    uint8_t pkt[8 + 400];
    const uint16_t id = 0x4d45;                       // "ME"
    pkt[0] = 8; pkt[1] = 0; pkt[2] = pkt[3] = 0;
    pkt[4] = id >> 8; pkt[5] = id & 255; pkt[6] = (seq >> 8) & 255; pkt[7] = seq & 255;
    for (int i = 0; i < size; i++) pkt[8 + i] = (uint8_t)(0x20 + i % 64);
    uint16_t ck = icmp_sum(pkt, 8 + size);
    pkt[2] = ck >> 8; pkt[3] = ck & 255;
    int64_t t0 = esp_timer_get_time();
    if (sendto(s, pkt, 8 + size, 0, (struct sockaddr *)&dst, sizeof dst) < 0) { close(s); return -2; }
    int result = -1;
    for (;;) {
        int64_t left = (int64_t)timeout_ms * 1000 - (esp_timer_get_time() - t0);
        if (left <= 0) break;
        if (interrupted(env) || (P && P->t && P->t->sigint)) { result = -4; break; }          // a program that catches Ctrl-C clears the flag itself
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
        int64_t slice = left < 100000 ? left : 100000;
        struct timeval tv = { (time_t)(slice / 1000000), (suseconds_t)(slice % 1000000) };
        int r = select(s + 1, &rs, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        uint8_t rx[600];
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, rx, sizeof rx, 0, (struct sockaddr *)&from, &fl);
        if (n < 28) continue;
        int ihl = (rx[0] & 15) * 4;
        if (ihl < 20 || n < ihl + 8) continue;
        const uint8_t *ic = rx + ihl;
        if (ic[0] == 0 && from.sin_addr.s_addr == dst.sin_addr.s_addr && ic[4] == (id >> 8) && ic[5] == (id & 255) && ic[6] == ((seq >> 8) & 255) && ic[7] == (seq & 255)) {
            result = (int)(esp_timer_get_time() - t0);
            break;
        }
        if (ic[0] == 3 && n >= ihl + 8 + 20 + 8) {                    // destination unreachable quoting our packet
            const uint8_t *q = ic + 8 + ((ic[8] & 15) * 4);
            if (q + 8 <= rx + n && q[0] == 8 && q[4] == (id >> 8) && q[5] == (id & 255)) { result = -3; break; }
        }
    }
    close(s);
    return result;
}

/* time */
static uint32_t sys_time_(wasm_exec_env_t env) { return time_now(); }
static int sys_time_state_(wasm_exec_env_t env) { return time_state(); }
static int sys_localtime_(wasm_exec_env_t env, uint32_t epoch, int *out, int n)   // out[8]: sec,min,hour,mday,mon,year,wday,yday
{
    if (n < 32) return -1;
    return time_local(epoch, out);
}
static int sys_tz_(wasm_exec_env_t env, const char *spec) { return time_set_tz_spec(spec) ? 0 : -1; }

/* system */
static int sys_abi_(wasm_exec_env_t env) { return SYS_ABI; }
static uint32_t sys_random_(wasm_exec_env_t env) { return esp_random(); }
static int sys_reboot_(wasm_exec_env_t env) { vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); return 0; }
static int sys_log_(wasm_exec_env_t env, const char *msg) { ESP_LOGI("prog", "%s", msg); return 0; }
static int sys_version_(wasm_exec_env_t env, char *buf, int n)
{
    if (n <= 0) return -1;
    strlcpy(buf, esp_app_get_description()->version, n);
    return (int)strlen(buf);
}

/* persistent key-value store (NVS) */
static int sys_kv_get_(wasm_exec_env_t env, const char *key, char *buf, int n) { return kv_get(key, buf, n); }
static int sys_kv_set_(wasm_exec_env_t env, const char *key, const char *buf, int n) { return kv_set(key, buf, n); }
static int sys_kv_del_(wasm_exec_env_t env, const char *key) { return kv_del(key); }
typedef struct { int want, at; char *buf; int n; int rc; } kvit_t;
static void kv_nth(const char *key, size_t len, void *ctx)
{
    kvit_t *k = ctx;
    if (k->at++ == k->want) { strlcpy(k->buf, key, k->n); k->rc = (int)strlen(k->buf); }
}
static int sys_kv_key_(wasm_exec_env_t env, int idx, char *buf, int n)      // name of the idx-th key, -1 past the end
{
    kvit_t k = { idx, 0, buf, n, -1 };
    if (n <= 0) return -1;
    kv_list(kv_nth, &k);
    return k.rc;
}

/* mqtt (native client, persistent connection) */
static int sys_mqtt_state_(wasm_exec_env_t env) { return mqtt_state(); }
static int sys_mqtt_pub_(wasm_exec_env_t env, const char *topic, const void *p, int n, int retain, int qos) { return mqtt_publish(topic, p, n, retain, qos); }
static int sys_mqtt_sub_(wasm_exec_env_t env, const char *f, int qos) { return mqtt_subscribe(f, qos); }
static int sys_mqtt_unsub_(wasm_exec_env_t env, const char *f) { return mqtt_unsubscribe(f); }
static int sys_mqtt_get_(wasm_exec_env_t env, const char *topic, char *buf, int n) { return mqtt_get(topic, buf, n); }
static int sys_mqtt_age_(wasm_exec_env_t env, const char *topic) { return mqtt_age_ms(topic); }
static int sys_mqtt_config_(wasm_exec_env_t env, const char *host, int port, const char *user, const char *pass, const char *cid)
{ return mqtt_set_config(host, port, user, pass, cid); }
static int sys_mqtt_info_(wasm_exec_env_t env, int what, char *buf, int n)     // 0 host, 1 user, 2 client id, 3 port (as text); the password is never returned
{
    char h[64], u[32], id[48]; int port;
    if (n <= 0) return -1;
    mqtt_get_config(h, u, id, sizeof h, &port);
    if (what == 0) strlcpy(buf, h, n);
    else if (what == 1) strlcpy(buf, u, n);
    else if (what == 2) strlcpy(buf, id, n);
    else if (what == 3) snprintf(buf, n, "%d", port);
    else return -1;
    return (int)strlen(buf);
}
static int sys_mqtt_ctl_(wasm_exec_env_t env, int on) { if (on) mqtt_start(); else mqtt_stop(); return 0; }

/* extra drivers */
static void sys_delay_us_(wasm_exec_env_t env, int us) { drv_delay_us(us); }
static int sys_pulse_in_(wasm_exec_env_t env, int pin, int level, int to) { return drv_pulse_in(pin, level, to); }
static int sys_sonar_pulse_(wasm_exec_env_t env, int trig, int echo, int to) { return drv_sonar_pulse(trig, echo, to); }
static int sys_ds18b20_(wasm_exec_env_t env, int pin) { return drv_ds18b20(pin); }
static int sys_adc_mv_(wasm_exec_env_t env, int pin) { return drv_adc_read_mv(pin); }
static int sys_spi_open_(wasm_exec_env_t env, int sck, int mosi, int miso, int hz, int mode) { return drv_spi_open(sck, mosi, miso, hz, mode); }
static int sys_spi_xfer_(wasm_exec_env_t env, int cs, uint8_t *tx, int n, uint8_t *rx, int n2) { return n == n2 ? drv_spi_xfer(cs, tx, rx, n) : -1; }
static int sys_pcnt_open_(wasm_exec_env_t env, int unit, int pin) { return drv_pcnt_open(unit, pin); }
static int sys_pcnt_read_(wasm_exec_env_t env, int unit) { return drv_pcnt_read(unit); }
static int sys_bench_(wasm_exec_env_t env, int kind, int cores, int ms, int32_t *out, int n) { return bench_run(kind, cores, ms, out, n); }
static int sys_pcnt_clear_(wasm_exec_env_t env, int unit) { return drv_pcnt_clear(unit); }

static NativeSymbol s_natives[] = {
    { "sys_write",      sys_write_,      "(i*~)i" },
    { "sys_read",       sys_read_,       "(i*~)i" },
    { "sys_open",       sys_open_,       "($i)i" },
    { "sys_close",      sys_close_,      "(i)i" },
    { "sys_sysinfo",    sys_sysinfo_,    "(i)i" },
    { "sys_netinfo",    sys_netinfo_,    "(i*~)i" },
    { "sys_tasks",      sys_tasks_,      "(*~)i" },
    { "sys_getkey",     sys_getkey_,     "(i)i" },
    { "sys_argc",       sys_argc_,       "()i" },
    { "sys_arg",        sys_arg_,        "(i*~)i" },
    { "sys_isatty",     sys_isatty_,     "(i)i" },
    { "sys_sigint",     sys_sigint_,     "(i)i" },
    { "sys_membuf",     sys_membuf_,     "()i" },
    { "sys_rewind",     sys_rewind_,     "(i)i" },
    { "sys_getcwd",     sys_getcwd_,     "(*~)i" },
    { "sys_chdir",      sys_chdir_,      "($)i" },
    { "sys_stat",       sys_stat_,       "($)i" },
    { "sys_run",        sys_run_,        "(*~iiii)i" },
    { "sys_millis",     sys_millis_,     "()i" },
    { "sys_sleep_ms",   sys_sleep_ms_,   "(i)" },
    { "sys_gpio_mode",  sys_gpio_mode_,  "(ii)i" },
    { "sys_gpio_write", sys_gpio_write_, "(ii)i" },
    { "sys_gpio_read",  sys_gpio_read_,  "(i)i" },
    { "sys_micros",     sys_micros_,     "()i" },
    { "sys_adc_read",   sys_adc_read_,   "(i)i" },
    { "sys_pwm",        sys_pwm_,        "(iii)i" },
    { "sys_dac_write",  sys_dac_write_,  "(ii)i" },
    { "sys_i2c_init",   sys_i2c_init_,   "(iii)i" },
    { "sys_i2c_probe",  sys_i2c_probe_,  "(i)i" },
    { "sys_i2c_write",  sys_i2c_write_,  "(i*~)i" },
    { "sys_i2c_read",   sys_i2c_read_,   "(i*~)i" },
    { "sys_i2c_wr",     sys_i2c_wr_,     "(i*~*~)i" },
    { "sys_uart_open",  sys_uart_open_,  "(iiii)i" },
    { "sys_uart_write", sys_uart_write_, "(i*~)i" },
    { "sys_uart_read",  sys_uart_read_,  "(i*~i)i" },
    { "sys_uart_close", sys_uart_close_, "(i)i" },
    { "sys_unlink", sys_unlink_, "($)i" }, { "sys_mkdir", sys_mkdir_, "($)i" }, { "sys_rmdir", sys_rmdir_, "($)i" },
    { "sys_rename", sys_rename_, "($$)i" }, { "sys_fsize", sys_fsize_, "($)i" }, { "sys_listdir", sys_listdir_, "($*~)i" },
    { "sys_seek", sys_seek_, "(iii)i" },
    { "sys_tcp_connect", sys_tcp_connect_, "($ii)i" }, { "sys_tcp_listen", sys_tcp_listen_, "(i)i" },
    { "sys_tcp_accept", sys_tcp_accept_, "(ii)i" }, { "sys_sock_timeout", sys_sock_timeout_, "(ii)i" },
    { "sys_udp_open", sys_udp_open_, "(i)i" }, { "sys_udp_sendto", sys_udp_sendto_, "(i$i*~)i" }, { "sys_dns", sys_dns_, "($*~)i" },
    { "sys_http_get", sys_http_get_, "($$ii)i" },
    { "sys_ping", sys_ping_, "($iii)i" },
    { "sys_time", sys_time_, "()i" }, { "sys_time_state", sys_time_state_, "()i" }, { "sys_localtime", sys_localtime_, "(i*~)i" },
    { "sys_tz", sys_tz_, "($)i" },
    { "sys_abi", sys_abi_, "()i" }, { "sys_random", sys_random_, "()i" }, { "sys_reboot", sys_reboot_, "()i" },
    { "sys_log", sys_log_, "($)i" }, { "sys_version", sys_version_, "(*~)i" },
    { "sys_kv_get", sys_kv_get_, "($*~)i" }, { "sys_kv_set", sys_kv_set_, "($*~)i" }, { "sys_kv_del", sys_kv_del_, "($)i" },
    { "sys_kv_key", sys_kv_key_, "(i*~)i" },
    { "sys_mqtt_state", sys_mqtt_state_, "()i" }, { "sys_mqtt_pub", sys_mqtt_pub_, "($*~ii)i" }, { "sys_mqtt_sub", sys_mqtt_sub_, "($i)i" },
    { "sys_mqtt_unsub", sys_mqtt_unsub_, "($)i" }, { "sys_mqtt_get", sys_mqtt_get_, "($*~)i" }, { "sys_mqtt_age", sys_mqtt_age_, "($)i" },
    { "sys_mqtt_config", sys_mqtt_config_, "($i$$$)i" }, { "sys_mqtt_ctl", sys_mqtt_ctl_, "(i)i" }, { "sys_mqtt_info", sys_mqtt_info_, "(i*~)i" },
    { "sys_delay_us", sys_delay_us_, "(i)" }, { "sys_pulse_in", sys_pulse_in_, "(iii)i" }, { "sys_sonar_pulse", sys_sonar_pulse_, "(iii)i" },
    { "sys_ds18b20", sys_ds18b20_, "(i)i" }, { "sys_adc_mv", sys_adc_mv_, "(i)i" },
    { "sys_spi_open", sys_spi_open_, "(iiiii)i" }, { "sys_spi_xfer", sys_spi_xfer_, "(i*~*~)i" },
    { "sys_bench", sys_bench_, "(iii*~)i" },
    { "sys_pcnt_open", sys_pcnt_open_, "(ii)i" }, { "sys_pcnt_read", sys_pcnt_read_, "(i)i" }, { "sys_pcnt_clear", sys_pcnt_clear_, "(i)i" },
};

/* ---------------- runtime ---------------- */
// Allocator for the WASM runtime: normal DRAM first. When a big request (e.g. a 64 KB linear memory)
// does not fit, fall back to the byte-accessible IRAM pool (slower, but keeps nested programs possible).
static void *rt_malloc(unsigned size)
{
    void *p = malloc(size);
    if (!p && size >= 8192) p = heap_caps_malloc(size, MALLOC_CAP_IRAM_8BIT);
    if (!p) ESP_LOGW(TAG, "runtime allocation of %u bytes failed (dram free %u, largest %u)", size,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return p;
}
static void *rt_realloc(void *p, unsigned size) { return realloc(p, size); }
static void rt_free(void *p) { free(p); }

void aot_reserve_pool(void);

bool aot_init(void)
{
    s_run_lock = xSemaphoreCreateMutex();
    aot_reserve_pool();                              // before any AOT text is placed in IRAM
    RuntimeInitArgs a;
    memset(&a, 0, sizeof a);
    a.mem_alloc_type = Alloc_With_Allocator;
    a.mem_alloc_option.allocator.malloc_func = (void *)rt_malloc;
    a.mem_alloc_option.allocator.realloc_func = (void *)rt_realloc;
    a.mem_alloc_option.allocator.free_func = (void *)rt_free;
    if (!wasm_runtime_full_init(&a)) { ESP_LOGE(TAG, "wasm_runtime_full_init failed"); return false; }
    if (!wasm_runtime_register_natives("env", s_natives, sizeof s_natives / sizeof s_natives[0])) {
        ESP_LOGE(TAG, "registering sys_* natives failed");
        return false;
    }
    return true;
}

void aot_kill(void)                                  // terminate the running program unconditionally
{
    wasm_module_inst_t i = s_inst;
    if (i) wasm_runtime_terminate(i);
}

void aot_interrupt(void)
{
    if (P && P->catch_int) return;                  // the running program handles Ctrl-C itself (shells, btop)
    wasm_module_inst_t i = s_inst;
    if (i) wasm_runtime_terminate(i);
}

static uint8_t *read_file(const char *host, uint32_t *size)
{
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    // Prefer IRAM (byte-accessible via CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY): the file buffer must stay
    // resident while the module lives, and this keeps the faster DRAM free for the program's linear memory.
    uint8_t *buf = NULL;
    if (n > 0) {
        buf = heap_caps_malloc(n, MALLOC_CAP_IRAM_8BIT);
        if (!buf) buf = malloc(n);
    }
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = buf ? (uint32_t)n : 0;
    return buf;
}

int aot_run_nolock(term_t *t, const char *cwd, const char *vpath, int argc, char **argv, io_t *in, io_t *out, io_t *err)
{
    char host[300], ebuf[128] = "";
    fs_host_path(vpath, host, sizeof host);

    uint32_t size;
    uint8_t *buf = read_file(host, &size);
    if (!buf) { io_write(err, "cannot read program\n", 20); return -1; }

    LoadArgs la = { 0 };
    la.name = "";
    // The AOT loader keeps pointers into the file buffer (data segments), so it must outlive the module.
    wasm_module_t mod = wasm_runtime_load_ex(buf, size, &la, ebuf, sizeof ebuf);
    if (!mod) {
        free(buf);
        char m[200]; int l = snprintf(m, sizeof m, "%s: load failed: %s\n", vpath, ebuf);
        io_write(err, m, l);
        return -2;
    }

    // stdio for programs linked against wasi-libc goes through the /dev/pts VFS device (top-level only).
    // WASI directory access is unavailable on ESP-IDF; programs use the sys_* calls for files.
    int fdin = open(TTY_PATH, O_RDWR), fdout = open(TTY_PATH, O_RDWR), fderr = open(TTY_PATH, O_RDWR);
    static const char *env[] = { "PATH=/esp/.local/bin:/bin", "HOME=/esp", "TERM=xterm" };
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, NULL, 0, env, 3, argv, argc, fdin, fdout, fderr);

    proc_t *me = calloc(1, sizeof *me);
    int rc = -3;
    if (!me) { io_write(err, "out of memory\n", 14); goto done; }
    me->parent = P; me->t = t; me->owner = xTaskGetCurrentTaskHandle();
    strlcpy(me->cwd, cwd, sizeof me->cwd);
    me->in = in; me->out = out; me->err = err;
    if (!me->parent) s_interactive = t && t->pty;
    me->argc = argc; me->argv = argv;
    P = me;

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, PROG_STACK, PROG_HEAP, ebuf, sizeof ebuf);
    if (!inst) {
        char m[200]; int l = snprintf(m, sizeof m, "%s: instantiate failed: %s\n", vpath, ebuf);
        io_write(err, m, l);
        if (me->parent && strstr(ebuf, "linear memory")) {
            static const char hint[] = "(not enough RAM to run a program inside a program: a second 64 KB program memory does not fit; "
                                       "inside sh use built-in commands such as ls cat grep head wc)\n";
            io_write(err, hint, sizeof hint - 1);
        }
    } else {
        wasm_module_inst_t saved = s_inst;
        if (!me->parent) { t->sigint = false; t->on_sigint = aot_interrupt; }
        s_inst = inst;
        // Freestanding programs export esp_main() and read their arguments with sys_argc/sys_arg;
        // programs built against wasi-libc use the standard _start path.
        wasm_function_inst_t entry = wasm_runtime_lookup_function(inst, "esp_main");
        int main_rc = 0; bool direct = false;
        if (entry) {
            wasm_exec_env_t ee = wasm_runtime_create_exec_env(inst, 2048);
            if (ee) {
                uint32_t res[2] = { 0, 0 };
                direct = true;
                wasm_runtime_call_wasm(ee, entry, 0, res);
                main_rc = (int)res[0];
                wasm_runtime_destroy_exec_env(ee);
            }
        } else {
            wasm_application_execute_main(inst, argc, argv);
        }
        s_inst = saved;
        if (!me->parent) t->on_sigint = NULL;
        const char *ex = wasm_runtime_get_exception(inst);
        rc = direct ? main_rc : (int)wasm_runtime_get_wasi_exit_code(inst);
        if (ex && (strstr(ex, "interrupted") || strstr(ex, "terminated"))) {
            rc = 130;                                   // Ctrl-C: silent, like a shell
            io_write(err, "\n", 1);
        } else if (ex && !strstr(ex, "proc exit")) {
            char m[200]; int l = snprintf(m, sizeof m, "%s: %s\n", argv[0], ex);
            io_write(err, m, l);
            if (!rc) rc = 1;
        }
        // Only the top-level program clears the Ctrl-C flag, so a nested child's interrupt also stops its parent.
        if (!me->parent) t->sigint = false;
        wasm_runtime_deinstantiate(inst);
    }
    for (int i = 0; i < MAX_FDS; i++) io_close(me->fds[i]);
    if (!me->parent) s_interactive = false;
    P = me->parent;
    free(me);
done:
    wasm_runtime_unload(mod);
    free(buf);
    if (fdin >= 0) close(fdin);
    if (fdout >= 0) close(fdout);
    if (fderr >= 0) close(fderr);
    return rc;
}

/* ---------------- linear-memory slots ---------------- */
// WAMR allocates a program's linear memory (64 KB) with os_mmap(); the linker wraps it (see CMakeLists).
// An SSH session fragments DRAM, so by the time a program runs there is no 64 KB block left. Two slots are
// therefore reserved at boot, before anything fragments memory:
//   slot 0: DRAM  - fast, used by the top-level program
//   slot 1: IRAM  - byte-accessible but slower (accesses trap into a handler); used by a nested program
//             (a shell running a command)
#define SLOT_SIZE (65536 + 64)
#define SLOT_MIN  60000                          // only linear memories (64 KB) use the slots
static struct { uint8_t *base; bool busy; } s_slot[2];

void aot_reserve_pool(void)
{
    static bool done;
    if (done) return;                                // called early from app_main and again from aot_init
    done = true;
    s_slot[0].base = heap_caps_malloc(SLOT_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_slot[1].base = heap_caps_malloc(SLOT_SIZE, MALLOC_CAP_IRAM_8BIT);
    ESP_LOGI(TAG, "linear-memory slots: DRAM %s, IRAM %s", s_slot[0].base ? "ok" : "missing", s_slot[1].base ? "ok" : "missing");
}

// For "free": state of the two linear-memory slots, e.g. "DRAM free, IRAM missing".
void aot_slot_info(char *out, size_t n)
{
    snprintf(out, n, "DRAM %s, IRAM %s (slot %d bytes)", !s_slot[0].base ? "missing" : s_slot[0].busy ? "busy" : "free",
             !s_slot[1].base ? "missing" : s_slot[1].busy ? "busy" : "free", SLOT_SIZE);
}

void *__real_os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file);
void *__wrap_os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    if (!(prot & MMAP_PROT_EXEC) && size >= SLOT_MIN && size <= SLOT_SIZE - 16) {
        for (int i = 0; i < 2; i++) {
            if (!s_slot[i].base || s_slot[i].busy) continue;
            s_slot[i].busy = true;
            uint8_t *fixed = (uint8_t *)(((uintptr_t)s_slot[i].base + 7) & ~(uintptr_t)7);
            memset(fixed, 0, size);
            return fixed;
        }
    }
    return __real_os_mmap(hint, size, prot, flags, file);
}

void __real_os_munmap(void *addr, size_t size);
void __wrap_os_munmap(void *addr, size_t size)
{
    for (int i = 0; i < 2; i++)
        if (s_slot[i].base && (uint8_t *)addr >= s_slot[i].base && (uint8_t *)addr < s_slot[i].base + SLOT_SIZE) { s_slot[i].busy = false; return; }
    __real_os_munmap(addr, size);
}


// Public entry. A program started from inside another program (sys_run) is nested and already runs under the
// lock; a top-level program (an SSH command or a web request) takes it, so only one runs at a time.
int aot_run(term_t *t, const char *cwd, const char *vpath, int argc, char **argv, io_t *in, io_t *out, io_t *err)
{
    bool nested = P && P->owner == xTaskGetCurrentTaskHandle();
    if (!nested && xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        io_write(err, "busy: another program is running\n", 33);
        return -4;
    }
    int rc = aot_run_nolock(t, cwd, vpath, argc, argv, in, out, err);
    if (!nested) xSemaphoreGive(s_run_lock);
    return rc;
}

// For callers that manage the lock themselves (the web server wants to know when its program really starts).
bool aot_lock(int timeout_ms) { return xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
void aot_unlock(void) { xSemaphoreGive(s_run_lock); }

bool aot_interactive_busy(void) { return s_interactive; }
