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

typedef struct { WOLFSSH *ssh; int fd; volatile bool want_shell, want_exec; char cmd[40]; int hard_errs; } sess_t;

extern const uint8_t hostkey_start[] asm("_binary_hostkey_der_start");
extern const uint8_t hostkey_end[] asm("_binary_hostkey_der_end");

static int user_auth(byte type, WS_UserAuthData *d, void *ctx)
{
    if (type != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
    if (auth_check(d->username, d->usernameSz, d->sf.password.password, d->sf.password.passwordSz)) { ESP_LOGI(TAG, "login ok"); return WOLFSSH_USERAUTH_SUCCESS; }
    ESP_LOGW(TAG, "login failed");
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
    if (r == WS_SUCCESS || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_EXTDATA || e == WS_WANT_WRITE || e == WS_CHAN_RXD) { s->hard_errs = 0; return 0; }
    if (e == WS_SOCKET_ERROR_E || e == WS_EOF || e == WS_CHANNEL_CLOSED) return -1;
    if (e == WS_FATAL_ERROR && ++s->hard_errs <= 200) { vTaskDelay(pdMS_TO_TICKS(2)); return 0; }
    return -1;
}

static int send_all(sess_t *s, const char *p, int n)
{
    TickType_t t0 = xTaskGetTickCount();
    while (n > 0) {
        int w = wolfSSH_stream_send(s->ssh, (byte *)p, n);
        if (w > 0) { p += w; n -= w; t0 = xTaskGetTickCount(); continue; }
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(20000)) return -1;
        int e = wolfSSH_get_error(s->ssh);
        if (e == WS_WANT_WRITE || e == WS_WANT_READ || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_CHAN_RXD) { vTaskDelay(pdMS_TO_TICKS(5)); if (service(s) < 0) return -1; continue; }
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

static void tail_out(const char *b, int n, void *ctx) { sess_t *s = ctx; if (s->fd >= 0) { if (send_text(s, b, n) < 0) s->fd = -1; } }

// returns 1 when the user asked to quit (q, Ctrl-C, Ctrl-D)
static int read_keys(sess_t *s)
{
    byte b[32];
    int r = wolfSSH_stream_read(s->ssh, b, sizeof b);
    if (r > 0) { for (int i = 0; i < r; i++) if (b[i] == 'q' || b[i] == 'Q' || b[i] == 3 || b[i] == 4) return 1; return 0; }
    int e = r < 0 ? r : wolfSSH_get_error(s->ssh);
    if (e == WS_WANT_READ || e == WS_CHAN_RXD || e == WS_EXTDATA || e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_SUCCESS) return 0;
    if (e == WS_FATAL_ERROR && ++s->hard_errs <= 200) return 0;
    return 1;                                            // EOF / closed / error
}

static void run_session(int fd)
{
    sess_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return; }
    s->fd = fd;
    s->ssh = wolfSSH_new(s_ctx);
    if (!s->ssh) { ESP_LOGE(TAG, "wolfSSH_new failed"); close(fd); free(s); return; }
    wolfSSH_set_fd(s->ssh, fd);
    wolfSSH_SetChannelReqCtx(s->ssh, s);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1, ka = 1, idle = 20, intvl = 5, cnt = 3;
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
            if (k > 0) { if (send_text(s, b, k) < 0) break; continue; }
            if (wait_fd(fd, false, 200)) { if (service(s) < 0) break; }
            if (read_keys(s)) break;
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
    wolfSSH_free(s->ssh);
    close(fd);
    free(s);
    ESP_LOGI(TAG, "session closed");
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
