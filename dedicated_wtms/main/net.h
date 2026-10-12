#pragma once
#include <stdbool.h>
#include <stddef.h>
// Wi-Fi station (static address or DHCP, settings in the NVS namespaces "net" and "wifi" shared with the miniesp OS) and mDNS.
void net_start(void);              // starts connecting in the background; reconnects forever
bool net_up(void);                 // has an IP address
const char *net_hostname(void);
void net_ip(char *out, size_t n);
int net_rssi(void);
