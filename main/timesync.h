#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Wall-clock time. The ESP32 has no battery clock, so: NTP when the network is up (state 2 = synced);
// after a reboot the last saved time is restored (state 1 = approximate: it lags by however long the board was off);
// before any of that the clock reads 1970 (state 0).
void time_init(void);                       // call once at boot (after NVS init)
int time_state(void);                       // 0 none, 1 approximate (restored), 2 synced
uint32_t time_now(void);                    // epoch seconds, 0 if state 0
bool time_set_tz_spec(const char *spec);    // "+6", "-5:30", "UTC+6" or a POSIX TZ string
const char *time_tz(void);                  // current POSIX TZ string
void time_ntp_servers(char *s1, char *s2, size_t n);
bool time_set_ntp(const char *s1, const char *s2);
int time_local(uint32_t epoch, int out[8]); // sec,min,hour,mday,mon(0-11),year(full),wday(0=Sun),yday
