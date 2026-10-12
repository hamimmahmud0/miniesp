#pragma once
// The few "system calls" the wtms control code (ported unchanged from the miniesp-pkg program) uses, implemented natively.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_SETUP 1                       // interactive setup does not exist here: configuration is done on the web page
enum { SI_UPTIME_S, SI_DRAM_FREE, SI_DRAM_TOTAL, SI_DRAM_MINFREE, SI_DRAM_LARGEST, SI_IRAM8_FREE, SI_IRAM8_TOTAL, SI_EXEC_FREE, SI_EXEC_TOTAL, SI_FS_USED, SI_FS_TOTAL, SI_RSSI };

int sys_sysinfo(int what);
int sys_open(const char *path, int mode);          // 0 read, 1 write/truncate, 2 append
int sys_read(int fd, void *buf, int n);
int sys_write(int fd, const void *buf, int n);
int sys_close(int fd);
int sys_fsize(const char *path);
int sys_stat(const char *path);                    // 1 file, 2 directory, -1 missing
int sys_mkdir(const char *path);
int sys_rename(const char *from, const char *to);
int sys_kv_get(const char *key, void *buf, int n);
int sys_kv_set(const char *key, const void *buf, int n);
int sys_kv_del(const char *key);
unsigned sys_time(void);
unsigned sys_millis(void);
unsigned sys_random(void);
int sys_localtime(unsigned epoch, int *out, int bytes);
int sys_tz(const char *spec);
void sys_sleep_ms(uint32_t ms);
int sys_mqtt_state(void);
int sys_mqtt_pub(const char *topic, const void *p, int n, int retain, int qos);
int sys_mqtt_sub(const char *topic, int qos);
int sys_mqtt_get(const char *topic, void *buf, int n);
int sys_mqtt_age(const char *topic);
int sys_mqtt_config(const char *host, int port, const char *user, const char *pass, const char *cid);
int sys_mqtt_info(int what, char *buf, int n);     // 0 host, 1 user, 2 client id, 3 port (text)
int sys_netinfo(int what, char *buf, int n);       // 0 hostname, 1 ip, 2 ssid
int sys_sonar_pulse(int trig, int echo, int timeout_us);
int sys_ds18b20(int pin);
int sys_log(const char *msg);                      // "E text" / "W text" are logged as error / warning

#define m_snprintf snprintf
#define m_vsnprintf vsnprintf
#define m_strlen strlen
#define m_strcmp strcmp
#define m_atoi atoi
void m_puts(const char *s);                        // output of a command: goes to the current response buffer (web) or nowhere
void m_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define m_eputs m_puts
#define m_eprintf m_printf

// output capture for one call (thread-local)
void compat_out_begin(char *buf, int cap);
int compat_out_end(void);
// the dashboard's state.json (kept in RAM: it changes every cycle)
int compat_state_json(char *out, int n);
