#include "io.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void io_init_term(io_t *io, term_t *t)
{
    memset(io, 0, sizeof *io);
    io->kind = IO_TERM;
    io->t = t;
}

io_t *io_open_file(const char *host_path, const char *mode)
{
    FILE *f = fopen(host_path, mode);
    if (!f) return NULL;
    io_t *io = calloc(1, sizeof *io);
    if (!io) { fclose(f); return NULL; }
    io->kind = IO_FILE;
    io->f = f;
    return io;
}

io_t *io_new_mem(void)
{
    io_t *io = calloc(1, sizeof *io);
    if (io) io->kind = IO_MEM;
    return io;
}

io_t *io_new_log(void (*sink)(void *, const void *, size_t), void *ctx)
{
    io_t *io = calloc(1, sizeof *io);
    if (io) { io->kind = IO_LOG; io->sink = sink; io->sink_ctx = ctx; }
    return io;
}

io_t *io_new_sock(int sock, int type, bool listening)
{
    io_t *io = calloc(1, sizeof *io);
    if (!io) { close(sock); return NULL; }
    io->kind = IO_SOCK; io->sock = sock; io->sock_type = type; io->listening = listening;
    return io;
}

int io_sock_timeout(io_t *io, int ms)
{
    if (io->kind != IO_SOCK) return -1;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return setsockopt(io->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

void io_close(io_t *io)
{
    if (!io || io->kind == IO_TERM) return;       // the terminal is never closed or freed here
    if (io->kind == IO_FILE && io->f) fclose(io->f);
    if (io->kind == IO_SOCK) { shutdown(io->sock, SHUT_RDWR); close(io->sock); }
    if (io->kind == IO_MEM) free(io->mem);
    free(io);
}

int io_write(io_t *io, const void *d, size_t n)
{
    switch (io->kind) {
    case IO_TERM: return io->t && !io->t->closed ? term_write(io->t, d, n) : -1;
    case IO_FILE: return fwrite(d, 1, n, io->f) == n ? 0 : -1;
    case IO_LOG: io->sink(io->sink_ctx, d, n); return 0;
    case IO_SOCK: {
        const char *p = d;
        if (io->listening) return -1;
        while (n) {
            int w = send(io->sock, p, n, 0);
            if (w <= 0) { if (w < 0 && (errno == EAGAIN || errno == EINTR)) continue; return -1; }
            p += w; n -= (size_t)w;
        }
        return 0;
    }
    case IO_MEM:
        if (io->len + n > IO_MEM_MAX) return -1;
        if (io->len + n > io->cap) {
            size_t nc = io->cap ? io->cap * 2 : 512;
            while (nc < io->len + n) nc *= 2;
            uint8_t *m = realloc(io->mem, nc);
            if (!m) return -1;
            io->mem = m; io->cap = nc;
        }
        memcpy(io->mem + io->len, d, n);
        io->len += n;
        return 0;
    }
    return -1;
}

int io_read(io_t *io, void *d, size_t n)
{
    switch (io->kind) {
    case IO_TERM: return io->t ? term_read_stdin(io->t, d, n) : 0;
    case IO_FILE: return (int)fread(d, 1, n, io->f);
    case IO_LOG: return 0;                                          // write-only: reads see EOF
    case IO_SOCK: {
        if (io->listening) return -1;
        int r = recv(io->sock, d, n, 0);
        if (r >= 0) return r;                                       // 0 = peer closed
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1; // -2: receive timeout
    }
    case IO_MEM: {
        size_t avail = io->len > io->pos ? io->len - io->pos : 0;
        size_t k = avail < n ? avail : n;
        if (k) memcpy(d, io->mem + io->pos, k);
        io->pos += k;
        return (int)k;
    }
    }
    return -1;
}

void io_rewind(io_t *io)
{
    if (io->kind == IO_MEM) io->pos = 0;
    else if (io->kind == IO_FILE) rewind(io->f);
}

long io_seek(io_t *io, long off, int whence)
{
    if (io->kind == IO_FILE) { if (fseek(io->f, off, whence)) return -1; return ftell(io->f); }
    if (io->kind == IO_MEM) {
        long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)io->pos : (long)io->len;
        long np = base + off;
        if (np < 0 || np > (long)io->len) return -1;
        io->pos = (size_t)np;
        return np;
    }
    return -1;
}
