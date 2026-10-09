// wtms - water tank management system (ultrasonic level sensor + MQTT pump relay + Home Assistant + web dashboard).
// Installed with:  pkg install wtms   then   wtms setup   (interactive)   or   wtms setup --headless key=value ...
//
//   wtms setup [--headless] [--start] [key=value ...]   configure + create the service (see the setup section below)
//   wtms run                 one control cycle (the `wtms` service runs this every 5 s)
//   wtms status              configuration, last state and a live reading
//   wtms read                live distance / level (use it while calibrating)
//   wtms config              show the configuration;  wtms config key=value ...  changes it (keys below)
//   wtms cmd reset|pump_on|pump_off     same as publishing to <name>/cmd  (picked up by the next cycle)
//   wtms discover            re-publish the Home Assistant discovery messages
//
// Configuration (persistent, NVS key "tank.cfg"):
//   name=tank1               MQTT topic prefix            pump=relay_node_1/relay/waterpump   relay topic prefix (/set, /state)
//   trig=33 echo=32          HC-SR04 pins (echo needs a 5V->3.3V divider)     temp=0   DS18B20 GPIO for temperature compensation (0 = none)
//   empty_mm=1000            sensor face to the water surface when the tank is EMPTY
//   full_mm=150              sensor face to the water surface when the tank is FULL (>= 25 mm: the sensor's dead zone)
//   vol_l=1000               litres at full (assumes straight walls: litres scale linearly with level)
//   on_pct=25 off_pct=95     start the pump at/below on_pct, stop at/above off_pct
//   max_run=30               stop (and latch a fault) after this many minutes of pumping
//   rest=5                   minimum minutes between runs     flow_min=3 rise_pm=10   fault "no_flow" if the level rose < rise_pm/10 % in flow_min minutes
//   auto=1                   automatic pump control           hist=300  seconds between history samples      ha=homeassistant  discovery prefix ("" = off)
//
// Safety: the pump is re-commanded ON every cycle while it should run, so a relay-node auto-off timer of ~60 s turns it off by itself if this
// board dies. A sensor failure, a full tank, the run-time limit or a missing level rise all stop the pump.
#include "mini.h"

typedef struct { const char *k; void *p; int n; } ik;       // n == 0: int, else a string of n bytes

static char c_name[24] = "tank1", c_pump[64] = "relay_node_1/relay/waterpump", c_ha[24] = "homeassistant";
static int c_trig = 33, c_echo = 32, c_temp = 0, c_empty = 1000, c_full = 150, c_vol = 1000, c_on = 25, c_off = 95;
static int c_maxrun = 30, c_rest = 5, c_flowmin = 3, c_rise = 10, c_auto = 1, c_hist = 300;
static int c_interval = 15, c_samples = 7, c_jump = 15, c_goodn = 3, c_histkb = 16, c_tempc = 20;
static const ik CFG[] = {
    { "name", c_name, sizeof c_name }, { "pump", c_pump, sizeof c_pump }, { "ha", c_ha, sizeof c_ha },
    { "trig", &c_trig, 0 }, { "echo", &c_echo, 0 }, { "temp", &c_temp, 0 }, { "empty_mm", &c_empty, 0 }, { "full_mm", &c_full, 0 },
    { "vol_l", &c_vol, 0 }, { "on_pct", &c_on, 0 }, { "off_pct", &c_off, 0 }, { "max_run", &c_maxrun, 0 }, { "rest", &c_rest, 0 },
    { "flow_min", &c_flowmin, 0 }, { "rise_pm", &c_rise, 0 }, { "auto", &c_auto, 0 }, { "hist", &c_hist, 0 },
    { "interval", &c_interval, 0 }, { "samples", &c_samples, 0 }, { "jump_pct", &c_jump, 0 }, { "good_n", &c_goodn, 0 }, { "hist_kb", &c_histkb, 0 }, { "temp_c", &c_tempc, 0 },
};
static int s_force, s_funtil, s_good, s_ema = -1, s_since, s_start, s_lastoff, s_fault, s_lastrun, s_lasthist, s_disc, s_jumps, s_hold;
static const ik STT[] = {
    { "ema", &s_ema, 0 }, { "force", &s_force, 0 }, { "funtil", &s_funtil, 0 }, { "good", &s_good, 0 }, { "since", &s_since, 0 }, { "start", &s_start, 0 }, { "lastoff", &s_lastoff, 0 }, { "fault", &s_fault, 0 },
    { "lastrun", &s_lastrun, 0 }, { "lasthist", &s_lasthist, 0 }, { "disc", &s_disc, 0 }, { "jumps", &s_jumps, 0 }, { "hold", &s_hold, 0 },
};
#define N(t) ((int)(sizeof(t) / sizeof((t)[0])))
#define DISC_VER 2
static int g_quiet;
static char g_local_cmd[24];                         // command given on the command line / CGI (handled like an MQTT command)
static const char *FAULTS[] = { "none", "sensor", "max_run", "no_flow" };
enum { F_NONE, F_SENSOR, F_MAXRUN, F_NOFLOW };
static const char *DIR = "/www/tank", *HIST = "/www/tank/history.csv", *HIST_OLD = "/www/tank/history.old.csv", *STATE_JSON = "/www/tank/state.json";

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

// Distance sensor-to-water in mm (median of 7 echoes, speed of sound corrected for temperature), or -1 on failure.
static int measure(void)
{
    g_t100 = c_tempc * 100;                                    // assumed air temperature when there is no DS18B20
    if (c_temp > 0) { int t = sys_ds18b20(c_temp); if (t > -9000) { g_t100 = t; g_have_t = 1; } }
    int v[15], n = 0;
    for (int i = 0; i < c_samples; i++) {
        int us = sys_sonar_pulse(c_trig, c_echo, 25000);
        if (us >= 150 && us <= 24000) v[n++] = us;
        sys_sleep_ms(60);
    }
    if (n < (c_samples + 1) / 2) return -1;                    // at least half of the echoes must be valid
    sort(v, n);
    int d = (int)((long long)v[n / 2] * (3313 + g_t100 * 606 / 10000) / 20000);
    if (d < 25 || d > c_empty + 150) return -1;           // closer than the sensor's dead zone or farther than the tank floor: not a real water echo
    return d;
}

static int level_pm(int d_mm)
{
    int span = c_empty - c_full; if (span < 50) span = 50;
    int pm = (c_empty - d_mm) * 1000 / span;
    return pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
}

static char tb[128], pb[760];
static void topic(const char *fmt) { m_snprintf(tb, sizeof tb, fmt, c_name); }
static int pub(const char *t, const char *p, int retain) { return sys_mqtt_pub(t, p, (int)m_strlen(p), retain, 1); }

static int mk_state(char *b, int n, int pm, int dist, const char *pump, int auto_)
{
    int t10 = g_t100 / 10;
    return m_snprintf(b, n, "{\"ts\":%u,\"level\":%d.%d,\"litres\":%d,\"distance_cm\":%d.%d,\"pump\":\"%s\",\"auto\":\"%s\",\"fault\":\"%s\",\"temp\":%d.%d,"
                      "\"on_pct\":%d,\"off_pct\":%d,\"vol\":%d,\"force\":\"%s\",\"force_left\":%d}",
                      sys_time(), pm / 10, pm % 10, c_vol * pm / 1000, dist / 10, iabs(dist) % 10, pump, auto_ ? "ON" : "OFF", FAULTS[s_fault],
                      t10 / 10, iabs(t10) % 10, c_on, c_off, c_vol, s_force == 1 ? "on" : s_force == 2 ? "off" : "none",
                      s_force && s_funtil && (unsigned)s_funtil > tnow() ? (int)((unsigned)s_funtil - tnow()) : 0);
}

static void write_file(const char *path, const char *s) { int fd = sys_open(path, 1); if (fd >= 0) { sys_write(fd, s, (int)m_strlen(s)); sys_close(fd); } }

static void history(int pm, int pumpon)
{
    if (!sys_time()) return;
    unsigned now = sys_time();
    if (s_lasthist && now - (unsigned)s_lasthist < (unsigned)c_hist) return;
    s_lasthist = (int)now;
    if (sys_fsize(HIST) > c_histkb * 1000) sys_rename(HIST, HIST_OLD);
    int fd = sys_open(HIST, 2);
    if (fd < 0) return;
    char l[48]; int n = m_snprintf(l, sizeof l, "%u,%d,%d,%d\n", now, pm, c_vol * pm / 1000, pumpon);
    sys_write(fd, l, n);
    sys_close(fd);
}

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
}

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
    if (sensor_ok) {
        int raw = level_pm(d);
        if (s_ema < 0) s_ema = raw;
        else if (iabs(raw - s_ema) > c_jump * 10 && s_jumps < 3) s_jumps++;           // ignore a one-off jump (splash, reflection)
        else { s_ema = (s_ema + raw) / 2; s_jumps = 0; }
        pm = s_ema;
        if (s_fault == F_SENSOR) s_fault = F_NONE;
    } else if (s_fault == F_NONE) s_fault = F_SENSOR;

    int online = 1;
    if (sys_mqtt_get(t_stat, buf, sizeof buf - 1) > 0) { buf[sys_mqtt_get(t_stat, buf, sizeof buf - 1)] = 0; online = eq(buf, "online"); }
    int ps = sys_mqtt_get(t_state, buf, sizeof buf - 1), pump_known = ps > 0, pump_on = 0;
    if (pump_known) { buf[ps] = 0; pump_on = eq(buf, "ON"); }
    int mq = sys_mqtt_state() == 2;

    // ---- commands ----
    if (mq && new_cmd(t_auto, buf, sizeof buf, now)) { c_auto = eq(buf, "ON") || eq(buf, "1"); save("tank.cfg", CFG, N(CFG)); }
    int want_on = 0, want_off = 0;
    int got = 0;
    if (g_local_cmd[0]) { for (int k = 0; k < (int)sizeof buf - 1 && g_local_cmd[k]; k++) buf[k] = g_local_cmd[k], buf[k + 1] = 0; got = 1; }
    else if (mq && new_cmd(t_cmd, buf, sizeof buf, now)) got = 1;
    if (got) {
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
    const char *act = "none";
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
            if (!sensor_ok) stop = 1;
            else if (pm >= c_off * 10) stop = 1;
            else if (want_off) { stop = 1; s_hold = (int)now + c_rest * 60; }
            else if (run > (unsigned)c_maxrun * 60) { stop = 1; s_fault = F_MAXRUN; }
            else if (run >= (unsigned)c_flowmin * 60 && pm - s_start < c_rise) { stop = 1; s_fault = F_NOFLOW; }
            if (stop) { pub(t_set, "OFF", 0); s_since = 0; s_lastoff = (int)now; act = "stop"; }
            else { pub(t_set, "ON", 0); act = "run"; }                          // keep-alive for the relay's auto-off timer
        } else {
            s_since = 0;
            int can = sensor_ok && s_good >= c_goodn && s_fault == F_NONE && online && pm < c_off * 10;     // 3 good readings in a row before the first start
            int rested = now - (unsigned)s_lastoff >= (unsigned)c_rest * 60 && (unsigned)s_hold <= now;
            if (can && ((c_auto && pm <= c_on * 10 && rested) || want_on)) {
                pub(t_set, "ON", 0); s_since = (int)now; s_start = pm; act = "start"; pump_on = 1;
            }
        }
    }
    s_lastrun = (int)now;

    // ---- publish ----
    if (!sensor_ok) { d = c_empty; pm = s_ema >= 0 ? s_ema : 0; }
    static char js[460];
    mk_state(js, sizeof js, pm, d, pump_on ? "ON" : "OFF", c_auto);
    write_file(STATE_JSON, js);
    if (mq) {
        if (c_ha[0] && s_disc != DISC_VER) discovery();
        topic("%s/state"); pub(tb, js, 1);
        topic("%s/auto/state"); pub(tb, c_auto ? "ON" : "OFF", 1);
    }
    history(pm, pump_on);
    save("tank.st", STT, N(STT));
    if (!g_quiet) m_printf("level %d.%d%% (%d L) dist %d.%d cm pump %s auto %s fault %s act %s mqtt %s\n", pm / 10, pm % 10, c_vol * pm / 1000, d / 10, d % 10,
             pump_on ? "ON" : "OFF", c_auto ? "ON" : "OFF", FAULTS[s_fault], act, mq ? (pump_known ? "ok" : "no-pump-state") : "down");
    return 0;
}

static void show_cfg(void)
{
    for (int i = 0; i < N(CFG); i++) {
        if (CFG[i].n) m_printf("%s=%s\n", CFG[i].k, (const char *)CFG[i].p);
        else m_printf("%s=%d\n", CFG[i].k, *(int *)CFG[i].p);
    }
}

// Validate the in-memory configuration and store it; returns an error text or NULL.
static const char *cfg_validate_save(void)
{
    if (c_full < 25) return "full_mm must be at least 25 (sensor dead zone)";
    if (c_empty <= c_full + 50) return "empty_mm must be larger than full_mm by more than 50";
    if (c_on >= c_off || c_off > 100 || c_on < 0) return "need 0 <= on_pct < off_pct <= 100";
    if (c_vol < 1) return "vol_l must be at least 1";
    if (c_maxrun < 1 || c_rest < 0 || c_flowmin < 1 || c_hist < 10) return "max_run/flow_min >= 1, rest >= 0, hist >= 10";
    if (c_interval < 5 || c_interval > 3600) return "interval must be 5..3600 seconds";
    if (c_samples < 3 || c_samples > 15) return "samples must be 3..15";
    if (c_jump < 5 || c_jump > 100) return "jump_pct must be 5..100";
    if (c_goodn < 1 || c_goodn > 20) return "good_n must be 1..20";
    if (c_histkb < 4 || c_histkb > 64) return "hist_kb must be 4..64";
    if (c_tempc < -30 || c_tempc > 60) return "temp_c must be -30..60";
    if (!c_name[0] || !c_pump[0]) return "name and pump must not be empty";
    save("tank.cfg", CFG, N(CFG));
    s_disc = 0; s_ema = -1; s_jumps = 0; s_good = 0;
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
    char js[460]; int fd = sys_open(STATE_JSON, 0), k = 0;
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
        if (d < 0) { jout("{\"ok\":false}"); return 0; }
        int pm = level_pm(d); char o[120];
        m_snprintf(o, sizeof o, "{\"ok\":true,\"distance_mm\":%d,\"level\":%d.%d,\"litres\":%d}", d, pm / 10, pm % 10, c_vol * pm / 1000);
        jout(o); return 0;
    }
    if (eq(sub, "auto") && argc > 4) {
        c_auto = eq(argv[4], "on");
        save("tank.cfg", CFG, N(CFG));
        return do_act("noop");
    }
    if (eq(sub, "act") && argc > 4) return do_act(argv[4]);
    jout("{\"ok\":false,\"error\":\"unknown\"}");
    return 0;
}

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
    if (su_shape == 'r') c_vol = su_a * su_b / 10 * su_ht / 100;
    else if (su_shape == 'c') c_vol = (int)((long long)785 * su_a * su_a / 1000 * su_ht / 1000);
    else if (su_shape == 'l') c_vol = su_lit;
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

int main(int argc, char **argv)
{
    load("tank.cfg", CFG, N(CFG));
    load("tank.st", STT, N(STT));
    const char *c = argc > 1 ? argv[1] : "";                  // no default action: `wtms` alone must never drive the pump
    if (eq(c, "run")) return run_cycle();
#ifndef NO_SETUP
    if (eq(c, "setup")) return do_setup(argc, argv);
#endif
    if (eq(c, "config")) {
        if (argc == 2) { show_cfg(); return 0; }
        const char *err = set_cfg(argc, argv, 2);
        if (err) { m_eprintf("wtms: %s\n", err); return 2; }
        m_puts("saved\n");
        return 0;
    }
    if (eq(c, "web")) return web(argc, argv);
    if (eq(c, "passwd") && argc > 2) {                                       // CLI (ssh): set/replace the web password
        if (m_strlen(argv[2]) < 6) { m_eputs("wtms: password must have at least 6 characters\n"); return 2; }
        sys_kv_set("tank.pw", argv[2], (int)m_strlen(argv[2])); sys_kv_del("tank.tok");
        m_puts("web password set; all web sessions logged out\n");
        return 0;
    }
    if (eq(c, "read")) {
        int d = measure();
        if (d < 0) { m_puts("no echo (check wiring: trig/echo pins, 5V, echo divider)\n"); return 1; }
        int pm = level_pm(d);
        m_printf("distance %d.%d cm  -> level %d.%d%% (%d L)\n", d / 10, d % 10, pm / 10, pm % 10, c_vol * pm / 1000);
        return 0;
    }
    if (eq(c, "status")) {
        show_cfg();
        m_printf("-- state: fault=%s pump_since=%d ema=%d\n", FAULTS[s_fault], s_since, s_ema);
        char js[420]; int n = sys_fsize(STATE_JSON);
        int fd = sys_open(STATE_JSON, 0);
        if (fd >= 0 && n > 0) { n = sys_read(fd, js, sizeof js - 1); if (n > 0) { js[n] = 0; m_printf("last: %s\n", js); } sys_close(fd); }
        return 0;
    }
    if (eq(c, "cmd") && argc > 2) {                                           // handled immediately (and by this cycle's publish)
        int n = 0; while (argv[2][n] && n < (int)sizeof g_local_cmd - 1) { g_local_cmd[n] = argv[2][n]; n++; }
        g_local_cmd[n] = 0;
        return run_cycle();
    }
    if (eq(c, "discover")) { s_disc = 0; save("tank.st", STT, N(STT)); return run_cycle(); }
    m_eputs("usage: wtms setup [--headless] | run | status | read | config [key=value ...] | cmd reset|force_on[:min]|force_off[:min]|release|pump_on|pump_off | passwd NEW | discover\n");
    return 2;
}
