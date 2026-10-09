#pragma once
// A tiny I/O abstraction shared by the shell and the AOT runner: a stream is the terminal,
// a LittleFS file, or an in-RAM buffer (used for pipes).
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "term.h"

typedef enum { IO_TERM, IO_FILE, IO_MEM, IO_LOG, IO_SOCK } io_kind_t;

typedef struct io {
    io_kind_t kind;
    term_t *t;                       // IO_TERM
    FILE *f;                         // IO_FILE
    uint8_t *mem;                    // IO_MEM
    size_t len, cap, pos;            // IO_MEM: bytes stored / allocated / read position
    void (*sink)(void *ctx, const void *d, size_t n);   // IO_LOG: output goes to this callback (no input)
    void *sink_ctx;
    int sock;                        // IO_SOCK: lwIP socket (TCP stream, UDP or listening)
    int sock_type;                   // SOCK_STREAM / SOCK_DGRAM
    bool listening;
} io_t;

#define IO_MEM_MAX (24 * 1024)       // cap per in-RAM pipe buffer

void io_init_term(io_t *io, term_t *t);
io_t *io_open_file(const char *host_path, const char *mode);      // heap-allocated, NULL on error
io_t *io_new_mem(void);
io_t *io_new_log(void (*sink)(void *, const void *, size_t), void *ctx);   // write-only stream into a callback (services)                                            // heap-allocated
void io_close(io_t *io);                                           // closes + frees heap streams
int io_write(io_t *io, const void *d, size_t n);                   // 0 ok, -1 error
int io_read(io_t *io, void *d, size_t n);                          // bytes read, 0 = EOF, <0 error/interrupt (sockets: -2 = timeout)
void io_rewind(io_t *io);
long io_seek(io_t *io, long off, int whence);                     // files and RAM streams: new position, or -1
io_t *io_new_sock(int sock, int type, bool listening);              // takes ownership of the socket
int io_sock_timeout(io_t *io, int ms);                              // receive timeout (0 = default)
