// Dashboard + API server (core 0). Serves the water-tank pages embedded in the firmware, the history files from /lfs/tank, state.json from RAM,
// and the same API the pages used with the miniesp OS: GET /cgi-bin/wtms?web+SUB+TOKEN+args (the words are separated by + or &).
#include "web.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "compat.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "fsx.h"
#include "hdrfilter.h"
#include "ota.h"
#include "wtms_core.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;

#define ASSET(sym) extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); extern const uint8_t sym##_end[] asm("_binary_" #sym "_end");
ASSET(index_html) ASSET(config_html) ASSET(login_html) ASSET(tank_css) ASSET(tank_js) ASSET(chart_js) ASSET(style_css) ASSET(ui_js)
static const struct { const char *path, *type; const uint8_t *s, *e; } ASSETS[] = {
    { "/tank/", "text/html; charset=utf-8", index_html_start, index_html_end },
    { "/tank/index.html", "text/html; charset=utf-8", index_html_start, index_html_end },
    { "/tank/config.html", "text/html; charset=utf-8", config_html_start, config_html_end },
    { "/tank/login.html", "text/html; charset=utf-8", login_html_start, login_html_end },
    { "/tank/tank.css", "text/css", tank_css_start, tank_css_end },
    { "/tank/tank.js", "application/javascript", tank_js_start, tank_js_end },
    { "/tank/chart.js", "application/javascript", chart_js_start, chart_js_end },
    { "/style.css", "text/css", style_css_start, style_css_end },
    { "/ui.js", "application/javascript", ui_js_start, ui_js_end },
};

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_file(httpd_req_t *req, const char *name)
{
    char path[96];
    snprintf(path, sizeof path, FS_BASE "/tank/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return send_text(req, "404 Not Found", "not found\n");
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *buf = malloc(1024);
    if (!buf) { fclose(f); return send_text(req, "500 Internal Server Error", "out of memory\n"); }
    esp_err_t e = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, 1024, f)) > 0) if ((e = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    if (e == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    free(buf); fclose(f);
    return e;
}

static esp_err_t api(httpd_req_t *req, const char *query)
{
    char q[240]; strlcpy(q, query, sizeof q);
    for (char *c = q; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("_.,:=@/-+&", *c))) return send_text(req, "400 Bad Request", "bad characters\n");
    char *argv[26]; int argc = 0;
    argv[argc++] = "wtms";
    for (char *p = q; *p && argc < 25;) {
        argv[argc++] = p;
        while (*p && *p != '+' && *p != '&') p++;
        if (*p) *p++ = 0;
    }
    if (argc < 3 || strcmp(argv[1], "web")) return send_text(req, "400 Bad Request", "usage: /cgi-bin/wtms?web+SUB+TOKEN+args\n");
    char *out = malloc(2048);
    if (!out) return send_text(req, "503 Service Unavailable", "out of memory\n");
    int n = wtms_web(argc, argv, out, 2048);
    const char *body = out; int len = n;
    const char *type = "application/json";
    if (len > 13 && !strncasecmp(body, "Content-Type:", 13)) {                       // CGI-style header block
        const char *nl = memchr(body, '\n', len);
        if (nl) {
            const char *nb = nl + 1; len -= (int)(nb - body); body = nb;
            if (len && *body == '\n') { body++; len--; }
        }
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, body, len);
    free(out);
    return e;
}

static esp_err_t handler(httpd_req_t *req)
{
    char uri[200]; strlcpy(uri, req->uri, sizeof uri);
    char *q = strchr(uri, '?'); const char *query = "";
    if (q) { *q = 0; query = q + 1; }
    if (!strcmp(uri, "/")) { httpd_resp_set_status(req, "302 Found"); httpd_resp_set_hdr(req, "Location", "/tank/"); return httpd_resp_send(req, NULL, 0); }
    if (!strcmp(uri, "/cgi-bin/wtms")) return api(req, query);
    if (!strcmp(uri, "/tank/state.json")) {
        char js[800]; int n = compat_state_json(js, sizeof js);
        httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, n ? js : "{}", n ? n : 2);
    }
    for (unsigned i = 0; i < sizeof ASSETS / sizeof *ASSETS; i++)
        if (!strcmp(uri, ASSETS[i].path)) {
            httpd_resp_set_type(req, ASSETS[i].type);
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            return httpd_resp_send(req, (const char *)ASSETS[i].s, (ssize_t)(ASSETS[i].e - ASSETS[i].s));
        }
    if (!strncmp(uri, "/tank/", 6) && !strchr(uri + 6, '/') && strstr(uri, ".csv")) return send_file(req, uri + 6);
    return send_text(req, "404 Not Found", "not found\n");
}

// Strip Cookie/Referer request headers before the server parses them (see hdrfilter.h): a big cookie jar must not stop a page loading.
#define HF_SOCKS 4
static struct { int fd; struct hf f; } s_hf[HF_SOCKS];
static struct hf *hf_slot(int fd) { for (int i = 0; i < HF_SOCKS; i++) if (s_hf[i].fd == fd) return &s_hf[i].f; return NULL; }
static int hf_recv(httpd_handle_t hd, int fd, char *buf, size_t len, int flags)
{
    struct hf *f = hf_slot(fd);
    if (!f || len <= HF_HOLD) { int r = recv(fd, buf, len, flags); return r < 0 ? (errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL) : r; }
    for (;;) {
        int r = recv(fd, buf + HF_HOLD, len - HF_HOLD, flags);
        if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? HTTPD_SOCK_ERR_TIMEOUT : errno == EINVAL || errno == EBADF ? HTTPD_SOCK_ERR_INVALID : HTTPD_SOCK_ERR_FAIL;
        if (r == 0) return 0;
        size_t o = hf_run(f, buf, HF_HOLD, (size_t)r);
        if (o > 0) return (int)o;
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

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.core_id = 0;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;
    cfg.max_open_sockets = 4;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.open_fn = hf_open;
    cfg.close_fn = hf_close;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }
    httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_http_handler };
    httpd_register_uri_handler(s_srv, &ota);
    httpd_uri_t u = { .uri = "/*", .method = HTTP_GET, .handler = handler };
    httpd_register_uri_handler(s_srv, &u);
    ESP_LOGI(TAG, "web server on port 80 (core %d)", (int)xPortGetCoreID());
}
