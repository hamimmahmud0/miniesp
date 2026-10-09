// btop - a full-screen system monitor for esp32-unix (CPU, memory, tasks), in the spirit of btop/htop.
//
//   q / Ctrl-C  quit        space  pause/resume        s  change sort (cpu, stack, name, pid)
//   + / -       refresh slower/faster                  r  clear the CPU history
//
// Needs a terminal that understands ANSI/UTF-8 (any modern SSH client). Adapts to the window size.
#include "mini.h"

#define MAXW 120
#define MAXH 40
#define MAXT 24

static char ob[20000];                       // one frame is built here and written in a single call
static int on;

static void put(const char *s) { while (*s && on < (int)sizeof ob - 1) ob[on++] = *s++; }
static void putc_(char c) { if (on < (int)sizeof ob - 1) ob[on++] = c; }

// tiny printf into the frame buffer: %s %d %u %c %%, with '-' (left align), '0' (zero pad) and a width
static void bp(const char *f, ...)
{
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { putc_(*f); continue; }
        f++;
        int left = 0, zero = 0, width = 0;
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        char tmp[16]; const char *s = tmp; int len = 0;
        if (*f == 's') { s = va_arg(ap, const char *); if (!s) s = ""; len = (int)m_strlen(s); }
        else if (*f == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
        else if (*f == 'd' || *f == 'u') {
            unsigned v; int neg = 0;
            if (*f == 'd') { int i = va_arg(ap, int); if (i < 0) { neg = 1; v = 0u - (unsigned)i; } else v = (unsigned)i; }
            else v = va_arg(ap, unsigned);
            char r[12]; int n = 0;
            do { r[n++] = '0' + v % 10; v /= 10; } while (v);
            if (neg) tmp[len++] = '-';
            while (n) tmp[len++] = r[--n];
        } else if (*f == '%') { tmp[0] = '%'; len = 1; }
        else if (!*f) break;
        int padn = width > len ? width - len : 0;
        if (!left) while (padn--) putc_(zero ? '0' : ' ');
        for (int i = 0; i < len; i++) putc_(s[i]);
        if (left) while (padn--) putc_(' ');
    }
    va_end(ap);
}

static int W = 80, H = 24;

static void at(int row, int col) { bp("\x1b[%d;%dH", row, col); }
static void color(int c) { bp("\x1b[38;5;%dm", c); }
static void reset(void) { put("\x1b[0m"); }
static int heat(int pct) { return pct < 50 ? 46 : pct < 75 ? 220 : pct < 90 ? 208 : 196; }   // green / yellow / orange / red

static void rep(const char *s, int n) { while (n-- > 0) put(s); }

// box border row: "┌─ title ───┐" / "└────┘"
static void box_top(int row, const char *title)
{
    at(row, 1); color(39);
    put("\xe2\x94\x8c\xe2\x94\x80 ");
    color(255); put(title); color(39);
    put(" ");
    rep("\xe2\x94\x80", W - 5 - (int)m_strlen(title));
    put("\xe2\x94\x90"); reset();
}
static void box_bottom(int row)
{
    at(row, 1); color(39); put("\xe2\x94\x94"); rep("\xe2\x94\x80", W - 2); put("\xe2\x94\x98"); reset();
}
static void row_begin(int row) { at(row, 1); color(39); put("\xe2\x94\x82"); reset(); put(" "); }
static void row_end(int row) { put("\x1b[K"); at(row, W); color(39); put("\xe2\x94\x82"); reset(); }

// a horizontal bar of `w` cells showing pct (0..100)
static void bar(int pct, int w)
{
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int filled = pct * w / 100;
    color(heat(pct)); rep("\xe2\x96\x88", filled);                 // █
    color(238);       rep("\xe2\x96\x91", w - filled); reset();    // ░
}

static int hist[MAXW];
static int nhist;

static void push_hist(int v)
{
    if (nhist < MAXW) hist[nhist++] = v;
    else { for (int i = 1; i < MAXW; i++) hist[i - 1] = hist[i]; hist[MAXW - 1] = v; }
}

static const char *blocks[9] = { " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };

static void kb(int bytes) { bp("%u", (unsigned)bytes / 1024); }

static void mem_row(int row, const char *label, int free_b, int total_b)
{
    int used = total_b - free_b;
    int pct = total_b > 0 ? (int)((long long)used * 100 / total_b) : 0;
    int bw = W - 43; if (bw < 8) bw = 8;
    row_begin(row);
    color(252); bp("%-10s", label); reset();
    put("["); bar(pct, bw); put("] ");
    color(heat(pct)); bp("%3d%%", pct); reset();
    put("  "); kb(used); put(" / "); kb(total_b); put(" KB");
    row_end(row);
}

static int sort_mode;                          // 0 cpu, 1 stack (lowest free first), 2 name, 3 pid
static const char *sort_names[4] = { "cpu", "stack", "name", "pid" };

static int less(const esp_task_t *a, const esp_task_t *b)    // should a come before b?
{
    switch (sort_mode) {
    case 0: return a->cpu10 > b->cpu10 || (a->cpu10 == b->cpu10 && a->pid < b->pid);
    case 1: return a->stack < b->stack;
    case 2: { const char *x = a->name, *y = b->name; while (*x && *x == *y) { x++; y++; } return *x < *y; }
    default: return a->pid < b->pid;
    }
}

static esp_task_t tasks[MAXT];
static int ntasks;
static int cpu_pct10;                          // total CPU usage x10
static int interval = 1000;
static int paused;
static int first = 1;

static void sample(void)
{
    ntasks = sys_tasks(tasks, (int)sizeof tasks);
    if (ntasks < 0) ntasks = 0;
    int idle = -1, sum = 0;
    for (int i = 0; i < ntasks; i++) {
        const char *n = tasks[i].name;
        if (n[0] == 'I' && n[1] == 'D' && n[2] == 'L' && n[3] == 'E') idle = (idle < 0 ? 0 : idle) + tasks[i].cpu10;
        else sum += tasks[i].cpu10;
    }
    cpu_pct10 = idle >= 0 ? 1000 - idle : sum;
    if (cpu_pct10 < 0) cpu_pct10 = 0; if (cpu_pct10 > 1000) cpu_pct10 = 1000;
    push_hist(cpu_pct10 / 10);
    for (int i = 1; i < ntasks; i++) {         // insertion sort
        esp_task_t t = tasks[i]; int j = i - 1;
        while (j >= 0 && less(&t, &tasks[j])) { tasks[j + 1] = tasks[j]; j--; }
        tasks[j + 1] = t;
    }
}

static void draw(void)
{
    on = 0;
    char host[32], ip[20], ssid[34];
    sys_netinfo(0, host, sizeof host); sys_netinfo(1, ip, sizeof ip); sys_netinfo(2, ssid, sizeof ssid);
    int up = sys_sysinfo(SI_UPTIME_S);
    int rssi = sys_sysinfo(SI_RSSI);

    // ---- header (reverse video) ----
    at(1, 1); put("\x1b[7m");
    int h0 = on;
    bp(" btop-esp  %s  up %ud %02u:%02u:%02u  %s", host, (unsigned)(up / 86400), (unsigned)(up / 3600 % 24), (unsigned)(up / 60 % 60), (unsigned)(up % 60), ip);
    if (ssid[0]) { bp("  %s", ssid); if (rssi) bp(" %ddBm", rssi); }
    bp("  %uMHz", (unsigned)sys_sysinfo(SI_CPU_MHZ));
    if (paused) bp("  [PAUSED]");
    int used = on - h0;
    rep(" ", W - used > 0 ? W - used : 0);
    reset();

    // ---- CPU ----
    box_top(2, "cpu");
    row_begin(3);
    int pct = cpu_pct10 / 10;
    color(252); bp("total     "); reset();
    int bw = W - 43; if (bw < 8) bw = 8;
    put("["); bar(pct, bw); put("] ");
    color(heat(pct)); bp("%3d.%u%%", pct, (unsigned)(cpu_pct10 % 10)); reset();
    bp("  tasks %d", sys_sysinfo(SI_NTASKS));
    row_end(3);
    row_begin(4);
    int gw = W - 4;
    int start = nhist > gw ? nhist - gw : 0;
    rep(" ", gw - (nhist - start));            // right-align: newest sample at the right edge
    for (int i = start; i < nhist; i++) { color(heat(hist[i])); put(blocks[hist[i] * 8 / 100 > 8 ? 8 : hist[i] * 8 / 100]); }
    reset();
    row_end(4);
    box_bottom(5);

    // ---- memory ----
    box_top(6, "memory");
    mem_row(7, "DRAM", sys_sysinfo(SI_DRAM_FREE), sys_sysinfo(SI_DRAM_TOTAL));
    mem_row(8, "IRAM pool", sys_sysinfo(SI_IRAM8_FREE), sys_sysinfo(SI_IRAM8_TOTAL));
    mem_row(9, "AOT code", sys_sysinfo(SI_EXEC_FREE), sys_sysinfo(SI_EXEC_TOTAL));
    int fsu = sys_sysinfo(SI_FS_USED), fst = sys_sysinfo(SI_FS_TOTAL);
    mem_row(10, "Flash FS", fst - fsu, fst);
    box_bottom(11);

    // ---- tasks ----
    char title[40]; int tl = 0;
    const char *pre = "tasks - sort: ";
    for (const char *p = pre; *p; p++) title[tl++] = *p;
    for (const char *p = sort_names[sort_mode]; *p; p++) title[tl++] = *p;
    title[tl] = 0;
    box_top(12, title);
    row_begin(13);
    put("\x1b[1m"); bp("%-4s %-12s %s %-4s %-6s %-6s", "PID", "NAME", "S", "PRIO", "STACK", "CPU%"); reset();
    row_end(13);
    int rows = H - 15;                         // lines available between the header row and the bottom border
    for (int i = 0; i < rows; i++) {
        int r = 14 + i;
        row_begin(r);
        if (i < ntasks) {
            const esp_task_t *t = &tasks[i];
            int tp = t->cpu10 / 10;
            bp("%-4u %-12s %c %-4u %-6u ", (unsigned)t->pid, t->name, t->state, (unsigned)t->prio, (unsigned)t->stack);
            color(heat(tp)); bp("%2d.%u", tp, (unsigned)(t->cpu10 % 10)); reset();
            int tw = W - 48; if (tw > 20) tw = 20;
            if (tw >= 4) { put(" "); bar(tp, tw); }
        }
        row_end(r);
    }
    box_bottom(H - 1);

    // ---- footer ----
    at(H, 1); put("\x1b[7m");
    const char *help = " q quit   space pause   s sort   +/- refresh   r reset graph ";
    put(help);
    int tail = 9;                                // room for " 5000ms " on the right
    rep(" ", W - (int)m_strlen(help) - tail > 0 ? W - (int)m_strlen(help) - tail : 0);
    bp(" %-5dms", interval);
    reset();
    m_write(1, ob, (size_t)on);
}

int main(int argc, char **argv)
{
    if (!sys_isatty(1)) { m_eputs("btop: needs a terminal (use ssh -t)\n"); return 1; }
    sys_sigint(1);                              // Ctrl-C arrives as a key, so the screen can be restored
    m_puts("\x1b[?1049h\x1b[?25l\x1b[2J");      // alternate screen, hide cursor
    sample();                                   // first sample only primes the counters
    int lastw = 0, lasth = 0;
    for (;;) {
        int w = sys_sysinfo(SI_COLS), h = sys_sysinfo(SI_ROWS);
        W = w < 60 ? 60 : w > MAXW ? MAXW : w;
        H = h < 18 ? 18 : h > MAXH ? MAXH : h;
        if (W != lastw || H != lasth) { m_puts("\x1b[2J"); lastw = W; lasth = H; }
        if (!paused) { if (first) { sys_sleep_ms(250); first = 0; } sample(); }
        draw();
        int k = sys_getkey(interval);
        if (k == 'q' || k == 'Q' || k == 3 || k == -3 || k == -2) break;
        if (k == ' ') paused = !paused;
        else if (k == 's') sort_mode = (sort_mode + 1) % 4;
        else if (k == '+' || k == '=') { if (interval < 5000) interval += 500; }
        else if (k == '-' || k == '_') { if (interval > 500) interval -= 500; }
        else if (k == 'r') nhist = 0;
    }
    m_puts("\x1b[?25h\x1b[?1049l");              // show cursor, leave the alternate screen
    return 0;
}
