#pragma once
// Terminal / line discipline shared by the SSH session task and the shell/program task.
//   SSH IO task  --term_push_in-->  [in stream]  --> shell, programs (stdin)
//   shell, programs (stdout) --term_write--> [out stream] --> SSH IO task
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#define TERM_HIST 8
#define TERM_LINE 256

typedef struct term {
    StreamBufferHandle_t in, out;
    bool pty;                       // true: interactive shell (echo, \n->\r\n); false: ssh host cmd
    volatile bool closed;           // transport gone
    volatile bool eof;              // client half-closed (exec mode stdin EOF)
    volatile bool sigint;           // Ctrl-C received
    int cols, rows;                 // terminal size reported by the client (default 80x24)
    volatile bool noecho;           // don't echo typed characters (password entry)
    volatile bool want_input;       // exec mode: the command is blocked waiting for stdin
    void (*on_sigint)(void);        // called from SSH IO task when Ctrl-C arrives
    char hist[TERM_HIST][TERM_LINE];
    int hist_n;
    char pend[TERM_LINE];           // cooked line handed to readers of tty stdin
    size_t pend_len, pend_pos;
} term_t;

term_t *term_new(bool pty);
void term_free(term_t *t);

// SSH side
void term_push_in(term_t *t, const unsigned char *d, size_t n);
size_t term_pop_out(term_t *t, unsigned char *buf, size_t n);

// Shell side
int term_write(term_t *t, const void *d, size_t n);
int term_puts(term_t *t, const char *s);
int term_printf(term_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int term_getc(term_t *t, int timeout_ms);                 // >=0 byte, -1 timeout, -2 eof/closed
// Cooked line input. Returns length (>=0), -1 EOF/closed, -2 interrupted (Ctrl-C).
int term_readline(term_t *t, const char *prompt, char *buf, size_t n, bool hist);
// stdin read for programs: pty -> cooked lines, exec -> raw bytes. 0 = EOF, <0 = error/interrupt.
int term_read_stdin(term_t *t, void *buf, size_t n);

// The terminal currently attached to /dev/pts (one session at a time).
void term_set_current(term_t *t);
term_t *term_current(void);
