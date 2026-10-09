// nano FILE: a small nano-style text editor (the whole file is held in RAM: up to ~39 KB).
//   ^O write out   ^X exit   ^K cut line   ^U paste   ^W search   ^A/^E line start/end   ^V/^Y page down/up
//   ^C cursor position   ^L redraw   arrows, Home/End, PgUp/PgDn, Delete, Backspace, Tab, Enter
#include "mini.h"

#define CAP 40000
static char txt[CAP];
static int len, cur, want, topline, coloff, modified;
static char cutbuf[1024]; static int cutlen;
static char fname[96], msg[80], findbuf[48];
static int W = 80, H = 24;
static char ob[3072]; static int on;

/* ---------- output ---------- */
static void flush(void) { if (on) { m_write(1, ob, (size_t)on); on = 0; } }
static void oc(char c) { if (on >= (int)sizeof ob) flush(); ob[on++] = c; }
static void os(const char *s) { while (*s) oc(*s++); }
static void at(int row, int col) { char b[16]; m_snprintf(b, sizeof b, "\x1b[%d;%dH", row, col); os(b); }

/* ---------- text helpers ---------- */
static int line_start(int i) { while (i > 0 && txt[i - 1] != '\n') i--; return i; }
static int line_end(int i) { while (i < len && txt[i] != '\n') i++; return i; }
static int is_cont(char c) { return (c & 0xC0) == 0x80; }                 // UTF-8 continuation byte: zero width
static int col_of(int ls, int pos)
{
    int col = 0;
    for (int i = ls; i < pos; i++) { if (txt[i] == '\t') col = (col / 4 + 1) * 4; else if (!is_cont(txt[i])) col++; }
    return col;
}
static int advance_to_col(int ls, int target)                              // offset on the line at display column <= target
{
    int col = 0, i = ls;
    while (i < len && txt[i] != '\n') {
        int w = txt[i] == '\t' ? (col / 4 + 1) * 4 - col : is_cont(txt[i]) ? 0 : 1;
        if (col + w > target) break;
        col += w; i++;
    }
    while (i < len && txt[i] != '\n' && is_cont(txt[i])) i++;
    return i;
}
static int line_no(int pos) { int n = 0; for (int i = 0; i < pos; i++) if (txt[i] == '\n') n++; return n; }
static void set_want(void) { want = col_of(line_start(cur), cur); }
static void setmsg(const char *m) { int i = 0; while (m[i] && i < (int)sizeof msg - 1) { msg[i] = m[i]; i++; } msg[i] = 0; }

static int insert(const char *s, int n)
{
    if (len + n >= CAP) { setmsg("[ buffer full ]"); return 0; }
    for (int i = len - 1; i >= cur; i--) txt[i + n] = txt[i];
    for (int i = 0; i < n; i++) txt[cur + i] = s[i];
    len += n; cur += n; modified = 1;
    return 1;
}
static void erase(int from, int to)                                        // delete [from, to)
{
    int n = to - from;
    if (n <= 0) return;
    for (int i = to; i < len; i++) txt[i - n] = txt[i];
    len -= n; if (cur > from) cur = cur >= to ? cur - n : from;
    modified = 1;
}

/* ---------- keys ---------- */
enum { K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_NONE };
static int read_key(int ms)
{
    int k = sys_getkey(ms);
    if (k != 27) return k;
    int c = sys_getkey(40);
    if (c == -1) return 27;                                               // a lone ESC
    if (c != '[' && c != 'O') return K_NONE;
    int num = 0, f;
    while ((f = sys_getkey(40)) >= 0) {
        if (f >= '0' && f <= '9') { num = num * 10 + (f - '0'); continue; }
        if (f == ';') { num = 0; continue; }
        break;
    }
    switch (f) {
    case 'A': return K_UP;   case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME; case 'F': return K_END;
    case '~': return num == 1 || num == 7 ? K_HOME : num == 4 || num == 8 ? K_END : num == 3 ? K_DEL : num == 5 ? K_PGUP : num == 6 ? K_PGDN : K_NONE;
    }
    return K_NONE;
}

/* ---------- screen ---------- */
static void help_row(int row, const char *const *kv, int n)
{
    at(row, 1);
    int used = 0, cell = W / 6 < 10 ? 10 : W / 6;
    for (int i = 0; i < n && used + cell <= W + 1; i++) {
        os("\x1b[7m"); os(kv[2 * i]); os("\x1b[0m "); os(kv[2 * i + 1]);
        int l = (int)m_strlen(kv[2 * i]) + 1 + (int)m_strlen(kv[2 * i + 1]);
        for (; l < cell; l++) oc(' ');
        used += cell;
    }
    os("\x1b[K");
}

static void draw(void)
{
    int rows = H - 4;
    int ls = line_start(cur), cy = line_no(cur), ccol = col_of(ls, cur);
    if (cy < topline) topline = cy;
    if (cy >= topline + rows) topline = cy - rows + 1;
    if (ccol < coloff) coloff = ccol;
    if (ccol >= coloff + W) coloff = ccol - W + 1;

    os("\x1b[?25l");
    at(1, 1); os("\x1b[7m");
    char t[120]; int tl = m_snprintf(t, sizeof t, "  miniesp nano   %s%s", fname[0] ? fname : "New Buffer", modified ? "  (modified)" : "");
    os(t); for (int i = tl; i < W; i++) oc(' ');
    os("\x1b[0m");

    int off = 0;
    for (int n = 0; n < topline && off < len; n++) off = line_end(off) + 1;
    for (int r = 0; r < rows; r++) {
        at(2 + r, 1);
        if (off <= len) {
            int col = 0, i = off;
            while (i < len && txt[i] != '\n') {
                unsigned char c = (unsigned char)txt[i];
                if (c == '\t') { int nx = (col / 4 + 1) * 4; for (; col < nx; col++) if (col >= coloff && col < coloff + W) oc(' '); }
                else if (is_cont(c)) { if (col > coloff && col <= coloff + W) oc((char)c); }
                else { if (col >= coloff && col < coloff + W) oc(c < 32 || c == 127 ? '?' : (char)c); col++; }
                i++;
            }
            off = i < len ? i + 1 : len + 1;
        }
        os("\x1b[K");
    }
    at(H - 2, 1); os("\x1b[K");
    if (msg[0]) { int l = (int)m_strlen(msg); at(H - 2, (W - l - 2) / 2 + 1 > 0 ? (W - l - 2) / 2 + 1 : 1); os("\x1b[7m "); os(msg); os(" \x1b[0m"); }
    static const char *const h1[] = { "^O", "Write Out", "^W", "Where Is", "^K", "Cut", "^U", "Paste", "^A", "Home", "^V", "Next Pg" };
    static const char *const h2[] = { "^X", "Exit", "^C", "Cur Pos", "^E", "End", "^L", "Redraw", "^Y", "Prev Pg", "", "" };
    help_row(H - 1, h1, 6); help_row(H, h2, 5);
    at(2 + cy - topline, ccol - coloff + 1);
    os("\x1b[?25h");
    flush();
}

/* ---------- prompt on the status line ---------- */
static int prompt(const char *label, char *buf, int cap)                   // 1 = accepted, 0 = cancelled
{
    int n = (int)m_strlen(buf);
    for (;;) {
        at(H - 2, 1); os("\x1b[K\x1b[7m "); os(label); os("\x1b[0m "); os(buf);
        os("\x1b[?25h"); flush();
        int k = read_key(1000);
        if (k == -1) continue;
        if (k == -2 || k == -3 || k == 27) return 0;
        if (k == '\r' || k == '\n') return 1;
        if ((k == 127 || k == 8) && n) buf[--n] = 0;
        else if (k >= 32 && k < 127 && n < cap - 1) { buf[n++] = (char)k; buf[n] = 0; }
    }
}
static int ask_yn(const char *q)                                           // 1 yes, 0 no, -1 cancel
{
    at(H - 2, 1); os("\x1b[K\x1b[7m "); os(q); os(" \x1b[0m"); flush();
    for (;;) {
        int k = read_key(1000);
        if (k == 'y' || k == 'Y') return 1;
        if (k == 'n' || k == 'N') return 0;
        if (k == -3 || k == -2 || k == 27 || k == 7) return -1;
    }
}

/* ---------- file I/O ---------- */
static int load(const char *path)
{
    int st = sys_stat(path);
    if (st == 2) { m_eprintf("nano: %s is a directory\n", path); return -1; }
    if (st < 0) return 0;                                                  // new file
    int fd = sys_open(path, 0);
    if (fd < 0) { m_eprintf("nano: cannot open %s\n", path); return -1; }
    int r;
    while ((r = sys_read(fd, txt + len, CAP - 1 - len)) > 0) len += r;
    int extra = 0; char one;
    if (len >= CAP - 1 && sys_read(fd, &one, 1) > 0) extra = 1;
    sys_close(fd);
    if (extra) { m_eprintf("nano: %s is larger than %d bytes\n", path, CAP - 1); return -1; }
    return 0;
}
static int save(void)
{
    if (!fname[0]) { setmsg("[ no file name ]"); return 0; }
    int fd = sys_open(fname, 1);
    if (fd < 0) { setmsg("[ cannot write file ]"); return 0; }
    int off = 0;
    while (off < len) { int w = sys_write(fd, txt + off, len - off > 512 ? 512 : len - off); if (w <= 0) break; off += w; }
    sys_close(fd);
    if (off < len) { setmsg("[ write failed (disk full?) ]"); return 0; }
    modified = 0;
    char m[60]; m_snprintf(m, sizeof m, "[ Wrote %d bytes ]", len); setmsg(m);
    return 1;
}
static int write_out(void)
{
    char name[96]; int i = 0; while (fname[i] && i < 95) { name[i] = fname[i]; i++; } name[i] = 0;
    if (!prompt("File Name to Write:", name, sizeof name) || !name[0]) { setmsg("[ cancelled ]"); return 0; }
    int j = 0; while (name[j]) { fname[j] = name[j]; j++; } fname[j] = 0;
    return save();
}

/* ---------- editing commands ---------- */
static void move_up(void) { int ls = line_start(cur); if (ls) cur = advance_to_col(line_start(ls - 1), want); }
static void move_down(void) { int le = line_end(cur); if (le < len) cur = advance_to_col(le + 1, want); }
static void cut_line(void)
{
    int ls = line_start(cur), le = line_end(cur);
    int e = le < len ? le + 1 : le;
    if (e == ls) return;
    cutlen = e - ls > (int)sizeof cutbuf ? (int)sizeof cutbuf : e - ls;
    for (int i = 0; i < cutlen; i++) cutbuf[i] = txt[ls + i];
    erase(ls, e); cur = ls < len ? ls : line_start(len);
    if (cur > len) cur = len;
    set_want();
}
static void search(void)
{
    if (!prompt("Search:", findbuf, sizeof findbuf) || !findbuf[0]) { setmsg("[ cancelled ]"); return; }
    int fl = (int)m_strlen(findbuf);
    for (int pass = 0; pass < 2; pass++) {
        int from = pass == 0 ? cur + 1 : 0, to = pass == 0 ? len : cur;
        for (int i = from; i + fl <= len && i <= to; i++) {
            int j = 0; while (j < fl && txt[i + j] == findbuf[j]) j++;
            if (j == fl) { cur = i; set_want(); if (pass) setmsg("[ Search Wrapped ]"); return; }
        }
    }
    setmsg("[ not found ]");
}

int main(int argc, char **argv)
{
    if (argc >= 2) {
        int k = 0; while (argv[1][k] && k < 95) { fname[k] = argv[1][k]; k++; } fname[k] = 0;
        if (load(fname)) return 1;
    }
    if (!sys_isatty(0)) { m_eputs("nano: needs a terminal (use ssh -t)\n"); return 1; }
    m_puts("\x1b[?1049h\x1b[2J");
    int running = 1;
    while (running) {
        int w = sys_sysinfo(SI_COLS), h = sys_sysinfo(SI_ROWS);
        if (w > 0) W = w > 200 ? 200 : w;
        if (h > 0) H = h;
        if (H < 8) H = 8;
        draw();
        int k = read_key(500);
        if (k == -1) continue;
        msg[0] = 0;
        if (k == -2) break;                                                // disconnected: leave without saving
        int rows = H - 4;
        switch (k) {
        case K_UP: move_up(); break;
        case K_DOWN: move_down(); break;
        case K_LEFT: if (cur > 0) { cur--; while (cur > 0 && is_cont(txt[cur])) cur--; } set_want(); break;
        case K_RIGHT: if (cur < len) { cur++; while (cur < len && is_cont(txt[cur])) cur++; } set_want(); break;
        case K_HOME: case 1: cur = line_start(cur); set_want(); break;
        case K_END: case 5: cur = line_end(cur); set_want(); break;
        case K_PGUP: case 25: for (int i = 0; i < rows - 1; i++) move_up(); break;
        case K_PGDN: case 22: for (int i = 0; i < rows - 1; i++) move_down(); break;
        case K_DEL: case 4: if (cur < len) { int e = cur + 1; while (e < len && is_cont(txt[e])) e++; erase(cur, e); } break;
        case 127: case 8: if (cur > 0) { int s = cur - 1; while (s > 0 && is_cont(txt[s])) s--; erase(s, cur); cur = s; set_want(); } break;
        case '\r': case '\n': { char c = '\n'; insert(&c, 1); set_want(); break; }
        case '\t': { char c = '\t'; insert(&c, 1); set_want(); break; }
        case 11: cut_line(); break;
        case 21: if (cutlen) { insert(cutbuf, cutlen); set_want(); } break;
        case 23: search(); break;
        case 15: write_out(); break;
        case 12: m_puts("\x1b[2J"); break;
        case -3: { char m[48]; int ls = line_start(cur); m_snprintf(m, sizeof m, "[ line %d, col %d, byte %d/%d ]", line_no(cur) + 1, col_of(ls, cur) + 1, cur, len); setmsg(m); break; }
        case 24:                                                           // ^X
            if (!modified) { running = 0; break; }
            { int a = ask_yn("Save modified buffer? (Y/N, ^C cancel)");
              if (a == 0) running = 0;
              else if (a == 1 && (fname[0] ? save() : write_out())) running = 0; }
            break;
        default:
            if (k >= 32 && k < 256 && k != 127) { char c = (char)k; insert(&c, 1); set_want(); }
        }
    }
    m_puts("\x1b[?25h\x1b[?1049l");
    return 0;
}
