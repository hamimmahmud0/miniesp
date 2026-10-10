#include "ssh_server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "auth.h"
#include "creds.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <pthread.h>
#include "esp_pthread.h"
#include "aot_run.h"
#include "shell.h"
#include "term.h"
#include <wolfssh/ssh.h>
#include <wolfssh/internal.h>      // WOLFSSH.widthChar/heightRows: the terminal size (the resize callback API is not built on ESP)

static const char *TAG = "ssh";
#define SSH_PORT 22
#define WORKER_STACK 16384

typedef struct {
    int fd;
    WOLFSSH *ssh;
    term_t *term;
    volatile bool want_shell, want_exec, eof, eof_seen;
    int zero_reads;
    int exit_code;                 // exit status of the exec'd command, reported to the client
    size_t sent;
    byte pre[2048];                 // stdin bytes that arrive together with the exec/shell request
    size_t pre_len;
    size_t rx;                     // stdin bytes received (debug)
    int hard_errs;
    char cmd[200];
    pthread_t worker;
    bool worker_started;
    volatile bool worker_done;
    bool closed;
} session_t;

static WOLFSSH_CTX *s_ctx;

/* Breadcrumbs for the watchdog: where the sshd task is, a heartbeat, and the live session socket. */
static const char *volatile s_where = "idle";
static volatile TickType_t s_beat;
static volatile int s_sess_fd = -1;
static volatile bool s_sess_active;
#define WHERE(w) do { s_where = (w); s_beat = xTaskGetTickCount(); } while (0)
int ssh_session_active(void) { return s_sess_active; }
const char *ssh_where(uint32_t *age_ms) { if (age_ms) *age_ms = (xTaskGetTickCount() - s_beat) * portTICK_PERIOD_MS; return s_where; }
void ssh_kill_session(void) { int fd = s_sess_fd; if (fd >= 0) shutdown(fd, SHUT_RDWR); }

/* ---------- host key (ECDSA P-256 DER embedded at build time from .creds/esp32_hostkey.der) ---------- */
extern const uint8_t hostkey_start[] asm("_binary_hostkey_der_start");
extern const uint8_t hostkey_end[] asm("_binary_hostkey_der_end");

/* ---------- wolfSSH callbacks ---------- */
static int user_auth(byte type, WS_UserAuthData *d, void *ctx)
{
    if (type != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
    if (auth_check(d->username, d->usernameSz, d->sf.password.password, d->sf.password.passwordSz)) {
        ESP_LOGI(TAG, "login ok");
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    ESP_LOGW(TAG, "login failed");
    vTaskDelay(pdMS_TO_TICKS(1500));                                    // slow down guessing
    return WOLFSSH_USERAUTH_INVALID_PASSWORD;
}

static int cb_shell(WOLFSSH_CHANNEL *ch, void *ctx) { ((session_t *)ctx)->want_shell = true; return WS_SUCCESS; }
// scp/sftp use a "subsystem" request. Reject it right away so the client fails fast instead of waiting
// (use `ssh host "cat f" > f` and `ssh host "put f" < f` instead).
static int cb_subsys(WOLFSSH_CHANNEL *ch, void *ctx)
{
    ESP_LOGW(TAG, "subsystem request rejected (no sftp/scp subsystem; use ssh host \"put f\" < f)");
    return 1;
}
static int cb_exec(WOLFSSH_CHANNEL *ch, void *ctx)
{
    session_t *s = ctx;
    const char *c = wolfSSH_ChannelGetSessionCommand(ch);
    strlcpy(s->cmd, c ? c : "", sizeof s->cmd);
    s->want_exec = true;
    return WS_SUCCESS;
}
static int cb_eof(WOLFSSH_CHANNEL *ch, void *ctx)
{
    session_t *s = ctx;
    s->eof = true;      // EOF packet parsed; data queued before it may still be unread, so don't flag stdin yet
    return WS_SUCCESS;
}

/* ---------- session IO ---------- */
// Runs as a pthread: WAMR's platform layer calls pthread_self(), which ESP-IDF only supports for pthreads.
static void *worker_task(void *arg)
{
    session_t *s = arg;
    term_set_current(s->term);
    if (s->want_shell) shell_run(s->term, CRED_SSH_USER);
    else s->exit_code = shell_exec(s->term, s->cmd);
    s->worker_done = true;
    return NULL;
}

static bool start_worker(session_t *s)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 0;
    cfg.stack_size = WORKER_STACK;
    cfg.thread_name = "sh";
    cfg.prio = 5;
    if (esp_pthread_set_cfg(&cfg) != ESP_OK) return false;
    if (pthread_create(&s->worker, NULL, worker_task, s) != 0) return false;
    pthread_detach(s->worker);
    s->worker_started = true;
    return true;
}

// Read whatever the client has sent and queue it for the shell. Returns <0 when the session is over.
static int pump_in(session_t *s)
{
    byte buf[256];
    for (;;) {
        // Flow control: only pull from the wire while the terminal buffer has room, so uploads
        // (ssh host "put f" < file) are never dropped; unread data waits in TCP/wolfSSH.
        if (xStreamBufferSpacesAvailable(s->term->in) < sizeof buf) { vTaskDelay(pdMS_TO_TICKS(2)); return 0; }
        int r = wolfSSH_stream_read(s->ssh, buf, sizeof buf);
        if (r > 0) {
            s->zero_reads = 0; s->hard_errs = 0; s->rx += r;
            term_push_in(s->term, buf, r);
            if (!s->term->pty) return 0;                      // exec: one chunk per call, the command pulls more as needed
            continue;
        }
        int err = r < 0 ? r : wolfSSH_get_error(s->ssh);      // the return value carries the code
        if (err == WS_WANT_READ || err == WS_EXTDATA || err == WS_WINDOW_FULL || err == WS_REKEYING) return 0;
        if (err == WS_CHAN_RXD) continue;
        if (err == WS_SUCCESS) {                               // 0 bytes: nothing usable right now
            if (s->term && !s->term->pty) {
                if (s->eof) { s->term->eof = true; s->eof_seen = true; ESP_LOGD(TAG, "stdin EOF (0-read) after %u bytes", (unsigned)s->rx); }   // EOF seen and nothing left to read
                return 0;
            }
            if (++s->zero_reads > 400) { ESP_LOGW(TAG, "peer sends only empty reads: closing"); return -1; }   // interactive: persistent 0 means the peer is gone
            return 0;
        }
        if (err == WS_EOF) {
            if (s->term && !s->term->pty) { s->term->eof = true; s->eof_seen = true; ESP_LOGD(TAG, "stdin EOF (WS_EOF) after %u bytes", (unsigned)s->rx); return 0; }   // stdin closed: keep running
            return -1;
        }
        if (err == WS_CHANNEL_CLOSED) return -1;
        if (err == WS_FATAL_ERROR && ++s->hard_errs <= 200) {  // seen at window-refill points on a non-blocking socket: retry
            vTaskDelay(pdMS_TO_TICKS(2));
            return 0;
        }
        ESP_LOGW(TAG, "read error %d (%s)", err, wolfSSH_ErrorToName(err));
        return -1;
    }
}

// Read and process whatever the client has sent. wolfSSH_stream_read cannot be used for this on its own:
// it refuses to touch the socket after the client's EOF, and under heavy output it can fail while a window
// update is being processed. wolfSSH_worker always processes incoming packets; when it reports that channel
// data arrived, that data is then fetched with pump_in. Returns <0 when the session is over.
static int service_wire(session_t *s)
{
    // An exec session still reading stdin must use wolfSSH_stream_read: the worker would also process the
    // client's EOF, after which stream_read refuses to hand over data that is still buffered (truncated uploads).
    if (s->term && !s->term->pty && !(s->eof || s->eof_seen)) return pump_in(s);
    word32 ch = 0;
    int r = wolfSSH_worker(s->ssh, &ch);
    int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
    if (e == WS_CHAN_RXD) return (s->eof || s->eof_seen) ? 0 : pump_in(s);
    if (r == WS_SUCCESS || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_EXTDATA || e == WS_WANT_WRITE) { s->hard_errs = 0; return 0; }
    if (e == WS_SOCKET_ERROR_E || e == WS_EOF || e == WS_CHANNEL_CLOSED) return -1;
    if (e == WS_FATAL_ERROR && ++s->hard_errs <= 200) { vTaskDelay(pdMS_TO_TICKS(2)); return 0; }
    ESP_LOGW(TAG, "wire error %d (%s)", e, wolfSSH_ErrorToName(e));
    return -1;
}

static int send_all(session_t *s, const byte *p, int n)
{
    TickType_t stall0 = xTaskGetTickCount();
    while (n > 0) {
        s_beat = xTaskGetTickCount();
        int w = wolfSSH_stream_send(s->ssh, (byte *)p, n);
        if (w > 0) { p += w; n -= w; stall0 = xTaskGetTickCount(); continue; }
        if ((xTaskGetTickCount() - stall0) > pdMS_TO_TICKS(30000)) { ESP_LOGW(TAG, "client not accepting data for 30 s: closing"); return -1; }
        int err = wolfSSH_get_error(s->ssh);
        if (err == WS_WANT_WRITE || err == WS_WANT_READ || err == WS_WINDOW_FULL || err == WS_REKEYING || err == WS_CHAN_RXD) {
            vTaskDelay(pdMS_TO_TICKS(5));
            if (service_wire(s) < 0) return -1; // window updates arrive on the read side
            continue;
        }
        ESP_LOGW(TAG, "send error %d (%s)", err, wolfSSH_ErrorToName(err));
        return -1;
    }
    return 0;
}

static int flush_out(session_t *s)
{
    byte buf[512];
    size_t n;
    while ((n = term_pop_out(s->term, buf, sizeof buf)) > 0) {
        if (send_all(s, buf, (int)n) < 0) return -1;
        s->sent += n;
    }
    return 0;
}

// Has the TCP peer gone away? (Non-destructive: peeks at the socket.) Needed because a running command
// is not reading from the wire, so a vanished client would otherwise hold the only session slot.
static bool peer_gone(int fd)
{
    char c;
    int r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return true;                                      // FIN received
    if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) return true;   // reset / timeout
    return false;
}

static bool wait_fd(int fd, bool for_write, int ms)
{
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv) > 0;
}

static void run_session(int fd)
{
    s_sess_fd = fd; s_sess_active = true; WHERE("session-start");
    bool leak = false;                                   // set when a stuck worker still references the session
    session_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return; }
    s->fd = fd;
    s->ssh = wolfSSH_new(s_ctx);
    if (!s->ssh) { ESP_LOGE(TAG, "wolfSSH_new failed"); close(fd); free(s); return; }
    wolfSSH_set_fd(s->ssh, fd);
    wolfSSH_SetChannelReqCtx(s->ssh, s);
    wolfSSH_SetChannelEofCtx(s->ssh, s);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Detect a client that vanished without closing (power loss, cable): probe after 20 s, give up after ~35 s.
    int ka = 1, idle = 20, intvl = 5, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);

    /* key exchange + authentication */
    TickType_t t0 = xTaskGetTickCount();
    int ret;
    WHERE("handshake");
    while ((ret = wolfSSH_accept(s->ssh)) != WS_SUCCESS) {
        s_beat = xTaskGetTickCount();
        int err = wolfSSH_get_error(s->ssh);
        if (err == WS_WANT_READ) wait_fd(fd, false, 200);
        else if (err == WS_WANT_WRITE) wait_fd(fd, true, 200);
        else if (err == WS_CHAN_RXD || err == WS_REKEYING) continue;
        else { ESP_LOGW(TAG, "accept failed: %d (%s)", err, wolfSSH_ErrorToName(err)); goto out; }
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(20000)) { ESP_LOGW(TAG, "login timeout"); goto out; }
    }
    ESP_LOGI(TAG, "session established");
    TickType_t t_est = xTaskGetTickCount(), last_probe = t_est;

    s->term = NULL;
    WHERE("await-request");
    for (;;) {
        s_beat = xTaskGetTickCount();                       // heartbeat: the watchdog flags a session whose loop stops turning
        // Wait for a shell/exec request.
        if (!s->term && !s->want_shell && !s->want_exec && (xTaskGetTickCount() - t_est) > pdMS_TO_TICKS(15000)) {
            ESP_LOGW(TAG, "no shell or command requested within 15 s: closing");   // do not hold the only session slot
            goto out;
        }
        if (!s->term && (s->want_shell || s->want_exec)) {
            s->term = term_new(s->want_shell);
            if (!s->term) goto out;
            if (s->pre_len) { term_push_in(s->term, s->pre, s->pre_len); s->pre_len = 0; }   // replay early stdin
            if (!start_worker(s)) { ESP_LOGE(TAG, "no memory for shell"); goto out; }
        }
        if (s->term && (xTaskGetTickCount() - last_probe) > pdMS_TO_TICKS(500)) {
            last_probe = xTaskGetTickCount();
            if (!s->worker_done && peer_gone(fd)) {
                ESP_LOGW(TAG, "client disconnected while a command was running");
                s->term->closed = true;
                if (!s->want_shell) aot_kill();                  // stop the orphaned program (shells end when the terminal closes)
                break;
            }
        }
        if (s->term) {
            word32 cw = s->ssh->widthChar, ch = s->ssh->heightRows;                  // from pty-req / window-change
            if (cw >= 20 && cw <= 300 && ch >= 5 && ch <= 100) { s->term->cols = (int)cw; s->term->rows = (int)ch; }
            if (flush_out(s) < 0) { ESP_LOGW(TAG, "flush failed"); break; }
            if (s->worker_done) { flush_out(s); ESP_LOGI(TAG, "worker done, %u bytes sent", (unsigned)s->sent); break; }
        }
        if (s->eof_seen) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }     // nothing more to read; wait for the command to finish
        // Exec sessions read from the wire only when the command wants stdin: wolfSSH answers a client
        // channel-EOF with its own EOF, after which our output would be discarded by the client.
        if (s->term && !s->term->pty && !s->term->want_input) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (wait_fd(fd, false, 10) || !s->term) {
            if (!s->term) {                        // handshake done, channel requests still in flight
                byte b[128];
                int r = wolfSSH_stream_read(s->ssh, b, sizeof b);
                if (r > 0) {                       // keep any input that rode along with the request
                    size_t room = sizeof s->pre - s->pre_len;
                    size_t k = (size_t)r < room ? (size_t)r : room;
                    memcpy(s->pre + s->pre_len, b, k);
                    s->pre_len += k;
                }
                if (r < 0) {
                    int err = wolfSSH_get_error(s->ssh);
                    if ((err == WS_EOF || err == WS_CHANNEL_CLOSED) && (s->want_shell || s->want_exec)) { s->eof = true; continue; }
                    if (err == WS_EOF || err == WS_CHANNEL_CLOSED) { ESP_LOGW(TAG, "closed before request (%d)", err); break; }
                    if (err != WS_WANT_READ && err != WS_CHAN_RXD && err != WS_EXTDATA && err != WS_WINDOW_FULL && err != WS_REKEYING) break;
                    if (err == WS_WANT_READ) vTaskDelay(pdMS_TO_TICKS(10));
                }
                continue;
            }
            if (service_wire(s) < 0) { ESP_LOGI(TAG, "client ended session"); break; }
        }
    }

out:
    WHERE("closing");
    if (s->term) s->term->closed = true;           // unblocks the shell if the client vanished
    if (s->worker_started && !s->worker_done) {    // give the worker a moment to notice
        for (int i = 0; i < 100 && !s->worker_done; i++) {
            if (s->worker_done) break;
            if (s->term) { /* drop output so a blocked writer can't hang */ unsigned char d[64]; while (term_pop_out(s->term, d, sizeof d)) ; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s->worker_done) { aot_kill(); for (int i = 0; i < 100 && !s->worker_done; i++) vTaskDelay(pdMS_TO_TICKS(20)); }
        if (!s->worker_done) { ESP_LOGW(TAG, "worker still running at session end: leaking its session state"); leak = true; }
    }
    if (s->want_exec) wolfSSH_SetExitStatus(s->ssh, (word32)(s->exit_code & 0xff));
    // Finish the shutdown properly: on a non-blocking socket it can return "would block" while the tail of the
    // output is still queued, so retry (servicing window updates from the client) until it completes.
    for (int i = 0; i < 300; i++) {
        int r = wolfSSH_shutdown(s->ssh);
        int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
        if (r == WS_SUCCESS && e == WS_SUCCESS) break;
        if (e == WS_WANT_WRITE || e == WS_WANT_READ || e == WS_CHAN_RXD || e == WS_WINDOW_FULL || e == WS_REKEYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            (void)service_wire(s);                                      // let window updates in
            continue;
        }
        break;                                                  // done, or the peer is gone
    }
    WHERE("tcp-drain");
    // Graceful TCP close: half-close, then drain whatever the client still sends. Closing with unread
    // data pending makes lwIP send RST, which can discard output the client has not received yet.
    shutdown(fd, SHUT_WR);
    for (int i = 0; i < 50; i++) {
        char junk[128];
        int n = recv(fd, junk, sizeof junk, MSG_DONTWAIT);
        if (n == 0) break;                          // client closed its side
        if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    wolfSSH_free(s->ssh);
    close(fd);
    term_set_current(NULL);
    if (!leak) { term_free(s->term); free(s); }          // a stuck worker may still touch them
    s_sess_active = false; s_sess_fd = -1; WHERE("idle");
    ESP_LOGI(TAG, "session closed");
}

static volatile bool s_ssh_run;                  // should the server be accepting connections?
static volatile int s_ssh_listen = -1;
static TaskHandle_t s_ssh_task;

int ssh_server_running(void) { return s_ssh_run; }

// Stop accepting connections right away (the listening socket is closed here); a session in progress continues.
void ssh_server_stop(void)
{
    s_ssh_run = false;
    int ls = s_ssh_listen;
    s_ssh_listen = -1;
    if (ls >= 0) close(ls);
}

static int open_listener(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SSH_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { close(ls); return -1; }
    return ls;
}

// One permanent task: it owns the wolfSSH setup and serves sessions. The run flag decides whether it listens.
static void server_task(void *arg)
{
    if (wolfSSH_Init() != WS_SUCCESS) { ESP_LOGE(TAG, "wolfSSH_Init failed"); vTaskDelete(NULL); }
    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) { ESP_LOGE(TAG, "ctx alloc failed"); vTaskDelete(NULL); }
    wolfSSH_SetUserAuth(s_ctx, user_auth);
    wolfSSH_CTX_SetChannelReqShellCb(s_ctx, cb_shell);
    wolfSSH_CTX_SetChannelReqExecCb(s_ctx, cb_exec);
    wolfSSH_CTX_SetChannelReqSubsysCb(s_ctx, cb_subsys);
    wolfSSH_CTX_SetChannelEofCb(s_ctx, cb_eof);
    wolfSSH_CTX_SetBanner(s_ctx, "miniesp\n");
    wolfSSH_CTX_SetWindowPacketSize(s_ctx, 4096, 1400);
    int ksz = hostkey_end - hostkey_start;
    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, hostkey_start, ksz, WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
        ESP_LOGE(TAG, "host key unusable (%d bytes)", ksz);
        vTaskDelete(NULL);
    }
    for (;;) {
        if (!s_ssh_run) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        int ls = open_listener();
        if (ls < 0) { ESP_LOGE(TAG, "bind/listen failed"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        s_ssh_listen = ls;
        ESP_LOGI(TAG, "listening on port %d (user '%s')", SSH_PORT, CRED_SSH_USER);
        WHERE("accept-wait");
        while (s_ssh_run && s_ssh_listen == ls) {              // stop() closes the listener; start() makes the outer loop open a new one
            int fd = accept(ls, NULL, NULL);
            if (fd < 0) { if (!s_ssh_run || s_ssh_listen != ls) break; vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            run_session(fd);
            WHERE("accept-wait");
            ESP_LOGI(TAG, "stack high-water: %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        int cur = s_ssh_listen;                  // stop() normally closed it already
        if (cur >= 0) { s_ssh_listen = -1; close(cur); }
        ESP_LOGI(TAG, "sshd not listening");
    }
}

void ssh_server_start(void)
{
    s_ssh_run = true;
    if (!s_ssh_task) xTaskCreatePinnedToCore(server_task, "sshd", 12 * 1024, NULL, 5, &s_ssh_task, 0);
}
