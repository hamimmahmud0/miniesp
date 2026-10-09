// wc [FILE...]: counts lines, words and bytes (stdin if no file) - shows file access through the sys_* calls (relative paths use the shell's cwd).
#include "mini.h"

int main(int argc, char **argv)
{
    int rc = 0, nfiles = argc > 1 ? argc - 1 : 1;                   // no file: count stdin
    for (int i = 1; i <= nfiles; i++) {
        int fd = 0;
        if (argc > 1) fd = sys_open(argv[i], 0);
        if (fd < 0) { m_eprintf("wc: %s: cannot open\n", argv[i]); rc = 1; continue; }
        unsigned lines = 0, words = 0, bytes = 0; int in_word = 0;
        char buf[128]; int n;
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
            for (int k = 0; k < n; k++) {
                char c = buf[k];
                if (c == '\n') lines++;
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
            bytes += (unsigned)n;
        }
        if (argc > 1) sys_close(fd);
        m_printf("%7u %7u %7u%s%s\n", lines, words, bytes, argc > 1 ? " " : "", argc > 1 ? argv[i] : "");
    }
    return rc;
}
