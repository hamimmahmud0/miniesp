// Web hosting: a small HTTP/1.1 server.
//   GET /path             static file from /www  (/ and directories serve index.html)
//   GET /cgi-bin/NAME?a+b runs /www/cgi-bin/NAME.aot with arguments "a" "b"; its stdout is the response.
//                         The program may start with "Content-Type: xxx" and a blank line (CGI style);
//                         otherwise JSON is detected by a leading { or [, everything else is text/plain.
#include "www.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aot_run.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "fs.h"
#include "hdrfilter.h"
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "io.h"
#include "kv.h"
#include "term.h"
#include "wifi_mgr.h"

static const char *TAG = "www";
static httpd_handle_t s_srv;
static volatile uint32_t s_reqs;

uint32_t www_requests(void) { return s_reqs; }

static const struct { const char *ext, *type; } MIME[] = {
    { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" }, { ".css", "text/css" },
    { ".js", "application/javascript" }, { ".json", "application/json" }, { ".txt", "text/plain; charset=utf-8" },
    { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
    { ".gif", "image/gif" }, { ".ico", "image/x-icon" }, { ".csv", "text/csv" }, { ".log", "text/plain; charset=utf-8" }, { ".xml", "application/xml" },
    { ".wasm", "application/wasm" }, { ".woff2", "font/woff2" },
};

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot)
        for (size_t i = 0; i < sizeof MIME / sizeof *MIME; i++)
            if (!strcasecmp(dot, MIME[i].ext)) return MIME[i].type;
    return "application/octet-stream";
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* ---------------- static files ---------------- */
static const char DEFAULT_PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>miniesp</title><style>body{font:16px system-ui;max-width:40em;margin:2em auto;padding:0 1em;color:#1f2937}"
    "h1{font-family:ui-monospace,Menlo,Consolas,monospace;display:flex;align-items:center;gap:12px}h1 b{color:#0f766e}"
    "code{background:#e2e8f0;padding:1px 6px;border-radius:4px}@media(prefers-color-scheme:dark){body{background:#111827;color:#e5e7eb}"
    "h1 b{color:#5eead4}code{background:#2a3547}}</style><body>"
    "<h1><svg width=44 height=44 viewBox='0 0 64 64'><g fill='#94a3b8'><rect x=21 y=3 width=4 height=9 rx=2 /><rect x=30 y=3 width=4 height=9 rx=2 />"
    "<rect x=39 y=3 width=4 height=9 rx=2 /><rect x=21 y=52 width=4 height=9 rx=2 /><rect x=30 y=52 width=4 height=9 rx=2 /><rect x=39 y=52 width=4 height=9 rx=2 />"
    "<rect x=3 y=21 width=9 height=4 rx=2 /><rect x=3 y=30 width=9 height=4 rx=2 /><rect x=3 y=39 width=9 height=4 rx=2 />"
    "<rect x=52 y=21 width=9 height=4 rx=2 /><rect x=52 y=30 width=9 height=4 rx=2 /><rect x=52 y=39 width=9 height=4 rx=2 /></g>"
    "<rect x=12 y=12 width=40 height=40 rx=7 fill='#0f766e' /><path d='M22 23l11 9-11 9' fill=none stroke=#fff stroke-width=4.5 />"
    "<rect x=35 y=39 width=11 height=4.5 fill='#f97316' /></svg><span>mini<b>esp</b></span></h1>"
    "<p>The web server works, but there is no <code>/www/index.html</code> yet.</p>"
    "<p>Put your site in <code>/www</code> (for example <code>ssh esp@host \"put /www/index.html\" &lt; index.html</code>) "
    "and programs in <code>/www/cgi-bin</code> to serve dynamic data at <code>/cgi-bin/NAME</code>.</p>";

static esp_err_t serve_file(httpd_req_t *req, const char *rel)
{
    char v[200], host[300];
    fs_resolve(WWW_ROOT, rel, v, sizeof v);
    size_t rl = strlen(WWW_ROOT);
    if (strncmp(v, WWW_ROOT, rl) || (v[rl] && v[rl] != '/')) return send_text(req, "403 Forbidden", "forbidden\n");
    fs_host_path(v, host, sizeof host);
    struct stat st;
    if (!stat(host, &st) && S_ISDIR(st.st_mode)) {
        size_t rn = strcspn(rel, "?");
        if (rn && rel[rn - 1] != '/') {                  // /tank -> /tank/ : the page's relative links (tank.css, ...) need the slash
            char loc[160]; snprintf(loc, sizeof loc, "/%.*s/", (int)(rn < 150 ? rn : 150), rel);
            httpd_resp_set_status(req, "301 Moved Permanently");
            httpd_resp_set_hdr(req, "Location", loc);
            return httpd_resp_send(req, NULL, 0);
        }
        strlcat(host, "/index.html", sizeof host); strlcat(v, "/index.html", sizeof v);
    }
    FILE *f = fopen(host, "rb");
    if (!f) {
        if (!strcmp(rel, "") || !strcmp(rel, "index.html")) { httpd_resp_set_type(req, "text/html"); return httpd_resp_send(req, DEFAULT_PAGE, sizeof DEFAULT_PAGE - 1); }
        return send_text(req, "404 Not Found", "not found\n");
    }
    httpd_resp_set_type(req, mime_for(v));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char *buf = malloc(1400);
    if (!buf) { fclose(f); return send_text(req, "500 Internal Server Error", "out of memory\n"); }
    esp_err_t e = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, 1400, f)) > 0)
        if ((e = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    free(buf);
    fclose(f);
    if (e == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

/* ---------------- CGI ---------------- */
#define CGI_WAIT_MS 1200                  // how long a request waits for the runtime when another program is running
                                          // (short on purpose: the HTTP server has a single task, so waiting stalls other requests)
#define CGI_TIMEOUT_MS 8000               // how long a started program may run
typedef struct {
    char path[100];
    int argc;
    char *argv[26];
    char argbuf[600];
    io_t *out, *err;
    int rc;
    SemaphoreHandle_t done;
    volatile bool abandoned;          // the request gave up: the worker frees the job itself
    volatile bool started;            // the program is really running (it holds the run lock)
} cgi_job_t;

static void job_free(cgi_job_t *j)
{
    io_close(j->out); io_close(j->err);
    vSemaphoreDelete(j->done);
    free(j);
}

static void *cgi_worker(void *arg)
{
    cgi_job_t *j = arg;
    term_t *t = term_new(false);
    io_t in = { .kind = IO_MEM };                      // empty stdin
    if (!t) j->rc = -5;
    else if (aot_interactive_busy() || !aot_lock(CGI_WAIT_MS)) j->rc = -4;   // an interactive program may hold it for minutes: fail fast; otherwise wait briefly
    else {
        if (!j->abandoned) {                           // (a request that already gave up must not start a program)
            j->started = true;
            j->rc = aot_run_nolock(t, FS_HOME, j->path, j->argc, j->argv, &in, j->out, j->err);
        } else j->rc = -4;
        aot_unlock();
    }
    term_free(t);
    bool drop = j->abandoned;
    if (!drop) xSemaphoreGive(j->done);
    if (drop) job_free(j);
    return NULL;
}

static esp_err_t serve_cgi(httpd_req_t *req, const char *name, const char *query)
{
    for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return send_text(req, "400 Bad Request", "bad program name\n");
    if (!*name) return send_text(req, "404 Not Found", "no program given\n");

    cgi_job_t *j = calloc(1, sizeof *j);
    if (!j) return send_text(req, "500 Internal Server Error", "out of memory\n");
    snprintf(j->path, sizeof j->path, WWW_ROOT "/cgi-bin/%s.aot", name);
    char host[300], v[120]; struct stat st;
    snprintf(v, sizeof v, "%s", j->path); fs_host_path(v, host, sizeof host);
    if (stat(host, &st)) { free(j); return send_text(req, "404 Not Found", "no such program\n"); }

    // arguments from the query string: words separated by '+' or '&', limited character set
    j->argv[0] = j->argbuf; strlcpy(j->argbuf, name, sizeof j->argbuf);
    j->argc = 1;
    size_t pos = strlen(name) + 1;
    for (const char *p = query; *p && j->argc < 25;) {
        size_t l = strcspn(p, "+&");
        if (l) {
            if (pos + l + 1 >= sizeof j->argbuf) { free(j); return send_text(req, "414 URI Too Long", "arguments too long\n"); }
            for (size_t k = 0; k < l; k++)
                if (!isalnum((unsigned char)p[k]) && !strchr("_.,:=@/-", p[k])) { free(j); return send_text(req, "400 Bad Request", "bad argument\n"); }
            memcpy(j->argbuf + pos, p, l); j->argbuf[pos + l] = 0;
            j->argv[j->argc++] = j->argbuf + pos; pos += l + 1;
        }
        p += l; if (*p) p++;
    }
    j->out = io_new_mem(); j->err = io_new_mem();
    j->done = xSemaphoreCreateBinary();
    if (!j->out || !j->err || !j->done) { job_free(j); return send_text(req, "500 Internal Server Error", "out of memory\n"); }

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 0;
    cfg.stack_size = 10240; cfg.thread_name = "cgi"; cfg.prio = 5;
    pthread_t th;
    if (esp_pthread_set_cfg(&cfg) != ESP_OK || pthread_create(&th, NULL, cgi_worker, j) != 0) { job_free(j); return send_text(req, "500 Internal Server Error", "cannot start program\n"); }
    pthread_detach(th);

    if (xSemaphoreTake(j->done, pdMS_TO_TICKS(CGI_WAIT_MS + CGI_TIMEOUT_MS)) != pdTRUE) {
        if (j->started) aot_kill();                    // our own runaway program: stop it (never someone else's)
        if (xSemaphoreTake(j->done, pdMS_TO_TICKS(2000)) != pdTRUE) { j->abandoned = true; return send_text(req, "504 Gateway Timeout", "program timed out\n"); }
    }
    esp_err_t e;
    if (j->rc == -4) { httpd_resp_set_hdr(req, "Retry-After", "2"); e = send_text(req, "503 Service Unavailable", "busy: another program (an SSH session or service) is using the runtime, try again\n"); }
    else if (j->rc < 0 && !j->out->len) {
        char m[200]; size_t el = j->err->len < 150 ? j->err->len : 150;           // include the loader's reason (e.g. not enough RAM)
        int ml = snprintf(m, sizeof m, "program could not be run: %.*s\n", (int)el, el ? (const char *)j->err->mem : "no details");
        (void)ml;
        e = send_text(req, "500 Internal Server Error", m);
    }
    else {
        const char *body = (const char *)j->out->mem; size_t len = j->out->len;
        const char *type = NULL; char ctype[64];
        if (len > 13 && !strncasecmp(body, "Content-Type:", 13)) {            // CGI header block
            const char *nl = memchr(body, '\n', len);
            if (nl) {
                size_t tl = (size_t)(nl - body) - 13; if (tl >= sizeof ctype) tl = sizeof ctype - 1;
                memcpy(ctype, body + 13, tl); ctype[tl] = 0;
                char *c = ctype; while (*c == ' ') c++;
                for (char *q = c + strlen(c); q > c && (q[-1] == '\r' || q[-1] == ' '); *--q = 0) {}
                memmove(ctype, c, strlen(c) + 1);
                type = ctype;
                body = nl + 1; len -= (size_t)(nl + 1 - (const char *)j->out->mem);
                if (len && *body == '\r') { body++; len--; }
                if (len && *body == '\n') { body++; len--; }
            }
        }
        if (!type) type = (len && (body[0] == '{' || body[0] == '[')) ? "application/json" : "text/plain; charset=utf-8";
        httpd_resp_set_type(req, type);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        e = httpd_resp_send(req, body, (ssize_t)len);
    }
    job_free(j);
    return e;
}

/* ---------------- dispatcher ---------------- */
static esp_err_t handler(httpd_req_t *req)
{
    s_reqs++;
    char uri[200];
    strlcpy(uri, req->uri, sizeof uri);
    char *q = strchr(uri, '?');
    const char *query = "";
    if (q) { *q = 0; query = q + 1; }
    if (strstr(uri, "%")) return send_text(req, "400 Bad Request", "percent-encoding is not supported\n");
    if (!strncmp(uri, "/cgi-bin/", 9)) return serve_cgi(req, uri + 9, query);
    return serve_file(req, uri[0] == '/' ? uri + 1 : uri);
}

int www_running(void) { return s_srv != NULL; }

int www_port(void)                                   // kv "www.port" = 1..65535 (not 22 = SSH); anything else means 80
{
    char b[8]; int n = kv_get("www.port", b, sizeof b - 1);
    if (n <= 0) return 80;
    b[n] = 0;
    int p = atoi(b);
    return p >= 1 && p <= 65535 && p != 22 ? p : 80;
}

// Strip Cookie/Referer header lines from every connection (see hdrfilter.h): a big cookie jar must not stop a page loading.
#define HF_SOCKS 4                      // >= cfg.max_open_sockets; lwIP socket numbers are offset, so slots are looked up by fd
static struct { int fd; struct hf f; } s_hf[HF_SOCKS];

static struct hf *hf_slot(int fd)
{
    for (int i = 0; i < HF_SOCKS; i++) if (s_hf[i].fd == fd) return &s_hf[i].f;
    return NULL;
}

static int hf_recv(httpd_handle_t hd, int fd, char *buf, size_t len, int flags)
{
    struct hf *f = hf_slot(fd);
    if (!f || len <= HF_HOLD) { int r = recv(fd, buf, len, flags); return r < 0 ? (errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL) : r; }
    for (;;) {
        int r = recv(fd, buf + HF_HOLD, len - HF_HOLD, flags);
        if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? HTTPD_SOCK_ERR_TIMEOUT : errno == EINVAL || errno == EBADF ? HTTPD_SOCK_ERR_INVALID : HTTPD_SOCK_ERR_FAIL;
        if (r == 0) return 0;
        size_t o = hf_run(f, buf, HF_HOLD, (size_t)r);
        if (o > 0) return (int)o;       // everything was dropped or held back: read on
    }
}

static esp_err_t hf_open(httpd_handle_t hd, int fd)
{
    struct hf *f = hf_slot(fd);
    for (int i = 0; !f && i < HF_SOCKS; i++) if (s_hf[i].fd <= 0) { s_hf[i].fd = fd; f = &s_hf[i].f; }
    if (f) { hf_init(f); httpd_sess_set_recv_override(hd, fd, hf_recv); }
    return ESP_OK;
}

static void hf_close(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < HF_SOCKS; i++) if (s_hf[i].fd == fd) s_hf[i].fd = 0;
    close(fd);
}

void www_stop(void)
{
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; ESP_LOGI(TAG, "web server stopped"); }
}

void www_start(void)
{
    if (s_srv) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = (uint16_t)www_port();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;
    cfg.max_uri_handlers = 2;
    cfg.max_open_sockets = 3;
    cfg.stack_size = 5120;
    cfg.lru_purge_enable = true;
    cfg.open_fn = hf_open;
    cfg.close_fn = hf_close;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }
    httpd_uri_t u = { .uri = "/*", .method = HTTP_GET, .handler = handler };
    httpd_register_uri_handler(s_srv, &u);
    ESP_LOGI(TAG, "web server on port %d, root %s", www_port(), WWW_ROOT);
}
