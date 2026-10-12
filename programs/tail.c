// tail [-n N | -N] [-f] [FILE...]: the last N lines (default 10) of each FILE or of stdin.
// -f: keep the file open and print what is appended (every 0.5 s) until Ctrl-C. A file that shrinks or is replaced (a rotated log such as
// /log/kernel.log) is followed from its start. With several files each gets a "==> name <==" header; -f follows the last one only.
#include "mini.h"


// Print the last n lines of the open file `fd` (size bytes) and return the size.
static int tail_file(int fd, int size, int n)
{
    char b[256];
    int pos = size, need = n, start = 0;
    if (n <= 0) return size;
    if (size > 0) {                                           // does the file end with a newline? then that one does not start a line
        sys_seek(fd, size - 1, 0);
        if (sys_read(fd, b, 1) == 1 && b[0] != '\n') need--;
    }
    while (pos > 0 && need >= 0) {                            // scan backwards for the (n+1)th newline
        int len = pos > (int)sizeof b ? (int)sizeof b : pos;
        pos -= len;
        sys_seek(fd, pos, 0);
        len = sys_read(fd, b, len);
        if (len <= 0) break;
        for (int i = len - 1; i >= 0; i--) if (b[i] == '\n' && need-- == 0) { start = pos + i + 1; goto found; }
    }
    start = 0;
found:
    sys_seek(fd, start, 0);
    int r;
    while ((r = sys_read(fd, b, sizeof b)) > 0) m_write(1, b, (size_t)r);
    return size;
}

// stdin is not seekable: keep the last 8 KB and print its last n lines
static int tail_stdin(int n)
{
    static char ring[8192];
    int total = 0, r;
    char b[256];
    while ((r = sys_read(0, b, sizeof b)) > 0) for (int i = 0; i < r; i++) ring[total++ & 8191] = b[i];
    int have = total < 8192 ? total : 8192, first = total - have, lines = 0;
    int end = total;
    if (end > first && ring[(end - 1) & 8191] != '\n') lines--;           // unterminated last line counts as one
    int start = first;
    for (int p = end - 1; p >= first; p--) if (ring[p & 8191] == '\n' && ++lines > n) { start = p + 1; break; }
    for (int p = start; p < end; p++) m_write(1, &ring[p & 8191], 1);
    return 0;
}

int main(int argc, char **argv)
{
    int n = 10, follow = 0, files = 0, rc = 0;
    const char *names[8];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == 'n' && !a[2] && i + 1 < argc) n = m_atoi(argv[++i]);
        else if (a[0] == '-' && a[1] == 'n') n = m_atoi(a + 2);
        else if (a[0] == '-' && a[1] == 'f' && !a[2]) follow = 1;
        else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') n = m_atoi(a + 1);
        else if (a[0] == '-' && a[1]) { m_eprintf("tail: unknown option %s\nusage: tail [-n N] [-f] [FILE...]\n", a); return 2; }
        else if (files < 8) names[files++] = a;
    }
    if (n < 0) n = 0;
    if (!files) { if (follow) { m_eputs("tail: -f needs a file\n"); return 2; } tail_stdin(n); return 0; }
    int off = 0;
    for (int i = 0; i < files; i++) {
        int size = sys_fsize(names[i]);
        int fd = size >= 0 ? sys_open(names[i], 0) : -1;
        if (fd < 0) { m_eprintf("tail: cannot open '%s'\n", names[i]); rc = 1; continue; }
        if (files > 1) m_printf("%s==> %s <==\n", i ? "\n" : "", names[i]);
        off = tail_file(fd, size, n);
        sys_close(fd);
    }
    if (!follow) return rc;
    const char *f = names[files - 1];
    for (;;) {
        sys_sleep_ms(500);
        int size = sys_fsize(f);
        if (size < 0) continue;                                // briefly missing while it is being rotated
        if (size < off) { off = 0; }                           // truncated or replaced: start again from the beginning
        if (size > off) {
            int fd = sys_open(f, 0);
            if (fd < 0) continue;
            sys_seek(fd, off, 0);
            char b[256]; int r;
            while ((r = sys_read(fd, b, sizeof b)) > 0) { m_write(1, b, (size_t)r); off += r; }
            sys_close(fd);
        }
    }
}
