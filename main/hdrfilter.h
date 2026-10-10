// Request-header filter for the web server. The ESP-IDF HTTP server keeps ALL request headers in one small buffer and
// answers "431 Header fields are too long" when they do not fit; browsers send big Cookie headers (cookies are shared
// by every port of a host) that the board never uses. This removes such header lines from the byte stream before the
// server parses it. Pure functions, no I/O: tools can test it on the host (see www.c for the use).
#pragma once
#include <stddef.h>
#include <string.h>

#define HF_HOLD 8                       // longest dropped header name incl. ':' ("referer:")

struct hf { unsigned char mode, n; char hold[HF_HOLD]; };
enum { HF_REQ, HF_START, HF_NAME, HF_KEEP, HF_DROP };   // request line | line start | reading name | keep line | drop line

static inline void hf_init(struct hf *s) { s->mode = HF_REQ; s->n = 0; }

static inline int hf_dropped(const char *h, int n)      // is h[0..n) (lower-cased compare) a name we remove?
{
    static const char *const names[] = { "cookie:", "referer:" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        if ((int)strlen(names[i]) != n) continue;
        int k = 0;
        while (k < n && ((h[k] | 0x20) == names[i][k] || (h[k] == names[i][k]))) k++;
        if (k == n) return 1;
    }
    return 0;
}

// Filters buf[start .. start+n) in place; the result is written from buf[0] and its length returned. start must be
// >= HF_HOLD so held-back bytes always fit (the output never overtakes the input).
static inline size_t hf_run(struct hf *s, char *buf, size_t start, size_t n)
{
    size_t o = 0;
    for (size_t i = start; i < start + n; i++) {
        char c = buf[i];
        switch (s->mode) {
        case HF_REQ: buf[o++] = c; if (c == '\n') s->mode = HF_START; break;
        case HF_START:
            if (c == '\r' || c == '\n') { buf[o++] = c; if (c == '\n') s->mode = HF_REQ; else s->mode = HF_START; break; }
            s->n = 0; s->mode = HF_NAME;
            /* fall through */
        case HF_NAME:
            s->hold[s->n++] = c;
            if (c == ':' || c == '\n' || s->n == HF_HOLD) {
                if (c == ':' && hf_dropped(s->hold, s->n)) { s->mode = HF_DROP; break; }
                for (int k = 0; k < s->n; k++) buf[o++] = s->hold[k];
                s->mode = c == '\n' ? HF_START : HF_KEEP;
            }
            break;
        case HF_KEEP: buf[o++] = c; if (c == '\n') s->mode = HF_START; break;
        case HF_DROP: if (c == '\n') s->mode = HF_START; break;
        }
    }
    return o;
}
