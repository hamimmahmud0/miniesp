#include "term.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static term_t *s_current;
void term_set_current(term_t *t) { s_current = t; }
term_t *term_current(void) { return s_current; }

term_t *term_new(bool pty)
{
    term_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->in = xStreamBufferCreate(1024, 1);
    t->out = xStreamBufferCreate(2048, 1);
    t->pty = pty;
    t->cols = 80; t->rows = 24;
    if (!t->in || !t->out) { term_free(t); return NULL; }
    return t;
}

void term_free(term_t *t)
{
    if (!t) return;
    if (t->in) vStreamBufferDelete(t->in);
    if (t->out) vStreamBufferDelete(t->out);
    free(t);
}

void term_push_in(term_t *t, const unsigned char *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (t->pty && d[i] == 0x03) {           // Ctrl-C: signal, never queued as data
            t->sigint = true;
            if (t->on_sigint) t->on_sigint();
            continue;
        }
        // Drop on overflow rather than block the SSH IO task.
        xStreamBufferSend(t->in, &d[i], 1, 0);
    }
}

size_t term_pop_out(term_t *t, unsigned char *buf, size_t n)
{
    return xStreamBufferReceive(t->out, buf, n, 0);
}

static int out_raw(term_t *t, const void *d, size_t n)
{
    const unsigned char *p = d;
    while (n && !t->closed) {
        size_t w = xStreamBufferSend(t->out, p, n, pdMS_TO_TICKS(200));
        p += w; n -= w;
    }
    return t->closed ? -1 : 0;
}

int term_write(term_t *t, const void *d, size_t n)
{
    if (!t->pty) return out_raw(t, d, n);
    const char *p = d; size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {                     // \n -> \r\n for terminals
            if (i > start && out_raw(t, p + start, i - start)) return -1;
            if (out_raw(t, "\r\n", 2)) return -1;
            start = i + 1;
        }
    }
    return start < n ? out_raw(t, p + start, n - start) : 0;
}

int term_puts(term_t *t, const char *s) { return term_write(t, s, strlen(s)); }

int term_printf(term_t *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    return n > 0 ? term_write(t, buf, n) : 0;
}

int term_getc(term_t *t, int timeout_ms)
{
    unsigned char c;
    TickType_t start = xTaskGetTickCount(), to = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (xStreamBufferReceive(t->in, &c, 1, pdMS_TO_TICKS(20)) == 1) return c;
        if (t->closed || t->eof) return -2;      // only after pending bytes are drained
        if (t->sigint && t->pty) return -3;
        if ((TickType_t)(xTaskGetTickCount() - start) >= to) return -1;
    }
}

static void redraw(term_t *t, const char *prompt, const char *buf, size_t len, size_t pos)
{
    term_puts(t, "\r\x1b[K");
    term_puts(t, prompt);
    term_write(t, buf, len);
    if (pos < len) { char m[16]; snprintf(m, sizeof m, "\x1b[%uD", (unsigned)(len - pos)); term_puts(t, m); }
}

int term_readline(term_t *t, const char *prompt, char *buf, size_t n, bool hist)
{
    size_t len = 0, pos = 0;                      // pos: cursor position inside buf (0..len)
    int hpos = t->hist_n;                         // == hist_n means "the new line being typed"
    char saved[TERM_LINE] = "";
    if (n > TERM_LINE) n = TERM_LINE;
    term_puts(t, prompt);
    for (;;) {
        int c = term_getc(t, 1000);
        if (c == -1) continue;
        if (c == -2) return -1;
        if (c == -3) { t->sigint = false; term_puts(t, "^C\n"); return -2; }
        if (c == 0x1b) {                          // escape sequence: arrows, Home/End, Delete; others are swallowed
            int c2 = term_getc(t, 50);
            if (c2 != '[' && c2 != 'O') continue;
            int fin, num = 0;
            for (;;) {
                fin = term_getc(t, 50);
                if (fin >= '0' && fin <= '9') { num = num * 10 + (fin - '0'); continue; }
                if (fin == ';') { num = 0; continue; }
                break;
            }
            if (hist && (fin == 'A' || fin == 'B')) {
                if (hpos == t->hist_n) { memcpy(saved, buf, len); saved[len] = 0; }
                if (fin == 'A' && hpos > 0) hpos--;
                else if (fin == 'B' && hpos < t->hist_n) hpos++;
                const char *src = hpos == t->hist_n ? saved : t->hist[hpos];
                len = strlen(src); if (len >= n) len = n - 1;
                memcpy(buf, src, len);
                pos = len;
                redraw(t, prompt, buf, len, pos);
            } else if (!t->noecho) {
                bool home = fin == 'H' || (fin == '~' && (num == 1 || num == 7));
                bool end = fin == 'F' || (fin == '~' && (num == 4 || num == 8));
                if (fin == 'D' && pos > 0) { pos--; term_puts(t, "\x1b[D"); }
                else if (fin == 'C' && pos < len) { pos++; term_puts(t, "\x1b[C"); }
                else if (home && pos) { pos = 0; redraw(t, prompt, buf, len, pos); }
                else if (end && pos < len) { pos = len; redraw(t, prompt, buf, len, pos); }
                else if (fin == '~' && num == 3 && pos < len) {          // Delete
                    memmove(buf + pos, buf + pos + 1, len - pos - 1); len--;
                    redraw(t, prompt, buf, len, pos);
                }
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            term_puts(t, "\n");
            buf[len] = 0;
            if (hist && len) {                    // append to history (drop oldest when full)
                if (t->hist_n == TERM_HIST) { memmove(t->hist[0], t->hist[1], sizeof(t->hist[0]) * (TERM_HIST - 1)); t->hist_n--; }
                memcpy(t->hist[t->hist_n], buf, len + 1);
                t->hist_n++;
            }
            return (int)len;
        }
        if (c == 0x04) {                          // Ctrl-D: EOF on empty line, else delete under the cursor
            if (len == 0) return -1;
            if (pos < len && !t->noecho) { memmove(buf + pos, buf + pos + 1, len - pos - 1); len--; redraw(t, prompt, buf, len, pos); }
            continue;
        }
        if (c == 0x7f || c == 0x08) {             // Backspace: delete before the cursor
            if (pos) {
                memmove(buf + pos - 1, buf + pos, len - pos); len--; pos--;
                if (!t->noecho) { if (pos == len) term_puts(t, "\b \b"); else redraw(t, prompt, buf, len, pos); }
            }
            continue;
        }
        if (c == 0x01 && !t->noecho) { pos = 0; redraw(t, prompt, buf, len, pos); continue; }          // Ctrl-A
        if (c == 0x05 && !t->noecho) { pos = len; redraw(t, prompt, buf, len, pos); continue; }        // Ctrl-E
        if (c == 0x15) { len = pos = 0; redraw(t, prompt, buf, 0, 0); continue; }                      // Ctrl-U
        if (c == 0x0c) { term_puts(t, "\x1b[2J\x1b[H"); redraw(t, prompt, buf, len, pos); continue; }  // Ctrl-L
        if (c >= 0x20 && c < 0x7f && len < n - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos++] = (char)c; len++;
            if (!t->noecho) {
                if (pos == len) { char ch = (char)c; term_write(t, &ch, 1); }          // typing at the end: just echo
                else redraw(t, prompt, buf, len, pos);                                  // inserting in the middle
            }
        }
    }
}

int term_read_stdin(term_t *t, void *buf, size_t n)
{
    if (!t->pty) {                                // exec mode: raw bytes until EOF
        // Ask the SSH side for more only when nothing is buffered: reading the wire early would also consume
        // the client's EOF, and wolfSSH answers that with its own EOF, after which our output is discarded.
        if (xStreamBufferBytesAvailable(t->in) == 0) t->want_input = true;
        int c = term_getc(t, 30000);
        if (c == -2) { t->want_input = false; return 0; }
        if (c < 0) { t->want_input = false; return -1; }
        unsigned char *p = buf; size_t got = 0;
        p[got++] = (unsigned char)c;
        while (got < n && xStreamBufferReceive(t->in, &p[got], 1, 0) == 1) got++;
        t->want_input = false;
        return (int)got;
    }
    if (t->pend_pos >= t->pend_len) {             // pty: cooked line, delivered with its '\n'
        int r = term_readline(t, "", t->pend, sizeof t->pend - 1, false);
        if (r == -1) return 0;
        if (r < 0) return -1;
        t->pend[r++] = '\n';
        t->pend_len = r; t->pend_pos = 0;
    }
    size_t k = t->pend_len - t->pend_pos;
    if (k > n) k = n;
    memcpy(buf, t->pend + t->pend_pos, k);
    t->pend_pos += k;
    return (int)k;
}
