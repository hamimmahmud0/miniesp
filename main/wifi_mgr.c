#include "wifi_mgr.h"
#include <stdio.h>
#include <string.h>
#include "creds.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_netif_ip_addr.h"
#include <ctype.h>
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";
#define STA_TIMEOUT_MS 15000
#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_eg;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static wifi_state_t s_state = WIFI_MODE_NONE_;
static volatile bool s_want_connect;
static char s_ssid[33];
static char s_hostname[32] = NET_DEFAULT_HOSTNAME;
static bool s_mdns_up;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_want_connect) esp_wifi_connect();          // keep retrying while in STA mode
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, GOT_IP_BIT);
    }
}

static bool load_nvs(char *ssid, size_t sn, char *pass, size_t pn)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &sn) == ESP_OK;
    if (ok && nvs_get_str(h, "pass", pass, &pn) != ESP_OK) pass[0] = 0;
    nvs_close(h);
    return ok && ssid[0];
}

bool wifi_mgr_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool wifi_mgr_clear(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    return true;
}


/* ---------------- network settings ---------------- */
bool net_hostname_valid(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 31 || n[0] == '-' || n[l - 1] == '-') return false;
    for (size_t i = 0; i < l; i++)
        if (!(islower((unsigned char)n[i]) || isdigit((unsigned char)n[i]) || n[i] == '-')) return false;
    return true;
}

void net_cfg_load(net_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    strlcpy(c->hostname, NET_DEFAULT_HOSTNAME, sizeof c->hostname);
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof c->hostname;
    char tmp[32];
    if (nvs_get_str(h, "host", tmp, &n) == ESP_OK && net_hostname_valid(tmp)) strlcpy(c->hostname, tmp, sizeof c->hostname);
    uint8_t st = 0; nvs_get_u8(h, "static", &st); c->use_static = st != 0;
    n = sizeof c->ip;   nvs_get_str(h, "ip", c->ip, &n);
    n = sizeof c->mask; nvs_get_str(h, "mask", c->mask, &n);
    n = sizeof c->gw;   nvs_get_str(h, "gw", c->gw, &n);
    n = sizeof c->dns;  nvs_get_str(h, "dns", c->dns, &n);
    nvs_close(h);
}

bool net_cfg_save(const net_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "host", c->hostname) == ESP_OK && nvs_set_u8(h, "static", c->use_static) == ESP_OK
              && nvs_set_str(h, "ip", c->ip) == ESP_OK && nvs_set_str(h, "mask", c->mask) == ESP_OK
              && nvs_set_str(h, "gw", c->gw) == ESP_OK && nvs_set_str(h, "dns", c->dns) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

const char *net_hostname(void) { return s_hostname; }

bool net_apply_hostname(const char *name)
{
    if (!net_hostname_valid(name)) return false;
    strlcpy(s_hostname, name, sizeof s_hostname);
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, s_hostname);
    if (s_ap_netif) esp_netif_set_hostname(s_ap_netif, s_hostname);
    if (s_mdns_up) mdns_hostname_set(s_hostname);
    return true;
}

bool wifi_mgr_mdns_running(void) { return s_mdns_up; }
static void start_mdns(void);
void wifi_mgr_mdns(bool on)
{
    if (on) start_mdns();
    else if (s_mdns_up) { mdns_free(); s_mdns_up = false; ESP_LOGI(TAG, "mDNS stopped"); }
}

static void start_mdns(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mDNS init failed"); return; }
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set("miniesp");
    mdns_service_add(NULL, "_ssh", "_tcp", 22, NULL, 0);
    s_mdns_up = true;
    ESP_LOGI(TAG, "mDNS: %s.local", s_hostname);
}

// Static addressing: stop DHCP and install the configured address. Returns false (-> DHCP) if invalid.
static bool apply_static(const net_cfg_t *c)
{
    esp_netif_ip_info_t info = { 0 };
    esp_ip4_addr_t dns;
    if (esp_netif_str_to_ip4(c->ip, &info.ip) != ESP_OK || esp_netif_str_to_ip4(c->mask, &info.netmask) != ESP_OK
        || esp_netif_str_to_ip4(c->gw, &info.gw) != ESP_OK) {
        ESP_LOGW(TAG, "static IP config invalid -> falling back to DHCP");
        return false;
    }
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_set_ip_info(s_sta_netif, &info);
    if (esp_netif_str_to_ip4(c->dns, &dns) != ESP_OK) dns = info.gw;                // default: gateway as DNS
    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4 = dns;
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d);
    ESP_LOGI(TAG, "static ip %s mask %s gw %s dns " IPSTR, c->ip, c->mask, c->gw, IP2STR(&dns));
    return true;
}

static void start_ap(void)
{
    s_want_connect = false;
    esp_wifi_stop();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CRED_AP_SSID, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(CRED_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    if (strlen(CRED_AP_PASS) >= 8) {
        strlcpy((char *)ap.ap.password, CRED_AP_PASS, sizeof ap.ap.password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "no AP password in .creds/esp32_ap_password: AP is OPEN");
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    strlcpy(s_ssid, CRED_AP_SSID, sizeof s_ssid);
    s_state = WIFI_MODE_AP_;
    start_mdns();
    ESP_LOGW(TAG, "AP mode: ssid '%s', ip 192.168.4.1", s_ssid);
}

void wifi_mgr_start(void)
{
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    net_cfg_t ncfg;
    net_cfg_load(&ncfg);
    strlcpy(s_hostname, ncfg.hostname, sizeof s_hostname);
    esp_netif_set_hostname(s_sta_netif, s_hostname);
    esp_netif_set_hostname(s_ap_netif, s_hostname);
    bool static_ok = ncfg.use_static && apply_static(&ncfg);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    char ssid[33] = "", pass[65] = "";
    if (!load_nvs(ssid, sizeof ssid, pass, sizeof pass)) {
        strlcpy(ssid, CRED_WIFI_SSID, sizeof ssid);
        strlcpy(pass, CRED_WIFI_PASS, sizeof pass);
    }
    (void)static_ok;
    if (!ssid[0]) { ESP_LOGW(TAG, "no SSID configured"); start_ap(); return; }

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    s_want_connect = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to '%s'...", ssid);
    esp_wifi_connect();

    EventBits_t b = xEventGroupWaitBits(s_eg, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(STA_TIMEOUT_MS));
    if (b & GOT_IP_BIT) {
        strlcpy(s_ssid, ssid, sizeof s_ssid);
        s_state = WIFI_MODE_STA_;
        start_mdns();
        return;
    }
    ESP_LOGW(TAG, "could not connect to '%s' within %d ms -> AP mode", ssid, STA_TIMEOUT_MS);
    start_ap();
}

wifi_state_t wifi_mgr_state(void) { return s_state; }
const char *wifi_mgr_ssid(void) { return s_ssid; }

void wifi_mgr_ip(char *out, size_t n)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *nif = s_state == WIFI_MODE_AP_ ? s_ap_netif : s_sta_netif;
    out[0] = 0;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr)
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
}

int wifi_mgr_scan(char *out, size_t n)
{
    // Scanning needs STA; in AP-only mode temporarily use APSTA.
    wifi_mode_t old; esp_wifi_get_mode(&old);
    if (old == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_scan_config_t sc = { 0 };
    int len = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t cnt = 12;
        wifi_ap_record_t recs[12];
        if (esp_wifi_scan_get_ap_records(&cnt, recs) == ESP_OK) {
            for (int i = 0; i < cnt && len < (int)n - 60; i++)
                len += snprintf(out + len, n - len, "%-32s ch%-2d %4d dBm %s\n", recs[i].ssid,
                                recs[i].primary, recs[i].rssi,
                                recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "secured");
        }
    }
    if (old == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
    return len;
}
