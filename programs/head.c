// head [-n N] [FILE]: first N lines (default 10) of FILE or stdin.
#include "mini.h"

int main(int argc, char **argv)
{
    int n = 10, i = 1, fd = 0;
    if (i + 1 < argc && argv[i][0] == '-' && argv[i][1] == 'n') { n = m_atoi(argv[i + 1]); i += 2; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') { n = m_atoi(argv[i] + 1); i++; }
    if (i < argc) { fd = sys_open(argv[i], 0); if (fd < 0) { m_eprintf("head: %s: cannot open\n", argv[i]); return 1; } }
    char buf[128]; int r, lines = 0;
    while (lines < n && (r = sys_read(fd, buf, sizeof buf)) > 0) {
        int k = 0;
        while (k < r && lines < n) { if (buf[k++] == '\n') lines++; }
        m_write(1, buf, (size_t)k);
    }
    return 0;
}
