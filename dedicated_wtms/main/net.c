#include "net.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "creds.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "net";
static esp_netif_t *s_netif;
static volatile bool s_up;
static char s_host[32] = "wtms";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        esp_wifi_connect();                                  // forever: the controller must come back by itself
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_up = true;
    }
}

static bool host_ok(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 31 || n[0] == '-' || n[l - 1] == '-') return false;
    for (size_t i = 0; i < l; i++) if (!(islower((unsigned char)n[i]) || isdigit((unsigned char)n[i]) || n[i] == '-')) return false;
    return true;
}

static void apply_static(nvs_handle_t h)
{
    char ip[20] = "", mask[20] = "", gw[20] = "", dns[20] = "";
    size_t n;
    n = sizeof ip;   nvs_get_str(h, "ip", ip, &n);
    n = sizeof mask; nvs_get_str(h, "mask", mask, &n);
    n = sizeof gw;   nvs_get_str(h, "gw", gw, &n);
    n = sizeof dns;  nvs_get_str(h, "dns", dns, &n);
    esp_netif_ip_info_t info = { 0 };
    if (esp_netif_str_to_ip4(ip, &info.ip) != ESP_OK || esp_netif_str_to_ip4(mask, &info.netmask) != ESP_OK || esp_netif_str_to_ip4(gw, &info.gw) != ESP_OK) {
        ESP_LOGW(TAG, "static IP settings invalid: using DHCP");
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_set_ip_info(s_netif, &info);
    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(dns, &d.ip.u_addr.ip4) != ESP_OK) d.ip.u_addr.ip4 = info.gw;
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d);
    ESP_LOGI(TAG, "static ip %s mask %s gw %s", ip, mask, gw);
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
        char tmp[32]; size_t n = sizeof tmp;
        if (nvs_get_str(h, "host", tmp, &n) == ESP_OK && host_ok(tmp)) strlcpy(s_host, tmp, sizeof s_host);
        uint8_t st = 0; nvs_get_u8(h, "static", &st);
        if (st) apply_static(h);
        nvs_close(h);
    }
    esp_netif_set_hostname(s_netif, s_host);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    char ssid[33] = CRED_WIFI_SSID, pass[65] = CRED_WIFI_PASS;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {              // a network saved by the miniesp OS wins over the build-time one
        char s[33], p[65]; size_t sn = sizeof s, pn = sizeof p;
        if (nvs_get_str(h, "ssid", s, &sn) == ESP_OK && s[0]) { strlcpy(ssid, s, sizeof ssid); pass[0] = 0; if (nvs_get_str(h, "pass", p, &pn) == ESP_OK) strlcpy(pass, p, sizeof pass); }
        nvs_close(h);
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to '%s' as %s", ssid, s_host);
    esp_wifi_connect();
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_host);
        mdns_instance_name_set("water tank manager");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_service_add(NULL, "_ssh", "_tcp", 22, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", s_host);
    }
}

bool net_up(void) { return s_up; }
const char *net_hostname(void) { return s_host; }
void net_ip(char *out, size_t n)
{
    esp_netif_ip_info_t ip; out[0] = 0;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr) snprintf(out, n, IPSTR, IP2STR(&ip.ip));
}
int net_rssi(void) { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }
