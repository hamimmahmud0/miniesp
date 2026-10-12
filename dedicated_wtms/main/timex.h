#pragma once
#include <stdint.h>
// Clock: SNTP (servers and time zone in the NVS namespace "time", as in the miniesp OS), the last saved time is used until the first sync.
void time_init(void);
int time_state(void);              // 0 none, 1 approximate (restored), 2 synced
uint32_t time_now(void);           // epoch seconds, 0 if the clock was never set
int time_local(uint32_t epoch, int out[8]);   // sec,min,hour,mday,mon(0-11),year,wday,yday
int time_set_tz(const char *spec); // "+6", "-5:30" or a POSIX TZ string; 0 on success
