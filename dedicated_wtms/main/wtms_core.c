// wtms core for dedicated_wtms: the control logic of the miniesp-pkg program wtms 1.3.7 (same decisions, configuration keys, MQTT topics, history files and
// web API), compiled natively. System calls come from compat.c. This file is a fork: fixes made in the program are not picked up automatically.
//   - runs on core 1 (app task below); the web handlers on core 0 call into it through wtms_web() / wtms_state under one lock
//   - state.json is served from RAM; the history files are in /lfs/tank
#include "compat.h"
#include "esp_log.h"
#include "wtms_core.h"
#include <stdint.h>
#include "sonar.h"
#include "klog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


typedef struct { const char *k; void *p; int n; } ik;       // n == 0: int, else a string of n bytes

static char c_name[24] = "tank1", c_pump[64] = "relay_node_1/relay/waterpump", c_ha[24] = "homeassistant";
static int c_trig = 33, c_echo = 32, c_temp = 0, c_empty = 1379, c_full = 205, c_vol = 1000, c_ml = 8517, c_on = 25, c_off = 95;
static int c_maxrun = 30, c_rest = 5, c_flowmin = 3, c_rise = 10, c_auto = 1, c_hist = 300;
static int c_blind = 90, c_minin = 0, c_interval = 15, c_samples = 7, c_jump = 15, c_goodn = 3, c_histkb = 32, c_tempc = 20;
static const ik CFG[] = {
    { "name", c_name, sizeof c_name }, { "pump", c_pump, sizeof c_pump }, { "ha", c_ha, sizeof c_ha },
    { "trig", &c_trig, 0 }, { "echo", &c_echo, 0 }, { "temp", &c_temp, 0 }, { "empty_mm", &c_empty, 0 }, { "full_mm", &c_full, 0 },
    { "vol_l", &c_vol, 0 }, { "ml_cm", &c_ml, 0 }, { "on_pct", &c_on, 0 }, { "off_pct", &c_off, 0 }, { "max_run", &c_maxrun, 0 }, { "rest", &c_rest, 0 },
    { "flow_min", &c_flowmin, 0 }, { "rise_pm", &c_rise, 0 }, { "min_in", &c_minin, 0 }, { "auto", &c_auto, 0 }, { "hist", &c_hist, 0 },
    { "interval", &c_interval, 0 }, { "samples", &c_samples, 0 }, { "jump_pct", &c_jump, 0 }, { "good_n", &c_goodn, 0 }, { "hist_kb", &c_histkb, 0 }, { "temp_c", &c_tempc, 0 }, { "blind_s", &c_blind, 0 },
};
static int s_force, s_funtil, s_good, s_ema = -1, s_since, s_start, s_lastoff, s_fault, s_lastrun, s_lasthist, s_disc, s_jumps, s_hold, s_goodlast;
static int s_lo, s_badn, s_lhb, s_lmem, s_lerr, s_lsup;       // log state (saved): flags of the last cycle, failed sensor cycles in a row, last status line / low-memory warning time
// Water accounting (volumes in decilitres): pv = volume at the previous history sample + 1 (0 = none), pps = pump seconds since then, urate = recent idle
// usage in ml/min, dd = date (yyyymmdd) of the running day, du/di/dps/dr = that day's usage, pump intake (dL), pump seconds, pump runs, lr* = last pump run
static int s_pv, s_pps, s_urate, s_dd, s_du, s_di, s_dps, s_dr, s_lrl, s_lrs, s_lrt;
static const ik STT[] = {
    { "ema", &s_ema, 0 }, { "force", &s_force, 0 }, { "funtil", &s_funtil, 0 }, { "good", &s_good, 0 }, { "since", &s_since, 0 }, { "start", &s_start, 0 }, { "lastoff", &s_lastoff, 0 }, { "fault", &s_fault, 0 },
    { "lastrun", &s_lastrun, 0 }, { "lasthist", &s_lasthist, 0 }, { "disc", &s_disc, 0 }, { "jumps", &s_jumps, 0 }, { "hold", &s_hold, 0 }, { "gl", &s_goodlast, 0 },
    { "pv", &s_pv, 0 }, { "pps", &s_pps, 0 }, { "urate", &s_urate, 0 }, { "dd", &s_dd, 0 }, { "du", &s_du, 0 }, { "di", &s_di, 0 }, { "dps", &s_dps, 0 },
    { "dr", &s_dr, 0 }, { "lrl", &s_lrl, 0 }, { "lrs", &s_lrs, 0 }, { "lrt", &s_lrt, 0 },
    { "lo", &s_lo, 0 }, { "bad", &s_badn, 0 }, { "lhb", &s_lhb, 0 }, { "lmem", &s_lmem, 0 }, { "lerr", &s_lerr, 0 }, { "lsup", &s_lsup, 0 },
};
#define N(t) ((int)(sizeof(t) / sizeof((t)[0])))
#define DISC_VER 3
static int g_quiet;
static char g_local_cmd[24];                         // command given on the command line / CGI (handled like an MQTT command)
static const char *FAULTS[] = { "none", "sensor", "max_run", "no_flow" };
enum { F_NONE, F_SENSOR, F_MAXRUN, F_NOFLOW };
static const char *DIR = "/www/tank", *HIST = "/www/tank/history.csv", *HIST_OLD = "/www/tank/history.old.csv", *STATE_JSON = "/www/tank/state.json",
                  *DAILY = "/www/tank/daily.csv", *DAILY_OLD = "/www/tank/daily.old.csv", *RUNS = "/www/tank/runs.csv", *RUNS_OLD = "/www/tank/runs.old.csv";

static int eq(const char *a, const char *b) { return !m_strcmp(a, b); }

static int find_key(const ik *t, int n, const char *k) { for (int i = 0; i < n; i++) if (eq(t[i].k, k)) return i; return -1; }

static void apply(const ik *t, int n, const char *k, const char *v)
{
    int i = find_key(t, n, k);
    if (i < 0) return;
    if (t[i].n) { int j = 0; while (v[j] && j < t[i].n - 1) { ((char *)t[i].p)[j] = v[j]; j++; } ((char *)t[i].p)[j] = 0; }
    else *(int *)t[i].p = m_atoi(v);
}

static void load(const char *key, const ik *t, int n)
{
    char buf[700];
    int len = sys_kv_get(key, buf, sizeof buf - 1);
    if (len <= 0) return;
    buf[len] = 0;
    char *p = buf;
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        char *q = p; while (*q && *q != '=') q++;
        if (*q) { *q = 0; apply(t, n, p, q + 1); }
        p = more ? e + 1 : e;
    }
}

static void save(const char *key, const ik *t, int n)
{
    char buf[700]; int o = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].n) o += m_snprintf(buf + o, (int)sizeof buf - o, "%s=%s\n", t[i].k, (const char *)t[i].p);
        else o += m_snprintf(buf + o, (int)sizeof buf - o, "%s=%d\n", t[i].k, *(int *)t[i].p);
    }
    if (o >= (int)sizeof buf) o = sizeof buf - 1;
    sys_kv_set(key, buf, o);
}

static unsigned tnow(void) { unsigned t = sys_time(); return t ? t : sys_millis() / 1000; }
static int iabs(int v) { return v < 0 ? -v : v; }
static void sort(int *a, int n) { for (int i = 1; i < n; i++) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; } }

static int g_t100 = 2000, g_have_t;      // temperature, 1/100 C

/* ---------------- log: into the firmware's kernel log (/log/kernel.log, kernel.1; sent over MQTT when a file fills up; read with `klog`) ---------------- */
// What is logged: events (pump start/stop with the reason, faults, commands, MQTT / relay-node changes, config changes), every error with its
// context (sensor echo statistics, memory, Wi-Fi, filesystem), missed control cycles and reboots, and a status line every 30 minutes.
// Lines starting with "E " / "W " are logged as errors / warnings (the kernel log writes those to flash within 10 s).
static int g_mn, g_mlo, g_mmed, g_mhi, g_magree, g_mto, g_mstuck, g_mbad, g_mwhy, g_mdist;     // last measure(): echoes, spread (us), agreeing, timeouts, stuck-high, out of window, why it failed
static const char *WHY[] = { "ok", "too few valid echoes", "echoes disagree (noise)", "distance outside the tank" };

static void wlog(char lvl, const char *fmt, ...)
{
    char l[420]; int o = 0;
    l[o++] = lvl == 'E' || lvl == 'W' ? lvl : 'I'; l[o++] = ' ';
    va_list ap; va_start(ap, fmt);
    o += m_vsnprintf(l + o, (int)sizeof l - o, fmt, ap);
    va_end(ap);
    if (o > (int)sizeof l - 1) o = sizeof l - 1;
    l[o] = 0;
    // the kernel log cuts lines at ~250 characters: send long messages in pieces
    for (int i = 0, e; i < o; i = e) {
        e = i + 200 < o ? i + 200 : o;
        if (e < o) { int k = e; while (k > i + 120 && l[k] != ' ') k--; if (l[k] == ' ') e = k + 1; }       // break at a space
        char piece[216]; int k = 0;
        if (i) { piece[k++] = l[0]; piece[k++] = ' '; piece[k++] = '.'; piece[k++] = '.'; piece[k++] = ' '; }
        for (int j = i; j < e; j++) piece[k++] = l[j];
        piece[k] = 0;
        sys_log(piece);
    }
}

// memory, Wi-Fi and filesystem figures for an error line
#ifndef CGI_BUILD
#define wlogx wlog
#else
#define wlogx(...) ((void)0)      // the web page's copy logs only the short event lines (it must stay small)
#endif
static const char *ctx(void)
{
    static char b[140];
    m_snprintf(b, sizeof b, "[up %ds heap %d/min %d/blk %d fs %dk rssi %d mqtt %d core %d]", sys_sysinfo(SI_UPTIME_S), sys_sysinfo(SI_DRAM_FREE),
               sys_sysinfo(SI_DRAM_MINFREE), sys_sysinfo(SI_DRAM_LARGEST), sys_sysinfo(SI_FS_USED) / 1024,
               sys_sysinfo(SI_RSSI), sys_mqtt_state(), (int)xPortGetCoreID());
    return b;
}

// Distance sensor-to-water in mm (median of 7 echoes, speed of sound corrected for temperature), or -1 on failure (g_mwhy says why).
static int measure(void)
{
    g_t100 = c_tempc * 100;                                    // assumed air temperature when there is no DS18B20
    if (c_temp > 0) { int t = sys_ds18b20(c_temp); if (t > -9000) { g_t100 = t; g_have_t = 1; } }
    int v[15], n = 0;
    g_mto = g_mstuck = g_mbad = g_mlo = g_mmed = g_mhi = g_magree = g_mdist = 0;
    for (int i = 0; i < c_samples; i++) {
        int us = sys_sonar_pulse(c_trig, c_echo, 25000);
        if (us >= 150 && us <= 24000) v[n++] = us;
        else if (us == -1) g_mto++;                            // no echo started
        else if (us == -2) g_mstuck++;                         // echo line stuck high
        else g_mbad++;                                         // shorter than the dead zone / longer than the tank
        sys_sleep_ms(60);
    }
    g_mn = n; g_mwhy = 1;
    if (n < (c_samples + 1) / 2) return -1;                    // at least half of the echoes must be valid
    sort(v, n);
    g_mlo = v[0]; g_mmed = v[n / 2]; g_mhi = v[n - 1];
    int agree = 0;                                             // and a majority must agree with the median within ~1 cm (60 us): a damp or noisy sensor
    for (int i = 0; i < n; i++) if (v[i] - v[n / 2] < 60 && v[n / 2] - v[i] < 60) agree++;      // returns scattered echoes whose median would be a false level
    g_magree = agree; g_mwhy = 2;
    if (agree < c_samples / 2 + 1) return -1;
    int d = (int)((long long)v[n / 2] * (3313 + g_t100 * 606 / 10000) / 20000);
    g_mdist = d; g_mwhy = 3;
    if (d < 25 || d > c_empty + 150) return -1;           // closer than the sensor's dead zone or farther than the tank floor: not a real water echo
    g_mwhy = 0;
    return d;
}

// With ml_cm set the EMPTY distance follows from the total storage and the litres per cm of water height.
static void derive(void)
{
    if (c_ml > 0 && c_vol > 0) c_empty = c_full + c_vol * 10000 / c_ml;
}

static int level_pm(int d_mm)
{
    int span = c_empty - c_full; if (span < 50) span = 50;
    int pm = (c_empty - d_mm) * 1000 / span;
    return pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
}

static char tb[128], pb[760];
static void topic(const char *fmt) { m_snprintf(tb, sizeof tb, fmt, c_name); }
static int g_pubrc, g_pubfail; static char g_pubt[64];                // a publish failed this cycle (logged once by run_cycle)
static int pub(const char *t, const char *p, int retain)
{
    int r = sys_mqtt_pub(t, p, (int)m_strlen(p), retain, 1);
    if (r < 0 && !g_pubfail) { g_pubfail = 1; g_pubrc = r; m_snprintf(g_pubt, sizeof g_pubt, "%s", t); }
    return r;
}

static int urate(void) { return s_urate > 0 ? s_urate : 0; }          // recent idle usage ml/min (the stored value is signed so sensor noise averages out)

static int pm_run_dl(int pm, int dur)                  // decilitres added by the pump run in progress / just ended: level rise + the usage that went on meanwhile
{
    return (pm - s_start) * c_vol / 100 + urate() * (dur > 3600 ? 3600 : dur) / 6000;
}

static int mk_state(char *b, int n, int pm, int dist, const char *pump, int auto_)
{
    int t10 = g_t100 / 10, live = s_since && tnow() - (unsigned)s_since >= 60, dur = live ? (int)(tnow() - (unsigned)s_since) : 0;
    int in = live ? pm_run_dl(pm, dur) * 6000 / dur : s_lrs > 0 ? s_lrl * 6000 / s_lrs : 0;     // pump intake ml/min
    if (in < 0) in = 0;
    return m_snprintf(b, n, "{\"ts\":%u,\"level\":%d.%d,\"litres\":%d,\"distance_cm\":%d.%d,\"pump\":\"%s\",\"auto\":\"%s\",\"fault\":\"%s\",\"temp\":%d.%d,"
                      "\"on_pct\":%d,\"off_pct\":%d,\"vol\":%d,\"force\":\"%s\",\"force_left\":%d,\"lpcm\":%d,"
                      "\"use_lpm\":%d.%02d,\"in_lpm\":%d.%02d,\"td_use\":%d.%d,\"td_in\":%d.%d,\"td_pump_s\":%d,\"td_runs\":%d,\"td_date\":%d,\"lr_l\":%d.%d,\"lr_s\":%d,\"lr_t\":%d}",
                      sys_time(), pm / 10, pm % 10, c_vol * pm / 1000, dist / 10, iabs(dist) % 10, pump, auto_ ? "ON" : "OFF", FAULTS[s_fault],
                      t10 / 10, iabs(t10) % 10, c_on, c_off, c_vol, s_force == 1 ? "on" : s_force == 2 ? "off" : "none",
                      s_force && s_funtil && (unsigned)s_funtil > tnow() ? (int)((unsigned)s_funtil - tnow()) : 0, c_ml,
                      urate() / 1000, urate() % 1000 / 10, in / 1000, in % 1000 / 10, (s_du < 0 ? 0 : s_du) / 10, iabs(s_du) % 10, s_di / 10, s_di % 10, s_dps, s_dr, s_dd,
                      s_lrl / 10, iabs(s_lrl) % 10, s_lrs, s_lrt);
}

static int g_fserr; static const char *g_fspath;                    // a file write failed this cycle (logged once by run_cycle)
static void write_file(const char *path, const char *s)
{
    int fd = sys_open(path, 1), n = (int)m_strlen(s);
    if (fd < 0 || sys_write(fd, s, n) != n) { g_fserr = 1; g_fspath = path; }
    if (fd >= 0) sys_close(fd);
}

static void append_line(const char *path, const char *old, int max, const char *l, int n)
{
    if (sys_fsize(path) > max) sys_rename(path, old);
    int fd = sys_open(path, 2);
    if (fd < 0) { g_fserr = 1; g_fspath = path; return; }
    if (sys_write(fd, l, n) != n) { g_fserr = 1; g_fspath = path; }
    sys_close(fd);
}

// One history sample every `hist` seconds: level, pump state and the water accounting for the interval since the previous sample.
// Usage: when the pump was idle, the drop in volume (signed, so sensor noise cancels out over a day); while it ran, the recent idle rate is assumed to
// continue, and the pump's intake is the volume change plus that usage. Row: time,level(0.1%),litres,pump,usage ml/min (signed),pump intake ml/min.
static void history(int pm, int pumpon)
{
    if (!sys_time()) return;
    unsigned now = sys_time();
    if (s_lasthist && now - (unsigned)s_lasthist < (unsigned)c_hist) return;
    int dt = s_lasthist ? (int)(now - (unsigned)s_lasthist) : 0, v = c_vol * pm / 100, u = 0, in = 0, um = 0, im = 0;
    if (s_pv > 0 && dt >= 30 && dt <= 3 * c_hist + 60) {
        int dv = v - (s_pv - 1);
        if (s_pps * 10 < dt) { u = -dv; s_urate = (3 * s_urate + u * 6000 / dt) / 4; }
        else { u = urate() * dt / 6000; in = dv + u; if (in < 0) in = 0; }
        um = u * 6000 / dt;                                            // signed on purpose: sensor noise averages out, readers clamp at 0 after averaging
        im = in > 0 && s_pps > 0 ? in * 6000 / s_pps : 0;
    }
    s_lasthist = (int)now;
    int lt[8]; sys_localtime(now, lt, 32);
    int date = lt[5] * 10000 + (lt[4] + 1) * 100 + lt[3];
    if (s_dd && date != s_dd) {                                    // a new day: store the finished one
        wlog('I', "day %d closed: used %d.%d L, pumped %d.%d L, pump %d s, %d runs", s_dd, (s_du < 0 ? 0 : s_du) / 10, iabs(s_du) % 10, s_di / 10, s_di % 10, s_dps, s_dr);
        char l[56]; int k = m_snprintf(l, sizeof l, "%d,%d,%d,%d,%d\n", s_dd, s_du < 0 ? 0 : s_du, s_di, s_dps, s_dr);
        append_line(DAILY, DAILY_OLD, 12000, l, k);
        s_du = s_di = s_dps = s_dr = 0;
    }
    s_dd = date; s_du += u; s_di += in; s_dps += s_pps; s_pv = v + 1; s_pps = 0;
    char l[64]; int k = m_snprintf(l, sizeof l, "%u,%d,%d,%d,%d,%d\n", now, pm, c_vol * pm / 1000, pumpon, um, im);
    append_line(HIST, HIST_OLD, c_histkb * 1000, l, k);
}

#ifdef NO_DISC          // the CGI build leaves Home Assistant discovery to the service (smaller: it must load while RAM is fragmented)
static void discovery(void) {}
#else
// Home Assistant MQTT discovery (retained). `extra` holds the entity specific JSON fields.
static void disc(const char *comp, const char *obj, const char *label, const char *extra)
{
    char host[40]; sys_netinfo(0, host, sizeof host);
    m_snprintf(tb, sizeof tb, "%s/%s/%s_%s/config", c_ha, comp, c_name, obj);
    m_snprintf(pb, sizeof pb, "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"availability_topic\":\"%s/status\",%s,"
               "\"device\":{\"identifiers\":[\"setms_%s\"],\"name\":\"Water tank\",\"manufacturer\":\"SETMS\",\"model\":\"ESP32 tank manager\"}}",
               label, c_name, obj, host, extra, c_name);
    pub(tb, pb, 1);
}

static void discovery(void)
{
    if (!c_ha[0]) return;
    char x[300];
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.level }}\",\"unit_of_measurement\":\"%%\",\"state_class\":\"measurement\",\"icon\":\"mdi:water-percent\"", c_name);
    disc("sensor", "level", "Tank level", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.litres }}\",\"unit_of_measurement\":\"L\",\"device_class\":\"water\",\"state_class\":\"measurement\"", c_name);
    disc("sensor", "litres", "Tank volume", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.distance_cm }}\",\"unit_of_measurement\":\"cm\",\"device_class\":\"distance\",\"entity_category\":\"diagnostic\"", c_name);
    disc("sensor", "distance", "Tank sensor distance", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.fault }}\",\"icon\":\"mdi:alert\"", c_name);
    disc("sensor", "fault", "Tank fault", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.use_lpm }}\",\"unit_of_measurement\":\"L/min\",\"state_class\":\"measurement\",\"icon\":\"mdi:water-minus\"", c_name);
    disc("sensor", "use_rate", "Tank water usage rate", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.in_lpm }}\",\"unit_of_measurement\":\"L/min\",\"state_class\":\"measurement\",\"icon\":\"mdi:water-plus\"", c_name);
    disc("sensor", "in_rate", "Tank pump intake rate", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.td_use }}\",\"unit_of_measurement\":\"L\",\"device_class\":\"water\",\"state_class\":\"total_increasing\"", c_name);
    disc("sensor", "used_today", "Tank water used today", x);
    m_snprintf(x, sizeof x, "\"state_topic\":\"%s/auto/state\",\"command_topic\":\"%s/auto/set\",\"icon\":\"mdi:autorenew\"", c_name, c_name);
    disc("switch", "auto", "Tank auto fill", x);
    m_snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"reset\",\"icon\":\"mdi:restart-alert\"", c_name);
    disc("button", "reset", "Tank reset fault", x);
    m_snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"force_on:30\",\"icon\":\"mdi:water-pump\"", c_name);
    disc("button", "force_on", "Tank force pump ON 30 min", x);
    m_snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"force_off\",\"icon\":\"mdi:water-pump-off\"", c_name);
    disc("button", "force_off", "Tank force pump OFF", x);
    m_snprintf(x, sizeof x, "\"command_topic\":\"%s/cmd\",\"payload_press\":\"release\",\"icon\":\"mdi:autorenew\"", c_name);
    disc("button", "release", "Tank release force (auto)", x);
    s_disc = DISC_VER;
    wlog('I', "Home Assistant discovery published (%s/...)", c_ha);
}
#endif

static int new_cmd(const char *t, char *buf, int n, unsigned now)    // a message that arrived since the previous cycle, or 0
{
    int len = sys_mqtt_get(t, buf, n - 1);
    int age = sys_mqtt_age(t);
    if (len <= 0 || age < 0) return 0;
    unsigned since = s_lastrun ? (now - (unsigned)s_lastrun) * 1000u + 2000u : 30000u;
    if ((unsigned)age > since) return 0;
    buf[len] = 0;
    return len;
}

static int run_cycle(void)
{
    unsigned now = tnow();
    // The service ticks every 5 s; the real cycle runs every `interval` seconds (at most 15 s while the pump is running, so the relay's
    // auto-off timer keeps being refreshed). Commands from the shell / web page always run at once.
    if (!g_local_cmd[0] && s_lastrun) {
        unsigned every = (unsigned)c_interval; if (s_since && every > 15) every = 15;
        if (now - (unsigned)s_lastrun < every - 2) return 0;
    }
    sys_mkdir(DIR);
    g_pubfail = 0; g_fserr = 0;
    int prev_fault = s_fault, lo = s_lo;
    if (s_lastrun && now > (unsigned)s_lastrun + 60 && now > (unsigned)s_lastrun + 3u * (unsigned)c_interval) {        // the service did not manage a cycle for a long time
        unsigned gap = now - (unsigned)s_lastrun, up = (unsigned)sys_sysinfo(SI_UPTIME_S);
        if (up < gap) wlogx('W', "restart: the board booted %us ago, no control cycle for %us %s", up, gap, ctx());
        else wlogx('E', "no control cycle for %us although the board was up: the service skipped runs (busy / program load failed: out of memory?) %s", gap, ctx());
    }
    int was_since = s_since, prev_run = s_lastrun;                      // to spot the end of a pump run and to count pump seconds
    char node[64]; int i = 0;                                         // relay-node base topic = everything before "/relay/"
    {
        const char *p = c_pump; int found = -1;
        for (int k = 0; p[k]; k++) if (p[k] == '/' && p[k + 1] == 'r' && p[k + 2] == 'e' && p[k + 3] == 'l' && p[k + 4] == 'a' && p[k + 5] == 'y' && p[k + 6] == '/') { found = k; break; }
        for (i = 0; found >= 0 && i < found && i < (int)sizeof node - 8; i++) node[i] = p[i];
        node[i] = 0;
    }
    char t_state[96], t_set[96], t_stat[96], t_cmd[48], t_auto[48], buf[24];
    m_snprintf(t_state, sizeof t_state, "%s/state", c_pump);
    m_snprintf(t_set, sizeof t_set, "%s/set", c_pump);
    m_snprintf(t_stat, sizeof t_stat, "%s/status", node);
    m_snprintf(t_cmd, sizeof t_cmd, "%s/cmd", c_name);
    m_snprintf(t_auto, sizeof t_auto, "%s/auto/set", c_name);
    sys_mqtt_sub(t_state, 1); sys_mqtt_sub(t_stat, 1); sys_mqtt_sub(t_cmd, 1); sys_mqtt_sub(t_auto, 1);

    // ---- measure ----
    int d = measure(), pm = -1, sensor_ok = d >= 0;
    s_good = sensor_ok ? (s_good < 100 ? s_good + 1 : s_good) : 0;
    int sens_logged = 0;                                       // a noisy sensor must not flood the log: one failure line per 5 minutes, the rest is counted
    if (sensor_ok && s_badn) { if (s_badn >= 4) wlogx('I', "sensor recovered after %d failed cycles (%us without a reading)", s_badn, now - (unsigned)s_goodlast); s_badn = 0; }
    else if (!sensor_ok) {
        s_badn++;
        if ((s_badn == 1 || s_badn % 20 == 0) && now - (unsigned)s_lerr < 300) s_lsup++;
        else if (s_badn == 1 || s_badn % 20 == 0) {
            sens_logged = 1; s_lerr = (int)now;
            wlogx('E', "sensor failure #%d (+%d suppressed): %s; valid %d/%d, no-echo %d, stuck %d, out-of-window %d, echo us min/med/max %d/%d/%d, agreeing %d, dist %d mm, pump %s, last good %us ago %s",
                 s_badn, s_lsup, WHY[g_mwhy], g_mn, c_samples, g_mto, g_mstuck, g_mbad, g_mlo, g_mmed, g_mhi, g_magree, g_mdist, s_since ? "ON" : "off",
                 s_goodlast ? now - (unsigned)s_goodlast : 0u, ctx());
            s_lsup = 0;
        }
    }
    if (sensor_ok) {
        int raw = level_pm(d);
        if (s_ema < 0) s_ema = raw;
        else if (iabs(raw - s_ema) > c_jump * 10 && s_jumps < 3) s_jumps++;           // ignore a one-off jump (splash, reflection)
        else { s_ema = (s_ema + raw) / 2; s_jumps = 0; }
        pm = s_ema;
        s_goodlast = (int)now;
        if (s_fault == F_SENSOR) s_fault = F_NONE;
    }
    // A running pump can disturb the sonar (supply noise, vibration): do not stop it for a few failed readings, only after blind_s seconds without a good one.
    int blind = !sensor_ok && c_blind > 0 && s_since && s_goodlast && now - (unsigned)s_goodlast <= (unsigned)c_blind
                && s_ema >= 0 && s_ema < c_off * 10 - 100;           // never blind within 10 % of the stop level (overflow)
    if (blind) pm = s_ema;
    else if (!sensor_ok && s_fault == F_NONE) s_fault = F_SENSOR;

    int online = 1;
    if (sys_mqtt_get(t_stat, buf, sizeof buf - 1) > 0) { buf[sys_mqtt_get(t_stat, buf, sizeof buf - 1)] = 0; online = eq(buf, "online"); }
    int ps = sys_mqtt_get(t_state, buf, sizeof buf - 1), pump_known = ps > 0, pump_on = 0;
    if (pump_known) { buf[ps] = 0; pump_on = eq(buf, "ON"); }
    int mq = sys_mqtt_state() == 2;
    if (lo & 128) {
        if (mq != !!(lo & 2)) wlogx(mq ? 'I' : 'W', "MQTT %s %s", mq ? "connected" : "DISCONNECTED", ctx());
        if (online != !!(lo & 4)) wlogx(online ? 'I' : 'E', "relay node %s", online ? "online" : "OFFLINE");
    }

    // ---- commands ----
    if (mq && new_cmd(t_auto, buf, sizeof buf, now)) { wlog('I', "auto mode %s (mqtt)", buf); c_auto = eq(buf, "ON") || eq(buf, "1"); save("tank.cfg", CFG, N(CFG)); }
    int want_on = 0, want_off = 0;
    int got = 0;
    if (g_local_cmd[0]) { for (int k = 0; k < (int)sizeof buf - 1 && g_local_cmd[k]; k++) buf[k] = g_local_cmd[k], buf[k + 1] = 0; got = 1; }
    else if (mq && new_cmd(t_cmd, buf, sizeof buf, now)) got = 1;
    if (got) {
        wlog('I', "command %s (%s)", buf, g_local_cmd[0] ? "web" : "mqtt");
        int fmin = 0;                                                     // "force_on:30" -> 30 minutes
        for (char *q = buf; *q; q++) if (*q == ':') { fmin = m_atoi(q + 1); *q = 0; break; }
        if (eq(buf, "force_on")) { s_force = 1; if (fmin <= 0) fmin = c_maxrun; if (fmin > 1440) fmin = 1440; s_funtil = (int)now + fmin * 60; }
        else if (eq(buf, "force_off")) { s_force = 2; s_funtil = fmin > 0 ? (int)now + (fmin > 1440 ? 1440 : fmin) * 60 : 0; }
        else if (eq(buf, "release")) { s_force = 0; s_funtil = 0; }
        else if (eq(buf, "reset")) { s_fault = F_NONE; s_hold = 0; s_jumps = 0; }
        else if (eq(buf, "pump_on")) want_on = 1;
        else if (eq(buf, "pump_off")) want_off = 1;
        else if (eq(buf, "discover")) s_disc = 0;
    }

    // ---- pump control ----
    const char *act = "none", *why = "";
    if (s_force && s_funtil && now >= (unsigned)s_funtil) { s_force = 0; s_funtil = 0; }          // forced period over
    if (s_force == 1 && sensor_ok && pm >= 1000) { s_force = 0; s_funtil = 0; }                    // overflow guard even when forced
    if (mq && s_force == 1) {                                                // forced ON: ignores auto mode, thresholds and faults
        pub(t_set, "ON", 0);
        if (!s_since) { s_since = (int)now; s_start = pm; }
        pump_on = 1; act = "force_on";
    } else if (mq && s_force == 2) {                                         // forced OFF: nothing starts the pump
        if (pump_on || !pump_known) { pub(t_set, "OFF", 0); s_lastoff = (int)now; }
        pump_on = 0; s_since = 0; act = "force_off";
    } else if (mq && pump_known) {
        if (pump_on) {
            if (!s_since) { s_since = (int)now; s_start = pm; }                 // started by someone else: adopt it
            unsigned run = now - (unsigned)s_since;
            int stop = 0;
            if (!sensor_ok && !blind) { stop = 1; why = "no valid level reading"; }
            else if (pm >= c_off * 10) { stop = 1; why = "level reached off_pct"; }
            else if (want_off) { stop = 1; why = "pump_off command"; s_hold = (int)now + c_rest * 60; }
            else if (run > (unsigned)c_maxrun * 60) { stop = 1; why = "max_run exceeded"; s_fault = F_MAXRUN; }
            else if (sensor_ok && run >= (unsigned)c_flowmin * 60 && (c_minin > 0 ? pm_run_dl(pm, (int)run) * 6000 / (int)run < c_minin * 100 : pm - s_start < c_rise)) { stop = 1; why = "no flow"; s_fault = F_NOFLOW; }
            if (stop) { pub(t_set, "OFF", 0); s_since = 0; s_lastoff = (int)now; act = "stop"; }
            else { pub(t_set, "ON", 0); act = "run"; }                          // keep-alive for the relay's auto-off timer
        } else {
            s_since = 0;
            int can = sensor_ok && s_good >= c_goodn && s_fault == F_NONE && online && pm < c_off * 10;     // 3 good readings in a row before the first start
            int rested = now - (unsigned)s_lastoff >= (unsigned)c_rest * 60 && (unsigned)s_hold <= now;
            if (can && ((c_auto && pm <= c_on * 10 && rested) || want_on)) {
                pub(t_set, "ON", 0); s_since = (int)now; s_start = pm; act = "start"; pump_on = 1; why = want_on ? "pump_on command" : "level at/below on_pct";
            }
        }
    }
    if (blind && sens_logged && s_since) wlogx('W', "pump keeps running without a level reading (blind_s=%d)", c_blind);
    if ((act[0] == 's' && act[1] != 'o') || (pump_known && (lo & 128) && pump_on != !!(lo & 8)))          // start / stop, or the pump state changed (forced, adopted, switched elsewhere)
        wlogx('I', "pump %s: %s%s%s, level %d.%d%% dist %d.%d cm, run %us", pump_on ? "ON" : "OFF", act, why[0] ? " - " : "", why, pm / 10, pm % 10, d / 10, d % 10,
             was_since ? now - (unsigned)was_since : 0u);
    if (s_fault != prev_fault) {
        if (s_fault == F_NONE) wlogx('I', "fault %s cleared", FAULTS[prev_fault]);
        else if (s_fault == F_MAXRUN) wlogx('E', "fault max_run: pump ran %us (limit %d min), level %d.%d%% -> %d.%d%% %s", now - (unsigned)was_since, c_maxrun, s_start / 10, s_start % 10, pm / 10, pm % 10, ctx());
        else if (s_fault == F_NOFLOW) wlogx('E', "fault no_flow: after %us the level moved %d.%d%% -> %d.%d%% (needs +%d per mille in %d min), intake %d ml/min, relay %s %s", now - (unsigned)was_since,
                                           s_start / 10, s_start % 10, pm / 10, pm % 10, c_rise, c_flowmin, was_since && now > (unsigned)was_since ? pm_run_dl(pm, (int)(now - (unsigned)was_since)) * 6000 / (int)(now - (unsigned)was_since) : 0,
                                           pump_known ? (pump_on ? "ON" : "OFF") : "unknown", ctx());
        else wlogx('E', "fault %s latched: no valid distance reading for %us (pump %s) %s", FAULTS[s_fault], s_goodlast ? now - (unsigned)s_goodlast : 0u, s_since ? "ON" : "off", ctx());
    }
    s_lastrun = (int)now;
    if (pump_on && prev_run && (int)now > prev_run) s_pps += (int)now - prev_run > 60 ? 60 : (int)now - prev_run;

    // ---- publish ----
    if (!sensor_ok) { d = c_empty; pm = s_ema >= 0 ? s_ema : 0; }
    static char js[700];
    mk_state(js, sizeof js, pm, d, pump_on ? "ON" : "OFF", c_auto);
    write_file(STATE_JSON, js);
    if (mq) {
        if (c_ha[0] && s_disc != DISC_VER) discovery();
        topic("%s/state"); pub(tb, js, 1);
        topic("%s/auto/state"); pub(tb, c_auto ? "ON" : "OFF", 1);
    }
    if (was_since && !s_since && sys_time() && (int)now - was_since >= 30) {        // a pump run just ended: remember what it delivered
        int dur = (int)now - was_since, added = pm_run_dl(pm, dur);
        if (added < 0) added = 0;
        s_lrl = added; s_lrs = dur; s_lrt = was_since; s_dr++;
        wlog('I', "pump run ended: %d s, +%d.%d L (start %d.%d%% -> %d.%d%%)", dur, added / 10, added % 10, s_start / 10, s_start % 10, pm / 10, pm % 10);
        char l[40]; int k = m_snprintf(l, sizeof l, "%d,%d,%d\n", was_since, dur, added);
        append_line(RUNS, RUNS_OLD, 8000, l, k);
    }
    history(pm, pump_on);
    // ---- log: errors of this cycle, low memory, periodic status ----
    if (g_pubfail && !(lo & 16)) wlogx('E', "mqtt publish failed: %s rc %d %s", g_pubt, g_pubrc, ctx());
    else if (!g_pubfail && (lo & 16)) wlogx('I', "mqtt publishing works again");
    if (g_fserr && !(lo & 32)) wlogx('E', "file write failed: %s (fs used %d of %d KB) %s", g_fspath, sys_sysinfo(SI_FS_USED) / 1024, sys_sysinfo(SI_FS_TOTAL) / 1024, ctx());
    else if (!g_fserr && (lo & 32)) wlogx('I', "file writes work again");
    if ((sys_sysinfo(SI_DRAM_FREE) < 20000 || sys_sysinfo(SI_DRAM_LARGEST) < 8192) && now - (unsigned)s_lmem >= 600) { s_lmem = (int)now; wlogx('W', "low memory %s", ctx()); }
    if (!s_lhb || now - (unsigned)s_lhb >= 1800) {
        s_lhb = (int)now;
        wlogx('I', "status: level %d.%d%% (%d L) dist %d.%d cm pump %s auto %s fault %s runs today %d mqtt %s %s", pm / 10, pm % 10, c_vol * pm / 1000, d / 10, d % 10, pump_on ? "ON" : "OFF",
             c_auto ? "ON" : "OFF", FAULTS[s_fault], s_dr, mq ? (pump_known ? "ok" : "no-pump-state") : "down", ctx());
    }
    s_lo = 128 | (mq << 1) | (online << 2) | (pump_known ? (pump_on && !(act[0] == 's' && act[2] == 'o')) << 3 : lo & 8) | (g_pubfail << 4) | (g_fserr << 5);
    save("tank.st", STT, N(STT));
    if (!g_quiet) m_printf("level %d.%d%% (%d L) dist %d.%d cm pump %s auto %s fault %s act %s mqtt %s\n", pm / 10, pm % 10, c_vol * pm / 1000, d / 10, d % 10,
             pump_on ? "ON" : "OFF", c_auto ? "ON" : "OFF", FAULTS[s_fault], act, mq ? (pump_known ? "ok" : "no-pump-state") : "down");
    g_local_cmd[0] = 0;
    return 0;
}

static void show_cfg(void)
{
    for (int i = 0; i < N(CFG); i++) {
        if (CFG[i].n) m_printf("%s=%s\n", CFG[i].k, (const char *)CFG[i].p);
        else m_printf("%s=%d\n", CFG[i].k, *(int *)CFG[i].p);
    }
}

static char s_cfg_prev[700];                                  // the configuration as last saved ("key=value" lines)
static int cfg_text(char *buf, int n)
{
    int o = 0;
    for (int i = 0; i < N(CFG) && o < n - 40; i++) {
        if (CFG[i].n) o += m_snprintf(buf + o, n - o, "%s=%s\n", CFG[i].k, (const char *)CFG[i].p);
        else o += m_snprintf(buf + o, n - o, "%s=%d\n", CFG[i].k, *(int *)CFG[i].p);
    }
    return o;
}
static void cfg_apply_text(char *t)                          // k=v lines -> variables
{
    for (char *p = t; *p;) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        char *q = p; while (*q && *q != '=') q++;
        if (*q) { *q = 0; apply(CFG, N(CFG), p, q + 1); }
        p = more ? e + 1 : e;
    }
}
static const char *cfg_validate_save_inner(void);
// Validate the in-memory configuration and store it; returns an error text or NULL. A rejected change is undone and logged; an accepted one is logged key by key.
static const char *cfg_validate_save(void)
{
    char now[700], prev[700];
    strcpy(prev, s_cfg_prev);
    const char *err = cfg_validate_save_inner();
    if (err) {
        wlog('W', "configuration change rejected: %s", err);
        cfg_apply_text(prev); derive();                      // back to what is in force
        return err;
    }
    cfg_text(now, sizeof now);
    int changes = 0;
    for (char *p = now; *p;) {                               // log every key whose value differs from the previous configuration
        char *e = p; while (*e && *e != '\n') e++;
        char save_c = *e; *e = 0;
        char *q = p; while (*q && *q != '=') q++;
        if (*q) {
            *q = 0;
            char want[80], key[24]; m_snprintf(key, sizeof key, "%s=", p);
            const char *old = NULL;
            for (const char *r = prev; *r;) { const char *re = strchr(r, '\n'); if (!strncmp(r, key, strlen(key))) { old = r + strlen(key); m_snprintf(want, sizeof want, "%.*s", re ? (int)(re - old) : (int)strlen(old), old); break; } r = re ? re + 1 : r + strlen(r); }
            if (!old || strcmp(want, q + 1)) { wlog('I', "config %s: %s -> %s", p, old ? want : "(unset)", q + 1); changes++; }
            *q = '=';
        }
        *e = save_c;
        p = save_c ? e + 1 : e;
    }
    if (!changes) wlog('I', "configuration saved (no value changed)");
    strcpy(s_cfg_prev, now);
    return NULL;
}
static const char *cfg_validate_save_inner(void)
{
    if (c_ml < 0 || (c_ml > 0 && c_ml < 100)) return "ml_cm must be 0 (use empty_mm) or at least 100";
    derive();
    if (c_full < 25) return "full_mm must be at least 25 (sensor dead zone)";
    if (c_empty <= c_full + 50) return "empty_mm must be larger than full_mm by more than 50";
    if (c_on >= c_off || c_off > 100 || c_on < 0) return "need 0 <= on_pct < off_pct <= 100";
    if (c_vol < 1 || c_vol > 200000) return "vol_l must be 1..200000";
    if (c_maxrun < 1 || c_rest < 0 || c_flowmin < 1 || c_hist < 10) return "max_run/flow_min >= 1, rest >= 0, hist >= 10";
    if (c_interval < 5 || c_interval > 3600) return "interval must be 5..3600 seconds";
    if (c_samples < 3 || c_samples > 15) return "samples must be 3..15";
    if (c_jump < 5 || c_jump > 100) return "jump_pct must be 5..100";
    if (c_goodn < 1 || c_goodn > 20) return "good_n must be 1..20";
    if (c_blind < 0 || c_blind > 600) return "blind_s must be 0..600";
    if (c_minin < 0 || c_minin > 2000) return "min_in must be 0..2000 (tenths of L/min)";
    if (c_histkb < 4 || c_histkb > 64) return "hist_kb must be 4..64";
    if (c_tempc < -30 || c_tempc > 60) return "temp_c must be -30..60";
    if (!c_name[0] || !c_pump[0]) return "name and pump must not be empty";
    save("tank.cfg", CFG, N(CFG));
    s_disc = 0; s_ema = -1; s_jumps = 0; s_good = 0; s_pv = 0; s_pps = 0;
    save("tank.st", STT, N(STT));
    return NULL;
}

// Apply key=value arguments argv[first..last); returns an error text or NULL. Nothing is saved unless every value is consistent.
static const char *set_cfg(int last, char **argv, int first)
{
    static char err[80];
    for (int i = first; i < last; i++) {
        char *a = argv[i], *e = a; while (*e && *e != '=') e++;
        if (!*e) return "expected key=value";
        *e = 0;
        if (find_key(CFG, N(CFG), a) < 0) { m_snprintf(err, sizeof err, "unknown key %s", a); return err; }
        apply(CFG, N(CFG), a, e + 1);
    }
    return cfg_validate_save();
}


#ifndef NO_WEB          // only the web page's copy (/www/cgi-bin/wtms.aot) serves the dashboard API: the service and the command line are smaller without it
/* ---------------- web (CGI) interface: /cgi-bin/wtms?web+SUB+TOKEN+args ---------------- */
// Login: the page sends the password hex-encoded (the CGI argument charset is limited); a random token (valid 7 days, max 6 sessions)
// is returned and must accompany every other call. The connection is plain HTTP: use this on a trusted LAN only.
static void jout(const char *body) { m_puts("Content-Type: application/json\n\n"); m_puts(body); m_puts("\n"); }

static int unhex(const char *h, char *out, int n)           // hex string -> bytes (NUL terminated), length or -1
{
    int l = (int)m_strlen(h);
    if (l % 2 || l / 2 >= n) return -1;
    for (int i = 0; i < l; i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char ch = h[i + k]; v = v * 16 + (ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 0); }
        out[i / 2] = (char)v;
    }
    out[l / 2] = 0;
    return l / 2;
}

static int toks_load(char *buf, int n)                       // live tokens as "tok exp\n" lines
{
    char raw[400]; unsigned now = tnow(); int o = 0;
    int len = sys_kv_get("tank.tok", raw, sizeof raw - 1);
    if (len <= 0) { buf[0] = 0; return 0; }
    raw[len] = 0;
    for (char *p = raw; *p;) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        char *sp = p; while (*sp && *sp != ' ') sp++;
        if (*sp && (unsigned)m_atoi(sp + 1) > now) o += m_snprintf(buf + o, n - o, "%s\n", p);
        p = more ? e + 1 : e;
    }
    return o;
}
static int tok_check(const char *t)
{
    char buf[400];
    int len = (int)m_strlen(t);
    if (len < 16 || len > 40) return 0;
    toks_load(buf, sizeof buf);
    for (char *p = buf; *p;) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        char *sp = p; while (*sp && *sp != ' ') sp++;
        if (*sp) { *sp = 0; if (eq(p, t)) return 1; }
        p = more ? e + 1 : e;
    }
    return 0;
}
static void tok_new(char *out)                                // 24 hex chars; stored with a 7 day expiry
{
    char buf[400];
    for (int i = 0; i < 3; i++) m_snprintf(out + i * 8, 9, "%08x", sys_random());
    int o = toks_load(buf, sizeof buf);
    char *first = buf; int lines = 0;
    for (char *p = buf; *p; p++) if (*p == '\n') lines++;
    while (lines >= 6) { while (*first != '\n') first++; first++; lines--; }   // drop the oldest session
    o = (int)m_strlen(first);
    if (first != buf) { for (int i = 0; i <= o; i++) buf[i] = first[i]; }
    o += m_snprintf(buf + o, (int)sizeof buf - o, "%s %u\n", out, tnow() + 7 * 86400);
    sys_kv_set("tank.tok", buf, o);
}
static void tok_del(const char *t)
{
    char buf[400], keep[400]; int o = 0;
    toks_load(buf, sizeof buf);
    for (char *p = buf; *p;) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        if (!(m_strlen(p) > m_strlen(t) && p[m_strlen(t)] == ' ' && !memcmp(p, t, m_strlen(t)))) o += m_snprintf(keep + o, (int)sizeof keep - o, "%s\n", p);
        p = more ? e + 1 : e;
    }
    sys_kv_set("tank.tok", keep, o);
}

static int do_act(const char *cmd)                           // run a control command now and answer with the fresh state
{
    int n = 0; while (cmd[n] && n < (int)sizeof g_local_cmd - 1) { g_local_cmd[n] = cmd[n]; n++; }
    g_local_cmd[n] = 0;
    g_quiet = 1;
    run_cycle();
    char js[700]; int fd = sys_open(STATE_JSON, 0), k = 0;
    if (fd >= 0) { k = sys_read(fd, js, sizeof js - 1); sys_close(fd); }
    if (k < 0) k = 0;
    js[k] = 0;
    jout(k ? js : "{}");
    return 0;
}

static int web(int argc, char **argv)
{
    while (argc > 3 && argv[argc - 1][0] == '_' && argv[argc - 1][1] == '=') argc--;     // ignore a cache-busting "&_=123" the browser added
    const char *sub = argc > 2 ? argv[2] : "";
    char pw[64], in[64], tok[32];
    int have = sys_kv_get("tank.pw", pw, sizeof pw - 1);
    if (have > 0) pw[have] = 0;
    if (eq(sub, "login")) {
        if (have <= 0) { jout("{\"ok\":false,\"error\":\"nopw\"}"); return 0; }
        int l = argc > 3 ? unhex(argv[3], in, sizeof in) : -1;
        if (l != have || memcmp(in, pw, have)) { sys_sleep_ms(1200); jout("{\"ok\":false,\"error\":\"bad\"}"); return 0; }   // the delay slows guessing
        tok_new(tok); char o[80]; m_snprintf(o, sizeof o, "{\"ok\":true,\"token\":\"%s\"}", tok); jout(o); return 0;
    }
    if (eq(sub, "setup")) {                                    // first-time password, only while none is set
        int l = argc > 3 ? unhex(argv[3], in, sizeof in) : -1;
        if (have > 0) { jout("{\"ok\":false,\"error\":\"exists\"}"); return 0; }
        if (l < 6) { jout("{\"ok\":false,\"error\":\"short\"}"); return 0; }
        sys_kv_set("tank.pw", in, l);
        tok_new(tok); char o[80]; m_snprintf(o, sizeof o, "{\"ok\":true,\"token\":\"%s\"}", tok); jout(o); return 0;
    }
    if (eq(sub, "status")) { jout(have > 0 ? "{\"ok\":true,\"pw\":true}" : "{\"ok\":true,\"pw\":false}"); return 0; }
    const char *t = argc > 3 ? argv[3] : "";
    if (!tok_check(t)) { jout("{\"ok\":false,\"error\":\"auth\"}"); return 0; }
    if (eq(sub, "me")) { jout("{\"ok\":true}"); return 0; }
    if (eq(sub, "logout")) { tok_del(t); jout("{\"ok\":true}"); return 0; }
    if (eq(sub, "passwd") && argc > 5) {
        int lo = unhex(argv[4], in, sizeof in);
        if (lo != have || memcmp(in, pw, have)) { sys_sleep_ms(1200); jout("{\"ok\":false,\"error\":\"bad\"}"); return 0; }
        char nw[64]; int ln = unhex(argv[5], nw, sizeof nw);
        if (ln < 6) { jout("{\"ok\":false,\"error\":\"short\"}"); return 0; }
        sys_kv_set("tank.pw", nw, ln); sys_kv_del("tank.tok"); tok_new(tok);
        char o[80]; m_snprintf(o, sizeof o, "{\"ok\":true,\"token\":\"%s\"}", tok); jout(o); return 0;
    }
    if (eq(sub, "get")) {
        m_puts("Content-Type: application/json\n\n{");
        for (int i = 0; i < N(CFG); i++) {
            if (CFG[i].n) { m_printf("\"%s\":\"", CFG[i].k); for (const char *s = CFG[i].p; *s; s++) { if (*s == '"' || *s == '\\') m_puts("\\"); char ch[2] = { *s, 0 }; m_puts(ch); } m_puts("\","); }
            else m_printf("\"%s\":%d,", CFG[i].k, *(int *)CFG[i].p);
        }
        m_printf("\"fault_now\":\"%s\"}\n", FAULTS[s_fault]);
        return 0;
    }
    // The CGI query is limited to ~240 characters, so long forms are sent in pieces ending with "end" (a cut-off request is rejected):
    //   web stage TOKEN new end | web stage TOKEN k=v ... end (any number of times) | web commit TOKEN end
    if (eq(sub, "set") || eq(sub, "stage") || eq(sub, "commit")) {
        if (argc < 5 || !eq(argv[argc - 1], "end")) { jout("{\"ok\":false,\"error\":\"request truncated\"}"); return 0; }
        int last = argc - 1;
        if (eq(sub, "set")) {
            const char *err = set_cfg(last, argv, 4);
            if (err) { char o[140]; m_snprintf(o, sizeof o, "{\"ok\":false,\"error\":\"%s\"}", err); jout(o); } else jout("{\"ok\":true}");
            return 0;
        }
        char pend[700]; int pl = sys_kv_get("tank.pend", pend, sizeof pend - 1);
        if (pl < 0) pl = 0;
        if (eq(sub, "stage")) {
            for (int i = 4; i < last; i++) {
                if (eq(argv[i], "new")) { pl = 0; continue; }
                char *e = argv[i]; while (*e && *e != '=') e++;
                if (!*e) { jout("{\"ok\":false,\"error\":\"expected key=value\"}"); return 0; }
                *e = 0; int known = find_key(CFG, N(CFG), argv[i]) >= 0; *e = '=';
                if (!known) { jout("{\"ok\":false,\"error\":\"unknown key\"}"); return 0; }
                pl += m_snprintf(pend + pl, (int)sizeof pend - pl, "%s\n", argv[i]);
                if (pl >= (int)sizeof pend - 1) { jout("{\"ok\":false,\"error\":\"too much data\"}"); return 0; }
            }
            sys_kv_set("tank.pend", pend, pl); jout("{\"ok\":true}"); return 0;
        }
        pend[pl] = 0;                                              // commit
        for (char *p = pend; *p;) {
            char *e = p; while (*e && *e != '\n') e++;
            int more = *e; *e = 0;
            char *q = p; while (*q && *q != '=') q++;
            if (*q) { *q = 0; apply(CFG, N(CFG), p, q + 1); }
            p = more ? e + 1 : e;
        }
        sys_kv_del("tank.pend");
        const char *err = pl ? cfg_validate_save() : "nothing staged";
        if (err) { char o[140]; m_snprintf(o, sizeof o, "{\"ok\":false,\"error\":\"%s\"}", err); jout(o); } else jout("{\"ok\":true}");
        return 0;
    }
    if (eq(sub, "mqttget")) {
        char h[64], u[32], id[48], p[8], o[260];
        sys_mqtt_info(0, h, sizeof h); sys_mqtt_info(1, u, sizeof u); sys_mqtt_info(2, id, sizeof id); sys_mqtt_info(3, p, sizeof p);
        m_snprintf(o, sizeof o, "{\"ok\":true,\"host\":\"%s\",\"port\":%s,\"user\":\"%s\",\"client\":\"%s\",\"state\":%d}", h, p, u, id, sys_mqtt_state());
        jout(o); return 0;
    }
    if (eq(sub, "mqttset") && argc >= 9 && eq(argv[argc - 1], "end")) {      // mqttset TOKEN host port hexuser hexpass end
        char u[40], p[40];
        int lu = unhex(argv[6], u, sizeof u), lp = unhex(argv[7], p, sizeof p);
        int port = m_atoi(argv[5]);
        if (lu < 0 || lp < 0 || port < 1 || port > 65535 || !argv[4][0]) { jout("{\"ok\":false,\"error\":\"bad broker settings\"}"); return 0; }
        jout(sys_mqtt_config(argv[4], port, u, p, "") == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}");
        return 0;
    }
    if (eq(sub, "readj")) {
        int d = measure();
        if (d < 0) { wlog('W', "sensor read (web): failed - %s; valid %d/%d, no-echo %d, stuck %d, out-of-window %d, echo us min/med/max %d/%d/%d, agreeing %d", WHY[g_mwhy], g_mn, c_samples, g_mto, g_mstuck, g_mbad, g_mlo, g_mmed, g_mhi, g_magree); jout("{\"ok\":false}"); return 0; }
        int pm = level_pm(d); char o[120];
        wlog('I', "sensor read (web): distance %d mm, level %d.%d%% (%d L), echo us min/med/max %d/%d/%d", d, pm / 10, pm % 10, c_vol * pm / 1000, g_mlo, g_mmed, g_mhi);
        m_snprintf(o, sizeof o, "{\"ok\":true,\"distance_mm\":%d,\"level\":%d.%d,\"litres\":%d}", d, pm / 10, pm % 10, c_vol * pm / 1000);
        jout(o); return 0;
    }
    if (eq(sub, "auto") && argc > 4) {
        c_auto = eq(argv[4], "on");
        wlog('I', "auto mode %s (web)", c_auto ? "ON" : "OFF");
        save("tank.cfg", CFG, N(CFG));
        return do_act("noop");
    }
    if (eq(sub, "act") && argc > 4) return do_act(argv[4]);
    jout("{\"ok\":false,\"error\":\"unknown\"}");
    return 0;
}

#endif  // NO_WEB

#ifndef NO_SETUP        // the CGI copy (/www/cgi-bin/wtms.aot) is built without setup: smaller, so it also loads while an SSH session fragments RAM
/* ---------------- setup: interactive or headless configuration, files and service ---------------- */
// The package manager (pkg install wtms) already put the program and the web pages in place; setup stores the configuration,
// connects the MQTT broker, optionally moves the web server to another port, sets the web login password, creates the
// `wtms` service (every 5 s) and checks the sensor. Nothing is changed until all answers are given and valid.
static char su_mqh[64], su_mqu[32], su_mqp[40], su_tz[16], su_pw[40];
static int su_mqport, su_mqset, su_webport, su_start = -1;           // su_start: -1 ask (interactive) / no (headless), 0 no, 1 yes
static int su_shape = 0, su_a, su_b, su_ht, su_lit, su_fullcm = -1, su_emptycm = -1;

static int has(const char *hay, const char *needle)
{
    for (; *hay; hay++) { const char *a = hay, *b = needle; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}

static int rd_line(char *buf, int n)                                // one edited line without the newline; -1 on Ctrl-C / end of input
{
    int r = sys_read(0, buf, n - 1);
    if (r <= 0) return -1;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r')) r--;
    buf[r] = 0;
    return r;
}
static void copy_to(char *dst, int n, const char *s) { int i = 0; while (s[i] && i < n - 1) { dst[i] = s[i]; i++; } dst[i] = 0; }

static int ask(const char *q, char *out, int n, const char *def)   // Enter keeps `def`; -1 aborts
{
    char b[100];
    m_printf("%s%s%s%s: ", q, def && *def ? " [" : "", def && *def ? def : "", def && *def ? "]" : "");
    if (rd_line(b, sizeof b) < 0) return -1;
    copy_to(out, n, b[0] ? b : (def ? def : ""));
    return 0;
}
static int ask_int(const char *q, int *v, int lo, int hi)           // loops until a number in range is given
{
    char d[16], b[24];
    for (;;) {
        m_snprintf(d, sizeof d, "%d", *v);
        if (ask(q, b, sizeof b, d) < 0) return -1;
        int ok = b[0] != 0; for (int i = 0; b[i]; i++) if (!((b[i] >= '0' && b[i] <= '9') || (i == 0 && b[i] == '-' && b[1]))) ok = 0;
        int x = m_atoi(b);
        if (ok && x >= lo && x <= hi) { *v = x; return 0; }
        m_printf("  please enter a number from %d to %d\n", lo, hi);
    }
}
static int ask_secret(const char *q, char *out, int n)               // no echo; -1 on Ctrl-C
{
    int o = 0;
    m_printf("%s: ", q);
    for (;;) {
        int k = sys_getkey(60000);
        if (k == -1) continue;
        if (k < 0 && k != -1) return -1;                              // -2 end of input, -3 Ctrl-C
        if (k == '\r' || k == '\n') break;
        if ((k == 127 || k == 8) && o) o--;
        else if (k >= 32 && k < 127 && o < n - 1) out[o++] = (char)k;
    }
    out[o] = 0; m_puts("\n");
    return 0;
}
static int ask_yn(const char *q, int def)                             // 1 / 0, -1 aborts; Enter gives `def`
{
    char b[8];
    m_printf("%s [%s]: ", q, def ? "Y/n" : "y/N");
    if (rd_line(b, sizeof b) < 0) return -1;
    if (b[0] == 'Y' || b[0] == 'y') return 1;
    if (b[0] == 'N' || b[0] == 'n') return 0;
    return def;
}

static void sys_cmd(const char *a, const char *b, const char *c, const char *d, const char *e, const char *f, const char *g, const char *h, const char *i, const char *j, const char *k)
{
    // run a built-in shell command through sys_run (built-ins need no second program memory): words are NUL separated
    const char *wv[11] = { a, b, c, d, e, f, g, h, i, j, k };
    char blob[300]; int n = 0, argc = 0;
    for (int x = 0; x < 11 && wv[x]; x++) { int l = (int)m_strlen(wv[x]) + 1; if (n + l > (int)sizeof blob) break; for (int y = 0; y < l; y++) blob[n + y] = wv[x][y]; n += l; argc++; }
    sys_run(blob, n, argc, 0, 1, 2);
}

static void shape_volume(void)                                       // litres from the tank dimensions
{
    if (su_shape == 'r') { c_vol = su_a * su_b / 10 * su_ht / 100; c_ml = su_a * su_b; }
    else if (su_shape == 'c') { c_vol = (int)((long long)785 * su_a * su_a / 1000 * su_ht / 1000); c_ml = (int)((long long)785 * su_a * su_a / 1000); }
    else if (su_shape == 'l') { c_vol = su_lit; c_ml = su_ht > 0 ? (int)((long long)su_lit * 1000 / su_ht) : 0; }
    if (c_vol < 1) c_vol = 1;
}

static int collect_interactive(void)
{
    char b[64];
    m_puts("== wtms setup ==  (Enter keeps the value in [brackets], Ctrl-C cancels without changing anything)\n");
    m_puts("\n-- Web dashboard --\n");
    su_webport = 80; { char p[8]; int n = sys_kv_get("www.port", p, sizeof p - 1); if (n > 0) { p[n] = 0; su_webport = m_atoi(p); } }
    if (su_webport < 1 || su_webport > 65535) su_webport = 80;
    if (ask_int("Web server port (the dashboard is http://<board>:PORT/tank/)", &su_webport, 1, 65535) < 0) return -1;
    if (su_webport == 22) { m_puts("  22 is the SSH port: using 80\n"); su_webport = 80; }
    for (;;) {
        if (ask_secret("Dashboard login password (6+ characters; empty = keep the current / set it on the first visit)", su_pw, sizeof su_pw) < 0) return -1;
        if (!su_pw[0] || m_strlen(su_pw) >= 6) break;
        m_puts("  at least 6 characters\n");
    }
    m_puts("\n-- MQTT broker (the one the relay node uses) --\n");
    char h[64] = "", u[32] = "", id[48], p[8] = "1883";
    sys_mqtt_info(0, h, sizeof h); sys_mqtt_info(1, u, sizeof u); sys_mqtt_info(2, id, sizeof id); sys_mqtt_info(3, p, sizeof p);
    if (!h[0]) copy_to(h, sizeof h, "192.168.0.50");
    if (ask("Broker host/IP", su_mqh, sizeof su_mqh, h) < 0) return -1;
    su_mqport = m_atoi(p) > 0 ? m_atoi(p) : 1883;
    if (ask_int("Broker port", &su_mqport, 1, 65535) < 0) return -1;
    if (ask("Broker user", su_mqu, sizeof su_mqu, u[0] ? u : "relay") < 0) return -1;
    if (ask_secret("Broker password (empty = keep the one already saved)", su_mqp, sizeof su_mqp) < 0) return -1;
    su_mqset = su_mqp[0] != 0 || !eq(su_mqh, h) || su_mqport != m_atoi(p) || !eq(su_mqu, u);
    if (su_mqset && !su_mqp[0]) { m_puts("  the broker changed: its password is needed\n"); if (ask_secret("Broker password", su_mqp, sizeof su_mqp) < 0) return -1; }
    m_puts("\n-- Pump relay --\n");
    if (ask("Pump topic prefix (relay node)", c_pump, sizeof c_pump, c_pump) < 0) return -1;
    m_puts("\n-- Tank --\n");
    if (ask("Name / MQTT prefix for this tank", c_name, sizeof c_name, c_name) < 0) return -1;
    if (ask("Tank shape: r = rectangular, c = cylinder, l = I type the litres", b, sizeof b, "r") < 0) return -1;
    su_shape = b[0] == 'c' ? 'c' : b[0] == 'l' ? 'l' : 'r';
    su_a = 100; su_b = 100; su_ht = 100; su_lit = c_vol;
    if (su_shape == 'r') { if (ask_int("Inside length (cm)", &su_a, 1, 2000) < 0 || ask_int("Inside width (cm)", &su_b, 1, 2000) < 0 || ask_int("Max water height (cm)", &su_ht, 1, 2000) < 0) return -1; }
    else if (su_shape == 'c') { if (ask_int("Inside diameter (cm)", &su_a, 1, 2000) < 0 || ask_int("Max water height (cm)", &su_ht, 1, 2000) < 0) return -1; }
    else { if (ask_int("Litres when full", &su_lit, 1, 1000000) < 0 || ask_int("Max water height (cm)", &su_ht, 1, 2000) < 0) return -1; }
    shape_volume();
    m_printf("Capacity: %d L\n", c_vol);
    m_puts("Mount the HC-SR04 at the top, facing down. Distances from the sensor face to the water surface:\n");
    su_fullcm = c_full / 10; su_emptycm = su_ht + su_fullcm;
    if (ask_int("  ...when the tank is FULL (cm, at least 3)", &su_fullcm, 3, 500) < 0) return -1;
    su_emptycm = su_ht + su_fullcm;
    if (ask_int("  ...when the tank is EMPTY, to the tank floor (cm)", &su_emptycm, su_fullcm + 6, 600) < 0) return -1;
    if (ask_int("Sensor TRIG GPIO", &c_trig, 0, 39) < 0 || ask_int("Sensor ECHO GPIO (through a 5 V -> 3.3 V divider!)", &c_echo, 0, 39) < 0) return -1;
    if (ask_int("DS18B20 temperature sensor GPIO (0 = none)", &c_temp, 0, 39) < 0) return -1;
    if (ask_int("Start the pump at or below (%)", &c_on, 0, 99) < 0 || ask_int("Stop the pump at or above (%)", &c_off, 1, 100) < 0) return -1;
    if (ask_int("Longest allowed pump run (minutes)", &c_maxrun, 1, 1440) < 0) return -1;
    if (ask("Time zone offset from UTC (e.g. +6, -5:30; empty = unchanged)", su_tz, sizeof su_tz, "") < 0) return -1;
    if (ask("Home Assistant MQTT discovery prefix (empty = off)", c_ha, sizeof c_ha, c_ha) < 0) return -1;
    m_puts("\n");
    return 0;
}

// headless: key=value arguments (setup keys below plus every `wtms config` key); returns an error text or NULL
static const char *collect_headless(int argc, char **argv, int first)
{
    static char err[80];
    su_webport = 0;
    for (int i = first; i < argc; i++) {
        if (eq(argv[i], "--headless")) continue;
        if (eq(argv[i], "--start")) { su_start = 1; continue; }
        char *a = argv[i], *e = a; while (*e && *e != '=') e++;
        if (!*e) { m_snprintf(err, sizeof err, "expected key=value, got %s", a); return err; }
        *e = 0; const char *v = e + 1;
        if (eq(a, "mqtt_host")) { copy_to(su_mqh, sizeof su_mqh, v); su_mqset = 1; }
        else if (eq(a, "mqtt_port")) { su_mqport = m_atoi(v); su_mqset = 1; }
        else if (eq(a, "mqtt_user")) { copy_to(su_mqu, sizeof su_mqu, v); su_mqset = 1; }
        else if (eq(a, "mqtt_pass")) { copy_to(su_mqp, sizeof su_mqp, v); su_mqset = 1; }
        else if (eq(a, "web_port")) su_webport = m_atoi(v);
        else if (eq(a, "web_pw")) copy_to(su_pw, sizeof su_pw, v);
        else if (eq(a, "tz")) copy_to(su_tz, sizeof su_tz, v);
        else if (eq(a, "start")) su_start = m_atoi(v) ? 1 : 0;
        else if (eq(a, "shape")) su_shape = v[0] == 'c' ? 'c' : v[0] == 'l' ? 'l' : 'r';
        else if (eq(a, "length")) su_a = m_atoi(v);
        else if (eq(a, "width")) su_b = m_atoi(v);
        else if (eq(a, "diameter")) su_a = m_atoi(v);
        else if (eq(a, "height")) su_ht = m_atoi(v);
        else if (eq(a, "litres")) su_lit = m_atoi(v);
        else if (eq(a, "full_cm")) su_fullcm = m_atoi(v);
        else if (eq(a, "empty_cm")) su_emptycm = m_atoi(v);
        else if (find_key(CFG, N(CFG), a) >= 0) apply(CFG, N(CFG), a, v);
        else { m_snprintf(err, sizeof err, "unknown setting %s", a); return err; }
    }
    if (su_shape) shape_volume();
    if (su_fullcm >= 0) c_full = su_fullcm * 10;
    if (su_emptycm >= 0) c_empty = su_emptycm * 10;
    else if (su_fullcm >= 0 && su_shape) c_empty = (su_ht + su_fullcm) * 10;
    if (su_mqset) {                                                  // missing parts of the broker settings come from the current ones
        char h[64] = "", u[32] = "", id[48], p[8] = "1883";
        sys_mqtt_info(0, h, sizeof h); sys_mqtt_info(1, u, sizeof u); sys_mqtt_info(2, id, sizeof id); sys_mqtt_info(3, p, sizeof p);
        if (!su_mqh[0]) copy_to(su_mqh, sizeof su_mqh, h);
        if (!su_mqu[0]) copy_to(su_mqu, sizeof su_mqu, u);
        if (su_mqport <= 0) su_mqport = m_atoi(p) > 0 ? m_atoi(p) : 1883;
        if (!su_mqh[0] || su_mqport > 65535) return "bad MQTT broker settings";
        if (!su_mqp[0]) return "mqtt_pass is required when the broker settings change";
    }
    if (su_webport && (su_webport < 1 || su_webport > 65535 || su_webport == 22)) return "web_port must be 1..65535 (not 22)";
    if (su_pw[0] && m_strlen(su_pw) < 6) return "web_pw must have at least 6 characters";
    return NULL;
}

static int setup_apply(int interactive)
{
    const char *err = cfg_validate_save();                           // validates first: nothing else is touched on error
    if (err) { m_eprintf("wtms: %s\n", err); return 2; }
    m_puts("== applying ==\n");
    sys_mkdir("/www"); sys_mkdir("/www/tank"); sys_mkdir("/www/cgi-bin");
    m_puts("ok: configuration saved\n");
    if (su_mqset) {
        if (sys_mqtt_config(su_mqh, su_mqport, su_mqu, su_mqp, "") == 0) m_printf("ok: MQTT broker %s:%d\n", su_mqh, su_mqport);
        else m_eputs("warning: the MQTT broker settings were rejected\n");
    }
    if (su_tz[0]) { if (sys_tz(su_tz) == 0) m_printf("ok: time zone %s\n", su_tz); else m_eputs("warning: bad time zone (use e.g. +6 or -5:30)\n"); }
    if (su_pw[0]) { sys_kv_set("tank.pw", su_pw, (int)m_strlen(su_pw)); sys_kv_del("tank.tok"); m_puts("ok: dashboard password set\n"); }
    int missing = sys_stat("/www/tank/index.html") != 1 || sys_stat("/www/cgi-bin/wtms.aot") != 1 || sys_stat("/esp/.local/bin/wtmsd.aot") != 1;
    if (missing) m_puts("warning: dashboard files are missing: run  pkg install wtms  (or  pkg fix)\n");
    int was_on = 0, exists = sys_stat("/etc/services/wtms.service") == 1;
    if (exists) {                                                    // an older unit ran the big `wtms run`: move it to the small wtmsd
        char u[300]; int fd = sys_open("/etc/services/wtms.service", 0), n = fd >= 0 ? sys_read(fd, u, sizeof u - 1) : 0;
        if (fd >= 0) sys_close(fd);
        if (n < 0) n = 0;
        u[n] = 0;
        if (has(u, "ExecStart=wtms run")) { was_on = has(u, "Enabled=true"); sys_cmd("service", "stop", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); sys_cmd("service", "rm", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); exists = 0; m_puts("ok: old service unit replaced\n"); }
    }
    if (!exists) {
        sys_cmd("service", "new", "wtms", "--every", "5", "--desc", "Water tank manager", "--", "wtmsd", "run", 0);
        m_puts("ok: service wtms created\n");
        if (was_on) { sys_cmd("service", "enable", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); sys_cmd("service", "start", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); m_puts("ok: it was enabled before: enabled and started again\n"); su_start = su_start < 0 ? 1 : su_start; }
    } else m_puts("ok: service wtms already exists\n");
    int portchg = 0;
    if (su_webport) {
        char p[8], cur[8] = "80"; int n = sys_kv_get("www.port", cur, sizeof cur - 1); if (n > 0) cur[n] = 0; else copy_to(cur, sizeof cur, "80");
        m_snprintf(p, sizeof p, "%d", su_webport);
        if (!eq(p, cur)) { sys_kv_set("www.port", p, (int)m_strlen(p)); portchg = 1; }
    }
    m_puts("\n== sensor check ==\n");
    int d = measure();
    int start = su_start;
    if (d < 0) {
        m_puts("no valid echo yet: check wiring (TRIG/ECHO pins, 5 V supply, echo divider). The pump will NOT be controlled until the sensor works.\n");
        if (interactive && start < 0) start = ask_yn("Enable the service anyway (it keeps the pump OFF until the sensor works)?", 0);
    } else {
        int pm = level_pm(d);
        m_printf("distance %d.%d cm -> level %d.%d%% (%d L)\n", d / 10, d % 10, pm / 10, pm % 10, c_vol * pm / 1000);
        if (interactive && start < 0) start = ask_yn("Does this match the real water level? Start the automatic pump control now?", 1);
    }
    if (start == 1) { sys_cmd("service", "enable", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); sys_cmd("service", "start", "wtms", 0, 0, 0, 0, 0, 0, 0, 0); m_puts("ok: service wtms enabled and started (runs every 5 s, starts at boot)\n"); }
    else m_puts("service wtms is not started. Start it with:  service enable wtms ; service start wtms\n");
    if (portchg) { m_printf("ok: web server moves to port %d now\n", su_webport); sys_cmd("service", "restart", "www", 0, 0, 0, 0, 0, 0, 0, 0); }
    char ip[20]; sys_netinfo(1, ip, sizeof ip);
    int wp = 80;
    { char p[8]; int n = sys_kv_get("www.port", p, sizeof p - 1); if (n > 0) { p[n] = 0; if (m_atoi(p) > 0) wp = m_atoi(p); } }
    if (su_webport) wp = su_webport;
    m_printf("\nDashboard : http://%s", ip); if (wp != 80) m_printf(":%d", wp); m_puts("/tank/   (log in there for pump control and Settings)\n");
    m_puts("Commands  : wtms status | wtms read | wtms config key=value | wtms cmd reset|force_on:30|force_off|release | service status wtms\n");
    m_puts("Safety    : set the relay node's auto-off timer for the pump to ~60 s (relay node web UI): wtms re-commands the pump every cycle, so the pump stops by itself if this board dies.\n");
    return 0;
}

static int do_setup(int argc, char **argv)
{
    int headless = 0;
    for (int i = 2; i < argc; i++) if (eq(argv[i], "--headless")) headless = 1;
    if (!headless && !sys_isatty(0)) { m_eputs("wtms: setup needs a terminal; use  wtms setup --headless key=value ...  (see the header of wtms.c / pkg docs)\n"); return 2; }
    if (headless) {
        const char *err = collect_headless(argc, argv, 2);
        if (err) { m_eprintf("wtms: %s\n", err); return 2; }
        return setup_apply(0);
    }
    if (collect_interactive() < 0) { m_puts("\ncancelled: nothing was changed\n"); load("tank.cfg", CFG, N(CFG)); return 1; }
    return setup_apply(1);
}

#endif

/* ---------------- entry points ---------------- */
static SemaphoreHandle_t s_mu;
static int s_inited;

void wtms_lock(void) { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
void wtms_unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

void wtms_init(void)
{
    load("tank.cfg", CFG, N(CFG));
    derive();
    cfg_text(s_cfg_prev, sizeof s_cfg_prev);
    load("tank.st", STT, N(STT));
    g_quiet = 1;
    s_inited = 1;
}

// one control cycle (the application task calls this every 5 s; the code itself runs a real cycle every `interval` seconds)
void wtms_cycle(void)
{
    if (!s_inited) return;
    wtms_lock();
    run_cycle();
    wtms_unlock();
}

// the dashboard API: argv = ["wtms", "web", SUB, TOKEN, args...]; the output (CGI style: "Content-Type: ...", blank line, body) goes to `out`
int wtms_web(int argc, char **argv, char *out, int cap)
{
    if (!s_inited) return 0;
    wtms_lock();
    compat_out_begin(out, cap);
    web(argc, argv);
    int n = compat_out_end();
    wtms_unlock();
    return n;
}

static volatile uint32_t s_cycles;
uint32_t wtms_cycles(void) { return s_cycles; }

// the application task: pinned to core 1 so the sonar echo interrupt and the control loop never compete with Wi-Fi / TCP / web / SSH (core 0)
static void app_task(void *arg)
{
    wtms_init();
    ESP_LOGI("wtms", "controller started on core %d", (int)xPortGetCoreID());
    for (;;) {
        sonar_service();
        wtms_cycle();
        s_cycles++;
        if (s_cycles <= 12) { ESP_LOGW("wtms", "cycle %u done, stack free %u", (unsigned)s_cycles, (unsigned)uxTaskGetStackHighWaterMark(NULL)); klog_flush_now(); }   // first cycles are traced to flash at once: they show how far a crash got
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wtms_start(void) { s_mu = xSemaphoreCreateMutex(); xTaskCreatePinnedToCore(app_task, "wtms", 12288, NULL, 5, NULL, 1); }

int wtms_token_ok(const char *t) { int r; wtms_lock(); r = tok_check(t); wtms_unlock(); return r; }
