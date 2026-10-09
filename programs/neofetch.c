// neofetch: system summary with the miniesp chip logo (colors only on a terminal; the logo is skipped when narrow).
#include "mini.h"

static const char *T = "", *O = "", *W = "", *B = "", *R = "";     // teal, orange, white, bold teal, reset

static void kb(char *out, int n, int bytes) { m_snprintf(out, n, "%d KB", bytes / 1024); }

int main(int argc, char **argv)
{
    if (sys_isatty(1)) { T = "\x1b[38;5;30m"; O = "\x1b[38;5;208m"; W = "\x1b[1;37m"; B = "\x1b[1;38;5;30m"; R = "\x1b[0m"; }
    int cols = sys_sysinfo(SI_COLS), logo = !(sys_isatty(1) && cols > 0 && cols < 58);

    char host[32] = "", ip[20] = "", ssid[34] = "", ver[48] = "", a[24], b[24], c[24];
    sys_netinfo(0, host, sizeof host); sys_netinfo(1, ip, sizeof ip); sys_netinfo(2, ssid, sizeof ssid);
    sys_version(ver, sizeof ver);
    int up = sys_sysinfo(SI_UPTIME_S), d = up / 86400, h = up % 86400 / 3600, m = up % 3600 / 60;
    int dt = sys_sysinfo(SI_DRAM_TOTAL), df = sys_sysinfo(SI_DRAM_FREE);

    char info[12][96]; int n = 0;
    m_snprintf(info[n++], 96, "%sesp%s@%s%s%s", B, R, B, host, R);
    m_snprintf(info[n++], 96, "%s-----------------%s", T, R);
    m_snprintf(info[n++], 96, "%sOS%s: esp32-unix (ESP-IDF + WAMR AOT)", B, R);
    m_snprintf(info[n++], 96, "%sFirmware%s: %s (syscall ABI %d)", B, R, ver, sys_abi());
    m_snprintf(info[n++], 96, "%sUptime%s: %dd %dh %dm", B, R, d, h, m);
    m_snprintf(info[n++], 96, "%sCPU%s: ESP32 Xtensa LX6 @ %d MHz (%d tasks)", B, R, sys_sysinfo(SI_CPU_MHZ), sys_sysinfo(SI_NTASKS));
    kb(a, sizeof a, dt - df); kb(b, sizeof b, dt); kb(c, sizeof c, sys_sysinfo(SI_DRAM_LARGEST));
    m_snprintf(info[n++], 96, "%sMemory%s: %s / %s (largest block %s)", B, R, a, b, c);
    kb(a, sizeof a, sys_sysinfo(SI_IRAM8_TOTAL) - sys_sysinfo(SI_IRAM8_FREE)); kb(b, sizeof b, sys_sysinfo(SI_IRAM8_TOTAL));
    m_snprintf(info[n++], 96, "%sIRAM pool%s: %s / %s", B, R, a, b);
    kb(a, sizeof a, sys_sysinfo(SI_FS_USED)); kb(b, sizeof b, sys_sysinfo(SI_FS_TOTAL));
    m_snprintf(info[n++], 96, "%sStorage%s: %s / %s (LittleFS)", B, R, a, b);
    m_snprintf(info[n++], 96, "%sNetwork%s: %s%s%s%s", B, R, ip, ssid[0] ? " on " : "", ssid, ssid[0] ? "" : "");
    int rssi = sys_sysinfo(SI_RSSI);
    if (rssi) m_snprintf(info[n++], 96, "%sSignal%s: %d dBm", B, R, rssi);
    int ts = sys_time_state();
    m_snprintf(info[n++], 96, "%sClock%s: %s", B, R, ts == 2 ? "NTP synced" : ts == 1 ? "approximate (not synced)" : "not set");

    // 21-column chip logo, one string per line
    char art[7][160]; const int nart = 7;
    m_snprintf(art[0], 160, "%s    ##   ##   ##     %s", T, R);
    m_snprintf(art[1], 160, "%s  .---------------.  %s", T, R);
    m_snprintf(art[2], 160, "%s==|               |==%s", T, R);
    m_snprintf(art[3], 160, "%s==|%s >%s %s_____%s       %s|==%s", T, W, R, O, R, T, R);
    m_snprintf(art[4], 160, "%s==|               |==%s", T, R);
    m_snprintf(art[5], 160, "%s  '---------------'  %s", T, R);
    m_snprintf(art[6], 160, "%s    ##   ##   ##     %s", T, R);
    int rows = n > nart ? n : nart, top = logo ? (rows - nart) / 2 : 0;
    for (int i = 0; i < rows; i++) {
        if (logo) {
            int ai = i - top;
            if (ai >= 0 && ai < nart) m_puts(art[ai]); else m_puts("                     ");
            m_puts("  ");
        }
        if (i < n) m_puts(info[i]);
        m_puts("\n");
    }
    if (sys_isatty(1)) {                         // color swatch
        if (logo) m_puts("                       ");
        m_printf("\x1b[48;5;30m   \x1b[48;5;208m   \x1b[48;5;236m   \x1b[48;5;81m   \x1b[0m\n");
    }
    return 0;
}
