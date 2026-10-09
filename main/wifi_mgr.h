#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { WIFI_MODE_NONE_, WIFI_MODE_STA_, WIFI_MODE_AP_ } wifi_state_t;

// Try the configured SSID (NVS override, else build-time creds); fall back to AP mode.
void wifi_mgr_start(void);
wifi_state_t wifi_mgr_state(void);
void wifi_mgr_ip(char *out, size_t n);          // current IPv4 as text ("" if none)
const char *wifi_mgr_ssid(void);                // SSID in use (STA target or AP name)
// Persist new STA credentials in NVS; caller reboots to apply.
bool wifi_mgr_save(const char *ssid, const char *pass);
bool wifi_mgr_clear(void);                      // back to build-time defaults
int  wifi_mgr_scan(char *out, size_t n);        // human-readable scan result

// ---- network settings (persisted in NVS, applied at boot; hostname also applies live) ----
#define NET_DEFAULT_HOSTNAME "esp-minix"        // reachable as esp-minix.local (mDNS)
typedef struct {
    char hostname[32];                          // mDNS / DHCP host name, without ".local"
    bool use_static;                            // false = DHCP
    char ip[16], mask[16], gw[16], dns[16];     // used when use_static
} net_cfg_t;
void net_cfg_load(net_cfg_t *c);
bool net_cfg_save(const net_cfg_t *c);
bool net_hostname_valid(const char *name);
bool net_apply_hostname(const char *name);      // live change of mDNS + DHCP host name
const char *net_hostname(void);
void wifi_mgr_mdns(bool on);                    // start/stop the <hostname>.local responder
bool wifi_mgr_mdns_running(void);
