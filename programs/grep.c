// grep [-v] [-c] [-i] PATTERN [FILE...]: print lines containing PATTERN (plain substring), stdin if no file.
#include "mini.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int contains(const char *line, const char *pat, int icase)
{
    if (!*pat) return 1;
    for (; *line; line++) {
        const char *a = line, *b = pat;
        while (*a && *b && (icase ? lower(*a) == lower(*b) : *a == *b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int grep_fd(int fd, const char *pat, int inv, int count, int icase, const char *label)
{
    char line[256]; int l = 0, c, matches = 0; char ch;
    for (;;) {
        int r = sys_read(fd, &ch, 1);
        if (r <= 0 && l == 0) break;
        if (r > 0 && ch != '\n') { if (l < (int)sizeof line - 1) line[l++] = ch; continue; }
        line[l] = 0; l = 0;
        if (contains(line, pat, icase) != inv) {
            matches++;
            if (!count) { if (label) { m_puts(label); m_puts(":"); } m_puts(line); m_puts("\n"); }
        }
        if (r <= 0) break;
    }
    if (count) { if (label) { m_puts(label); m_puts(":"); } m_printf("%d\n", matches); }
    return matches;
}

int main(int argc, char **argv)
{
    int inv = 0, count = 0, icase = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'v') inv = 1; else if (*p == 'c') count = 1; else if (*p == 'i') icase = 1; }
    if (i >= argc) { m_eputs("usage: grep [-vci] PATTERN [FILE...]\n"); return 2; }
    const char *pat = argv[i++];
    int total = 0;
    if (i >= argc) return grep_fd(0, pat, inv, count, icase, 0) ? 0 : 1;
    for (; i < argc; i++) {
        int fd = sys_open(argv[i], 0);
        if (fd < 0) { m_eprintf("grep: %s: cannot open\n", argv[i]); continue; }
        total += grep_fd(fd, pat, inv, count, icase, argc - (i - 1) > 2 ? argv[i] : 0);
        sys_close(fd);
    }
    return total ? 0 : 1;
}
