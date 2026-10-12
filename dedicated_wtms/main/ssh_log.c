#include "ssh_log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "auth.h"
#include "creds.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "klog.h"
#include <wolfssh/ssh.h>

static const char *TAG = "ssh";
#define SSH_PORT 22
static WOLFSSH_CTX *s_ctx;
static char s_peer[20] = "?";                  // address of the client of the current session

typedef struct { WOLFSSH *ssh; int fd; volatile bool want_shell, want_exec; char cmd[40]; int hard_errs; int odd; char why[80]; char line[340]; int ll; bool color; } sess_t;
static void ended(sess_t *s, const char *why, int code) { if (!s->why[0]) snprintf(s->why, sizeof s->why, "%s%s%s", why, code ? " " : "", code ? wolfSSH_ErrorToName(code) : ""); }

extern const uint8_t hostkey_start[] asm("_binary_hostkey_der_start");
extern const uint8_t hostkey_end[] asm("_binary_hostkey_der_end");

static int user_auth(byte type, WS_UserAuthData *d, void *ctx)
{
    if (type != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
    if (auth_check(d->username, d->usernameSz, d->sf.password.password, d->sf.password.passwordSz)) { ESP_LOGI(TAG, "login ok from %s (user %.*s)", s_peer, (int)d->usernameSz, (const char *)d->username); return WOLFSSH_USERAUTH_SUCCESS; }
    ESP_LOGW(TAG, "login FAILED from %s (user %.*s)", s_peer, (int)d->usernameSz, (const char *)d->username);
    vTaskDelay(pdMS_TO_TICKS(1500));
    return WOLFSSH_USERAUTH_INVALID_PASSWORD;
}
static int cb_shell(WOLFSSH_CHANNEL *ch, void *ctx) { ((sess_t *)ctx)->want_shell = true; return WS_SUCCESS; }
static int cb_exec(WOLFSSH_CHANNEL *ch, void *ctx)
{
    sess_t *s = ctx;
    const char *c = wolfSSH_ChannelGetSessionCommand(ch);
    strlcpy(s->cmd, c ? c : "", sizeof s->cmd);
    s->want_exec = true;
    return WS_SUCCESS;
}
static int cb_subsys(WOLFSSH_CHANNEL *ch, void *ctx) { return 1; }               // no sftp/scp

static bool wait_fd(int fd, bool wr, int ms)
{
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(fd + 1, wr ? NULL : &set, wr ? &set : NULL, NULL, &tv) > 0;
}

// process packets from the client (window updates, requests); <0: the session is over
static int service(sess_t *s)
{
    word32 ch = 0;
    int r = wolfSSH_worker(s->ssh, &ch);
    int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
    if (r == WS_SUCCESS || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_EXTDATA || e == WS_WANT_WRITE || e == WS_CHAN_RXD) { s->hard_errs = 0; s->odd = 0; return 0; }
    if (e == WS_SOCKET_ERROR_E || e == WS_EOF || e == WS_CHANNEL_CLOSED) { ended(s, "client closed the connection:", e); return -1; }
    if (e == WS_FATAL_ERROR && ++s->hard_errs <= 200) { vTaskDelay(pdMS_TO_TICKS(2)); return 0; }
    if (++s->odd <= 50) { vTaskDelay(pdMS_TO_TICKS(5)); return 0; }                 // an unexpected status code: tolerated for a while, it is usually transient
    ended(s, "SSH error:", e);
    return -1;
}

static int send_all(sess_t *s, const char *p, int n)
{
    TickType_t t0 = xTaskGetTickCount();
    while (n > 0) {
        int w = wolfSSH_stream_send(s->ssh, (byte *)p, n);
        if (w > 0) { p += w; n -= w; t0 = xTaskGetTickCount(); continue; }
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(20000)) { ended(s, "client stopped accepting data for 20 s", 0); return -1; }
        int e = wolfSSH_get_error(s->ssh);
        if (e == WS_WANT_WRITE || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_CHAN_RXD) { vTaskDelay(pdMS_TO_TICKS(5)); if (service(s) < 0) return -1; continue; }
        ended(s, "send error:", e);
        return -1;
    }
    return 0;
}

// text -> terminal: every LF becomes CR LF
static int send_text(sess_t *s, const char *p, int n)
{
    char b[256]; int o = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] == '\n') b[o++] = '\r';
        b[o++] = p[i];
        if (o >= (int)sizeof b - 2) { if (send_all(s, b, o) < 0) return -1; o = 0; }
    }
    return o ? send_all(s, b, o) : 0;
}

// ---- colours (terminal sessions only): the level colours the "E (123)" part and, for errors / warnings, the message; every task / tag has its own colour ----
static const char *lvl_color(char c)
{
    switch (c) { case 'E': return "\x1b[1;31m"; case 'W': return "\x1b[33m"; case 'I': return "\x1b[32m"; case 'D': return "\x1b[36m"; default: return "\x1b[90m"; }
}
static const char *tag_color(const char *t, int n)
{
    static const char *const pal[] = { "\x1b[96m", "\x1b[95m", "\x1b[94m", "\x1b[36m", "\x1b[35m", "\x1b[34m", "\x1b[93m", "\x1b[92m" };
    unsigned h = 0; for (int i = 0; i < n; i++) h = h * 31 + (unsigned char)t[i];
    return pal[h % (sizeof pal / sizeof *pal)];
}
// "[2026-10-12 11:20:59] PM I (220280) api: text"  ->  dim stamp, level colour, tag colour, message (red / yellow for E / W)
static int colorize(const char *l, int n, char *out, int cap)
{
    int i = 0, o = 0;
    if (n > 0 && l[0] == '[') { int j = 0; while (j < n && l[j] != ']') j++; if (j + 4 < n && l[j + 1] == ' ' && (l[j + 2] == 'A' || l[j + 2] == 'P') && l[j + 3] == 'M') i = j + 5; }
    if (i + 4 >= n || !strchr("EWIDV", l[i]) || l[i + 1] != ' ' || l[i + 2] != '(') return -1;
    int p = i + 3; while (p < n && l[p] != ')') p++;
    int t0 = p + 2, t1 = t0; while (t1 < n && l[t1] != ':' && l[t1] != ' ' && t1 - t0 < 20) t1++;
    if (p + 2 >= n || l[t1] != ':') return -1;
    const char *lc = lvl_color(l[i]);
    o += snprintf(out + o, cap - o, "\x1b[90m%.*s\x1b[0m%s%.*s\x1b[0m %s%.*s:\x1b[0m", i, l, lc, p + 1 - i, l + i, tag_color(l + t0, t1 - t0), t1 - t0, l + t0);
    int ml = n - (t1 + 1);
    const char *mc = l[i] == 'E' ? "\x1b[91m" : l[i] == 'W' ? "\x1b[33m" : "";
    if (t1 - t0 == 5 && !memcmp(l + t0, "trace", 5)) mc = l[t1 + 2] == 'd' || (l[t1 + 2] == '-' && l[t1 + 3] == '-') ? "\x1b[1;97m" : "\x1b[37m";      // trace: decisions and cycle markers bright, the rest light grey
    o += snprintf(out + o, cap - o, "%s%.*s%s", mc, ml, l + t1 + 1, mc[0] ? "\x1b[0m" : "");
    return o < cap ? o : cap - 1;
}
static int send_line(sess_t *s, const char *l, int n)            // one line without its LF
{
    char out[640];
    int o = s->color ? colorize(l, n, out, sizeof out - 2) : -1;
    if (o < 0) { if (n > (int)sizeof out - 2) n = sizeof out - 2; memcpy(out, l, n); o = n; }
    out[o++] = '\n';
    return send_text(s, out, o);
}
// log text in arbitrary pieces -> whole lines (a partial line waits for its end)
static int send_log(sess_t *s, const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (p[i] == '\n') { if (send_line(s, s->line, s->ll) < 0) return -1; s->ll = 0; }
        else if (s->ll < (int)sizeof s->line - 1) s->line[s->ll++] = p[i];
    }
    return 0;
}

static void tail_out(const char *b, int n, void *ctx) { sess_t *s = ctx; if (s->fd >= 0) { if (send_log(s, b, n) < 0) s->fd = -1; } }

// has the TCP peer gone away? (peeks at the socket)
static bool peer_gone(int fd)
{
    char c;
    int r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return true;
    if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) return true;
    return false;
}

// Wait up to 200 ms for the client; let wolfSSH process what arrived (window updates, keep-alives, requests) and read the keys only when it reports
// channel data (wolfSSH_stream_read called blindly returned "general function failure" after ~80 s of idle). Returns 1 when the session should end:
// q / Ctrl-C / Ctrl-D, the client closed, or the connection is gone.
static int poll_input(sess_t *s)
{
    if (!wait_fd(s->fd, false, 200)) return peer_gone(s->fd) ? (ended(s, "TCP connection lost", 0), 1) : 0;
    word32 ch = 0;
    int r = wolfSSH_worker(s->ssh, &ch);
    int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
    if (e == WS_CHAN_RXD) {
        byte b[32];
        int n = wolfSSH_stream_read(s->ssh, b, sizeof b);
        if (n > 0) { for (int i = 0; i < n; i++) if (b[i] == 'q' || b[i] == 'Q' || b[i] == 3 || b[i] == 4) { snprintf(s->why, sizeof s->why, "user pressed %s", b[i] == 3 ? "Ctrl-C" : b[i] == 4 ? "Ctrl-D" : "q"); return 1; } }
        s->hard_errs = 0; s->odd = 0;
        return 0;
    }
    if (r == WS_SUCCESS || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_EXTDATA || e == WS_WANT_WRITE) { s->hard_errs = 0; s->odd = 0; return 0; }
    if (e == WS_SOCKET_ERROR_E || e == WS_EOF || e == WS_CHANNEL_CLOSED) { ended(s, "client closed the connection:", e); return 1; }
    if (e == WS_FATAL_ERROR && ++s->hard_errs <= 200) { vTaskDelay(pdMS_TO_TICKS(2)); return 0; }
    if (++s->odd <= 50) { vTaskDelay(pdMS_TO_TICKS(5)); return 0; }
    ended(s, "SSH error:", e);
    return 1;
}

static void run_session(int fd)
{
    sess_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return; }
    s->fd = fd;
    { struct sockaddr_in a; socklen_t l = sizeof a; s_peer[0] = 0; if (getpeername(fd, (struct sockaddr *)&a, &l) == 0) inet_ntop(AF_INET, &a.sin_addr, s_peer, sizeof s_peer); }
    ESP_LOGI(TAG, "connection from %s", s_peer);
    TickType_t t_start = xTaskGetTickCount();
    s->ssh = wolfSSH_new(s_ctx);
    if (!s->ssh) { ESP_LOGE(TAG, "wolfSSH_new failed"); close(fd); free(s); return; }
    wolfSSH_set_fd(s->ssh, fd);
    wolfSSH_SetChannelReqCtx(s->ssh, s);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1, ka = 1, idle = 30, intvl = 10, cnt = 9;           // dead-client detection after ~2 min: Wi-Fi at -85 dBm loses probes, a short limit dropped good sessions
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);

    TickType_t t0 = xTaskGetTickCount();
    int ret;
    while ((ret = wolfSSH_accept(s->ssh)) != WS_SUCCESS) {
        int err = wolfSSH_get_error(s->ssh);
        if (err == WS_WANT_READ) wait_fd(fd, false, 200);
        else if (err == WS_WANT_WRITE) wait_fd(fd, true, 200);
        else if (err == WS_CHAN_RXD || err == WS_REKEYING) continue;
        else { ESP_LOGW(TAG, "accept failed: %d", err); goto out; }
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(20000)) { ESP_LOGW(TAG, "login timeout"); goto out; }
    }
    // wait for the shell / exec request (a few packets)
    t0 = xTaskGetTickCount();
    while (!s->want_shell && !s->want_exec) {
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(15000)) goto out;
        if (wait_fd(fd, false, 100) && service(s) < 0) goto out;
        byte b[16]; wolfSSH_stream_read(s->ssh, b, sizeof b);        // keeps early input from blocking the channel
    }
    {
        int n = 40, follow = 1;
        s->color = s->want_shell;                           // an interactive terminal gets colours, `ssh host log N` (pipes, files) stays plain
        if (s->want_exec) {
            follow = 0; n = 100;
            if (!strncmp(s->cmd, "log", 3) || !strncmp(s->cmd, "tail", 4)) { const char *a = strchr(s->cmd, ' '); if (a && atoi(a) > 0) n = atoi(a); }
            else if (!strcmp(s->cmd, "follow")) { follow = 1; n = 40; }
            else { send_text(s, "dedicated_wtms: this SSH server only shows the log:  ssh host  (live)  |  ssh host log [N]\n", 94); goto out; }
        }
        if (follow) send_text(s, "dedicated_wtms log - q or Ctrl-C quits\n", 39);
        klog_tail(n, tail_out, s);
        if (s->fd < 0) goto out;
        uint32_t cur = klog_live_end();
        while (follow) {
            char b[200];
            int k = klog_live_read(&cur, b, sizeof b);
            if (k > 0) { if (send_log(s, b, k) < 0) break; continue; }
            if (poll_input(s)) break;
        }
    }
out:
    for (int i = 0; i < 100; i++) {                                    // finish the SSH shutdown (flushes the tail of the output)
        int r = wolfSSH_shutdown(s->ssh);
        int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
        if (r == WS_SUCCESS && e == WS_SUCCESS) break;
        if (e == WS_WANT_WRITE || e == WS_WANT_READ || e == WS_CHAN_RXD || e == WS_WINDOW_FULL || e == WS_REKEYING) { vTaskDelay(pdMS_TO_TICKS(10)); service(s); continue; }
        break;
    }
    shutdown(fd, SHUT_WR);
    for (int i = 0; i < 25; i++) { char j[64]; int n = recv(fd, j, sizeof j, MSG_DONTWAIT); if (n == 0) break; if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) break; vTaskDelay(pdMS_TO_TICKS(20)); }
    bool was_exec = s->want_exec;
    char why[80]; strlcpy(why, s->why, sizeof why);
    wolfSSH_free(s->ssh);
    close(fd);
    free(s);
    ESP_LOGI(TAG, "session from %s closed after %u s (%s): %s", s_peer, (unsigned)((xTaskGetTickCount() - t_start) / configTICK_RATE_HZ), was_exec ? "log command" : "live view", why[0] ? why : "ended normally");
}

static void server_task(void *arg)
{
    if (wolfSSH_Init() != WS_SUCCESS) { ESP_LOGE(TAG, "wolfSSH_Init failed"); vTaskDelete(NULL); }
    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) { ESP_LOGE(TAG, "ctx alloc failed"); vTaskDelete(NULL); }
    wolfSSH_SetUserAuth(s_ctx, user_auth);
    wolfSSH_CTX_SetChannelReqShellCb(s_ctx, cb_shell);
    wolfSSH_CTX_SetChannelReqExecCb(s_ctx, cb_exec);
    wolfSSH_CTX_SetChannelReqSubsysCb(s_ctx, cb_subsys);
    wolfSSH_CTX_SetBanner(s_ctx, "dedicated_wtms\n");
    wolfSSH_CTX_SetWindowPacketSize(s_ctx, 4096, 1400);
    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, hostkey_start, hostkey_end - hostkey_start, WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) { ESP_LOGE(TAG, "host key unusable"); vTaskDelete(NULL); }
    for (;;) {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        if (ls >= 0) setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SSH_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { if (ls >= 0) close(ls); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        ESP_LOGI(TAG, "log viewer listening on port %d (user '%s')", SSH_PORT, CRED_SSH_USER);
        for (;;) {
            int fd = accept(ls, NULL, NULL);
            if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            run_session(fd);
        }
    }
}

void ssh_log_start(void) { xTaskCreatePinnedToCore(server_task, "sshd", 12 * 1024, NULL, 5, NULL, 0); }
