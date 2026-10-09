// mini.h - the programmer's interface to esp32-unix: syscalls + tiny libc-free helpers.
// Programs are built freestanding (no wasi-libc): this header provides the entry point, the syscalls
// and the few libc-like helpers needed. Write a normal  int main(int argc, char **argv).
//
// Programs are plain C compiled to WebAssembly and AOT-compiled for the ESP32. They talk to the OS
// through the sys_* host functions below (implemented in main/aot_run.c). Staying off wasi-libc's
// stdio/printf keeps programs tiny (printf alone costs ~25 KB of wasm and a lot of scarce RAM).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define SYS(name) __attribute__((import_module("env"), import_name(name)))
SYS("sys_write")      int      sys_write(int fd, const void *buf, int n);   // fd 1/2 = terminal, >=3 = opened files
SYS("sys_read")       int      sys_read(int fd, void *buf, int n);          // fd 0 = terminal (one edited line); 0 = EOF
SYS("sys_open")       int      sys_open(const char *path, int mode);        // mode: 0 read, 1 write/truncate, 2 append
SYS("sys_close")      int      sys_close(int fd);
SYS("sys_getcwd")     int      sys_getcwd(char *buf, int n);
SYS("sys_chdir")      int      sys_chdir(const char *path);                 // change this program's working directory
SYS("sys_stat")       int      sys_stat(const char *path);                  // 1 file, 2 directory, -1 missing
SYS("sys_membuf")     int      sys_membuf(void);                            // in-RAM stream (for pipes); returns an fd
SYS("sys_rewind")     int      sys_rewind(int fd);                          // read an in-RAM stream from the start
SYS("sys_run")        int      sys_run(const char *blob, int blob_len, int argc, int fd_in, int fd_out, int fd_err);
                                                                            // run a command (words are NUL-separated in blob); returns its exit status
SYS("sys_argc")       int      sys_argc(void);
SYS("sys_arg")        int      sys_arg(int i, char *buf, int n);            // copy argv[i]; returns its length
SYS("sys_isatty")     int      sys_isatty(int fd);
SYS("sys_sigint")     int      sys_sigint(int op);                          // 1 catch Ctrl-C, 2 stop catching, 0 pending? (clears)
SYS("sys_sysinfo")    int      sys_sysinfo(int what);                       // see SI_* below
SYS("sys_netinfo")    int      sys_netinfo(int what, char *buf, int n);     // 0 hostname, 1 ip, 2 ssid
SYS("sys_tasks")      int      sys_tasks(void *buf, int n);                 // fills esp_task_t records; CPU% since the previous call
SYS("sys_getkey")     int      sys_getkey(int ms);                          // raw key: byte, -1 timeout, -2 end of input, -3 Ctrl-C
SYS("sys_micros")     uint32_t sys_micros(void);                           // microseconds since boot (wraps after ~71 min)
SYS("sys_adc_read")   int      sys_adc_read(int pin);                       // ADC1 pins 32-39: raw 0..4095 (~0-3.1 V)
SYS("sys_pwm")        int      sys_pwm(int pin, int freq_hz, int duty_permille);   // duty 0..1000; negative duty stops
SYS("sys_dac_write")  int      sys_dac_write(int pin, int value);           // GPIO25/26, 0..255
SYS("sys_i2c_init")   int      sys_i2c_init(int sda, int scl, int hz);      // 0,0,0 = SDA 21, SCL 22, 100 kHz (also automatic)
SYS("sys_i2c_probe")  int      sys_i2c_probe(int addr);                     // 0 if a device answers
SYS("sys_i2c_write")  int      sys_i2c_write(int addr, const void *buf, int n);
SYS("sys_i2c_read")   int      sys_i2c_read(int addr, void *buf, int n);
SYS("sys_i2c_wr")     int      sys_i2c_wr(int addr, const void *w, int wn, void *r, int rn);   // write, restart, read
SYS("sys_uart_open")  int      sys_uart_open(int port, int tx, int rx, int baud);              // port 1 or 2
SYS("sys_uart_write") int      sys_uart_write(int port, const void *buf, int n);
SYS("sys_uart_read")  int      sys_uart_read(int port, void *buf, int n, int timeout_ms);
SYS("sys_uart_close") int      sys_uart_close(int port);
SYS("sys_millis")     uint32_t sys_millis(void);                            // ms since boot
SYS("sys_sleep_ms")   void     sys_sleep_ms(uint32_t ms);                   // interruptible with Ctrl-C
SYS("sys_gpio_mode")  int      sys_gpio_mode(int pin, int out);             // 1 = output, 0 = input with pull-up
SYS("sys_gpio_write") int      sys_gpio_write(int pin, int level);
SYS("sys_gpio_read")  int      sys_gpio_read(int pin);

/* ---- Phase 4 syscalls (ABI 2; sys_mqtt_info added in ABI 3): everything an application needs without reflashing the firmware ---- */
SYS("sys_abi")        int      sys_abi(void);                               // syscall ABI version (2)
SYS("sys_unlink")     int      sys_unlink(const char *path);
SYS("sys_mkdir")      int      sys_mkdir(const char *path);
SYS("sys_rmdir")      int      sys_rmdir(const char *path);
SYS("sys_rename")     int      sys_rename(const char *from, const char *to);   // replaces the target
SYS("sys_fsize")      int      sys_fsize(const char *path);                 // bytes, -1 missing
SYS("sys_listdir")    int      sys_listdir(const char *path, char *buf, int n);   // names, one per line, '/' suffix = directory
SYS("sys_seek")       int      sys_seek(int fd, int off, int whence);       // 0 set, 1 cur, 2 end; returns the position (seek(fd,0,1) = tell)
SYS("sys_tcp_connect") int     sys_tcp_connect(const char *host, int port, int timeout_ms);   // fd (sys_read/sys_write/sys_close), -1 error
SYS("sys_tcp_listen") int      sys_tcp_listen(int port);
SYS("sys_tcp_accept") int      sys_tcp_accept(int lfd, int timeout_ms);     // new fd, 0 timeout, -1 error
SYS("sys_sock_timeout") int    sys_sock_timeout(int fd, int ms);            // receive timeout: sys_read then returns -1 when it expires (0 = peer closed)
SYS("sys_udp_open")   int      sys_udp_open(int port);                      // 0 = ephemeral; read with sys_read, send with sys_udp_sendto
SYS("sys_udp_sendto") int      sys_udp_sendto(int fd, const char *host, int port, const void *buf, int n);
SYS("sys_dns")        int      sys_dns(const char *host, char *buf, int n); // dotted IPv4 string
SYS("sys_time")       uint32_t sys_time(void);                              // epoch seconds (0 if never set)
SYS("sys_time_state") int      sys_time_state(void);                        // 0 none, 1 restored/approximate, 2 NTP-synced
SYS("sys_localtime")  int      sys_localtime(uint32_t epoch, int *out, int bytes);   // out[8]: sec,min,hour,mday,mon(0-11),year,wday,yday
SYS("sys_tz")         int      sys_tz(const char *spec);                    // "+6", "-5:30" or a POSIX TZ string
SYS("sys_random")     uint32_t sys_random(void);                            // hardware random
SYS("sys_reboot")     int      sys_reboot(void);
SYS("sys_log")        int      sys_log(const char *msg);                    // to the serial log
SYS("sys_version")    int      sys_version(char *buf, int n);               // firmware version string
SYS("sys_kv_get")     int      sys_kv_get(const char *key, void *buf, int n);        // length, -1 missing (NVS, 1000 bytes max)
SYS("sys_kv_set")     int      sys_kv_set(const char *key, const void *buf, int n);
SYS("sys_kv_del")     int      sys_kv_del(const char *key);
SYS("sys_kv_key")     int      sys_kv_key(int idx, char *buf, int n);       // name of the idx-th key, -1 past the end
SYS("sys_mqtt_state") int      sys_mqtt_state(void);                        // 0 off, 1 connecting, 2 connected
SYS("sys_mqtt_pub")   int      sys_mqtt_pub(const char *topic, const void *p, int n, int retain, int qos);
SYS("sys_mqtt_sub")   int      sys_mqtt_sub(const char *filter, int qos);   // persistent; latest message cached
SYS("sys_mqtt_unsub") int      sys_mqtt_unsub(const char *filter);
SYS("sys_mqtt_get")   int      sys_mqtt_get(const char *topic, void *buf, int n);    // latest payload length, -1 none
SYS("sys_mqtt_age")   int      sys_mqtt_age(const char *topic);             // ms since it arrived, -1 never
SYS("sys_mqtt_config") int     sys_mqtt_config(const char *host, int port, const char *user, const char *pass, const char *client_id);
SYS("sys_mqtt_info")  int      sys_mqtt_info(int what, char *buf, int n);   // ABI 3: 0 broker host, 1 user, 2 client id, 3 port (text); never the password
SYS("sys_mqtt_ctl")   int      sys_mqtt_ctl(int on);                        // 1 start / 0 stop the native client
SYS("sys_delay_us")   void     sys_delay_us(int us);                        // busy wait (<= 100 ms)
SYS("sys_pulse_in")   int      sys_pulse_in(int pin, int level, int timeout_us);     // pulse width us, -1 timeout, -2 too long
SYS("sys_sonar_pulse") int     sys_sonar_pulse(int trig, int echo, int timeout_us);  // HC-SR04 echo width us (distance_cm = us/58)
SYS("sys_ds18b20")    int      sys_ds18b20(int pin);                        // 1/100 degC, -9999 error (blocks ~0.8 s)
SYS("sys_adc_mv")     int      sys_adc_mv(int pin);                         // calibrated millivolts
SYS("sys_spi_open")   int      sys_spi_open(int sck, int mosi, int miso, int hz, int mode);
SYS("sys_spi_xfer")   int      sys_spi_xfer(int cs, const void *tx, int n, void *rx, int n2);   // n == n2 <= 64; pass the same buffer for both
SYS("sys_pcnt_open")  int      sys_pcnt_open(int unit, int pin);            // unit 0/1: counts rising edges in hardware
SYS("sys_pcnt_read")  int      sys_pcnt_read(int unit);
SYS("sys_pcnt_clear") int      sys_pcnt_clear(int unit);

static inline int m_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static inline size_t m_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static inline void m_write(int fd, const char *s, size_t n)
{
    while (n) { int w = sys_write(fd, s, (int)n); if (w <= 0) return; s += w; n -= (size_t)w; }
}
static inline void m_puts(const char *s) { m_write(1, s, m_strlen(s)); }
static inline void m_eputs(const char *s) { m_write(2, s, m_strlen(s)); }

static inline void m_vfprintf(int fd, const char *f, va_list ap)
{
    char out[96]; size_t o = 0;
#define FLUSH() do { m_write(fd, out, o); o = 0; } while (0)
    for (; *f; f++) {
        if (o > sizeof out - 24) FLUSH();
        if (*f != '%') { out[o++] = *f; continue; }
        f++;
        int zero = 0, width = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            FLUSH(); m_write(fd, s, m_strlen(s));
        } else if (*f == 'c') {
            out[o++] = (char)va_arg(ap, int);
        } else if (*f == 'd' || *f == 'u' || *f == 'x') {
            unsigned v; int neg = 0;
            if (*f == 'd') { int i = va_arg(ap, int); if (i < 0) { neg = 1; v = 0u - (unsigned)i; } else v = (unsigned)i; }
            else v = va_arg(ap, unsigned);
            unsigned base = *f == 'x' ? 16 : 10;
            char tmp[12]; int n = 0;
            do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            int len = n + neg;
            if (!zero) while (len++ < width) out[o++] = ' ';
            if (neg) out[o++] = '-';
            if (zero) while (len++ < width) out[o++] = '0';
            while (n) out[o++] = tmp[--n];
        } else if (*f == '%') {
            out[o++] = '%';
        } else if (!*f) {
            break;
        }
    }
    FLUSH();
#undef FLUSH
}
static inline void m_printf(const char *f, ...) { va_list ap; va_start(ap, f); m_vfprintf(1, f, ap); va_end(ap); }
static inline void m_eprintf(const char *f, ...) { va_list ap; va_start(ap, f); m_vfprintf(2, f, ap); va_end(ap); }

static inline int m_atoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

// snprintf-style formatting into a buffer: %s %c %d %u %x with optional 0 and width. Returns the length it wanted (like snprintf).
static inline int m_vsnprintf(char *b, int n, const char *f, va_list ap)
{
    int o = 0;
#define PUT(c) do { if (o < n - 1) b[o] = (char)(c); o++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int zero = 0, width = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 's') { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) PUT(*s++); }
        else if (*f == 'c') PUT(va_arg(ap, int));
        else if (*f == 'd' || *f == 'u' || *f == 'x') {
            unsigned v; int neg = 0;
            if (*f == 'd') { int i = va_arg(ap, int); if (i < 0) { neg = 1; v = 0u - (unsigned)i; } else v = (unsigned)i; }
            else v = va_arg(ap, unsigned);
            unsigned base = *f == 'x' ? 16 : 10;
            char tmp[12]; int k = 0;
            do { tmp[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            int len = k + neg;
            if (!zero) while (len++ < width) PUT(' ');
            if (neg) PUT('-');
            if (zero) while (len++ < width) PUT('0');
            while (k) PUT(tmp[--k]);
        } else if (*f == '%') PUT('%');
        else if (!*f) break;
    }
    if (n > 0) b[o < n ? o : n - 1] = 0;
#undef PUT
    return o;
}
static inline int m_snprintf(char *b, int n, const char *f, ...) { va_list ap; va_start(ap, f); int r = m_vsnprintf(b, n, f, ap); va_end(ap); return r; }

enum { SI_UPTIME_S, SI_DRAM_FREE, SI_DRAM_TOTAL, SI_DRAM_MINFREE, SI_DRAM_LARGEST, SI_IRAM8_FREE, SI_IRAM8_TOTAL,
       SI_EXEC_FREE, SI_EXEC_TOTAL, SI_FS_USED, SI_FS_TOTAL, SI_RSSI, SI_CPU_MHZ, SI_NTASKS, SI_COLS, SI_ROWS };
typedef struct { char name[12]; unsigned char state, prio; unsigned short stack, cpu10, pid; } esp_task_t;   // cpu10 = CPU% x 10

/* ---- freestanding runtime: memory primitives (the compiler may emit calls to these) ---- */
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    char *a = d; const char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

/* ---- entry point: the OS calls esp_main(); it builds argc/argv from the process arguments ---- */
int main(int argc, char **argv);
static char m_argbuf[1280];
static char *m_argv[33];
__attribute__((export_name("esp_main"))) int esp_main(void)
{
    int argc = sys_argc(), used = 0, n = 0;
    for (int i = 0; i < argc && n < 32; i++) {
        int l = sys_arg(i, m_argbuf + used, (int)sizeof m_argbuf - used);
        if (l < 0) break;
        m_argv[n++] = m_argbuf + used;
        used += l + 1;
    }
    m_argv[n] = 0;
    return main(n, m_argv);
}
