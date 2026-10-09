// pkg: package manager (step 1: read the package list).
//   pkg update          fetch the journals listed in /etc/pkg/sources.list (following "journal" links) into /var/pkg/index
//   pkg list [WORD]     list the known packages (optionally only names containing WORD); [i] = installed
//   pkg sources         show the sources
// Journal format: see journals/main.journal. Needs sys_http_get (ABI 4) and a correct clock (TLS certificates).
#include "mini.h"

#define SRC_FILE   "/etc/pkg/sources.list"
#define INDEX      "/var/pkg/index"
#define INDEX_TMP  "/var/pkg/index.tmp"
#define JOURNAL    "/var/pkg/journal.tmp"
#define MAX_JOURNALS 16
#define MAX_DEPTH    3
#define URL_LEN      200
#define LINE_LEN     480

static char seen[MAX_JOURNALS][URL_LEN];   // every journal URL ever queued
static unsigned char depth[MAX_JOURNALS];
static int njournals;

static int starts(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int valid_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}
static int is_hex64(const char *s) { int n = 0; for (; *s; s++, n++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0; return n == 64; }
static int valid_url(const char *s) { return (starts(s, "https://") || starts(s, "http://")) && m_strlen(s) < URL_LEN; }

// Split a line in place into at most max whitespace-separated words.
static int split(char *line, char **w, int max)
{
    int n = 0;
    while (*line && n < max) {
        while (is_sp(*line)) *line++ = 0;
        if (!*line) break;
        w[n++] = line;
        while (*line && !is_sp(*line)) line++;
    }
    while (is_sp(*line)) *line++ = 0;
    return n;
}

// Read the next line of fd into buf (without '\n'). Returns 0 at EOF. Over-long lines are cut.
static int rbuf_n, rbuf_i; static char rbuf[128];
static int next_line(int fd, char *buf, int cap)
{
    int o = 0, got = 0;
    for (;;) {
        if (rbuf_i >= rbuf_n) { rbuf_n = sys_read(fd, rbuf, sizeof rbuf); rbuf_i = 0; if (rbuf_n <= 0) { rbuf_n = 0; break; } }
        char c = rbuf[rbuf_i++]; got = 1;
        if (c == '\n') break;
        if (o < cap - 1) buf[o++] = c;
    }
    buf[o] = 0;
    return got;
}
static void reader_reset(void) { rbuf_n = rbuf_i = 0; }

static int index_has(const char *name)
{
    int fd = sys_open(INDEX_TMP, 0), found = 0;
    if (fd < 0) return 0;
    reader_reset();
    char line[LINE_LEN];
    int nl = (int)m_strlen(name);
    while (!found && next_line(fd, line, sizeof line)) {
        if (starts(line, name) && line[nl] == ' ') found = 1;
    }
    sys_close(fd);
    return found;
}

static void queue(const char *url, int d)
{
    for (int i = 0; i < njournals; i++) if (!m_strcmp(seen[i], url)) return;
    if (njournals >= MAX_JOURNALS || d > MAX_DEPTH) { m_eprintf("  skipped (limit): %s\n", url); return; }
    int k = 0; while (url[k]) { seen[njournals][k] = url[k]; k++; } seen[njournals][k] = 0;
    depth[njournals++] = (unsigned char)d;
}

static int add_pkg(int out, char **w, int nw)       // pkg NAME VERSION URL key=value...
{
    if (nw < 4 || !valid_name(w[1]) || !valid_url(w[3]) || m_strlen(w[2]) > 24) return -1;
    const char *sha = 0; int size = 0, abi = 0;
    for (int i = 4; i < nw; i++) {
        if (starts(w[i], "sha256=")) sha = w[i] + 7;
        else if (starts(w[i], "size=")) size = m_atoi(w[i] + 5);
        else if (starts(w[i], "abi=")) abi = m_atoi(w[i] + 4);
    }
    if (!sha || !is_hex64(sha) || size <= 0) return -1;
    if (index_has(w[1])) return -2;                  // an earlier journal already provides it
    char rec[LINE_LEN];
    int n = m_snprintf(rec, sizeof rec, "%s %s %d %d %s %s\n", w[1], w[2], size, abi, sha, w[3]);
    sys_write(out, rec, n);
    return 0;
}

static int cmd_update(void)
{
    if (sys_time_state() == 0) m_eputs("pkg: warning: the clock is not set; HTTPS certificate checks may fail (see: ntp)\n");
    sys_mkdir("/var"); sys_mkdir("/var/pkg");
    int sfd = sys_open(SRC_FILE, 0);
    if (sfd < 0) { m_eputs("pkg: no " SRC_FILE "\n"); return 1; }
    reader_reset();
    char line[LINE_LEN];
    njournals = 0;
    while (next_line(sfd, line, sizeof line)) {
        char *w[2]; int nw = split(line, w, 2);
        if (nw < 1 || w[0][0] == '#') continue;
        if (valid_url(w[0])) queue(w[0], 0); else m_eprintf("pkg: bad source ignored: %s\n", w[0]);
    }
    sys_close(sfd);
    if (!njournals) { m_eputs("pkg: no sources\n"); return 1; }

    int out = sys_open(INDEX_TMP, 1);
    if (out < 0) { m_eputs("pkg: cannot write " INDEX_TMP "\n"); return 1; }
    int fetched = 0, failed = 0, packages = 0, dups = 0;
    for (int j = 0; j < njournals; j++) {            // breadth-first: njournals grows while we walk
        m_printf("fetch %s\n", seen[j]);
        int r = sys_http_get(seen[j], JOURNAL, 65536, 15000);
        if (r < 0) {
            failed++;
            if (r == -4) { m_eputs("interrupted\n"); sys_close(out); sys_unlink(INDEX_TMP); return 130; }
            m_eprintf("  failed (%d)%s\n", r, r == -1 ? ": network/TLS error (clock? repo private?)" : r == -404 ? ": not found" : "");
            continue;
        }
        fetched++;
        int fd = sys_open(JOURNAL, 0);
        if (fd < 0) { failed++; continue; }
        reader_reset();
        int ln = 0;
        while (next_line(fd, line, sizeof line)) {
            ln++;
            char *w[12]; int nw = split(line, w, 12);
            if (nw < 1 || w[0][0] == '#') continue;
            if (!m_strcmp(w[0], "journal") && nw >= 2 && valid_url(w[1])) queue(w[1], depth[j] + 1);
            else if (!m_strcmp(w[0], "pkg")) {
                int a = add_pkg(out, w, nw);
                if (a == 0) packages++; else if (a == -2) dups++; else m_eprintf("  line %d: bad pkg entry ignored\n", ln);
            } else m_eprintf("  line %d: unknown entry ignored\n", ln);
        }
        sys_close(fd);
    }
    sys_close(out);
    sys_unlink(JOURNAL);
    if (!fetched) { sys_unlink(INDEX_TMP); m_eputs("pkg: no journal could be fetched; keeping the old index\n"); return 1; }
    sys_rename(INDEX_TMP, INDEX);
    m_printf("%d journals read%s, %d packages%s\n", fetched, failed ? " (some failed)" : "", packages, dups ? " (duplicates skipped)" : "");
    return failed ? 2 : 0;
}

static int contains(const char *s, const char *w)
{
    for (; *s; s++) if (starts(s, w)) return 1;
    return 0;
}

static int cmd_list(const char *filter)
{
    int fd = sys_open(INDEX, 0);
    if (fd < 0) { m_eputs("pkg: no package list yet; run: pkg update\n"); return 1; }
    reader_reset();
    char line[LINE_LEN], path[64]; int n = 0;
    while (next_line(fd, line, sizeof line)) {
        char *w[6]; if (split(line, w, 6) < 6) continue;          // name version size abi sha url
        if (filter && !contains(w[0], filter)) continue;
        m_snprintf(path, sizeof path, "/esp/.local/bin/%s.aot", w[0]);
        m_printf("%s %s", sys_stat(path) == 1 ? "[i]" : "   ", w[0]);
        for (int k = (int)m_strlen(w[0]); k < 18; k++) m_puts(" ");
        m_printf("%s  %s B  abi %s\n", w[1], w[2], w[3]);
        n++;
    }
    sys_close(fd);
    if (!n) m_puts("(no packages)\n");
    return 0;
}

static int cmd_sources(void)
{
    int fd = sys_open(SRC_FILE, 0);
    if (fd < 0) { m_eputs("pkg: no " SRC_FILE "\n"); return 1; }
    char buf[128]; int r;
    while ((r = sys_read(fd, buf, sizeof buf)) > 0) m_write(1, buf, (size_t)r);
    sys_close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !m_strcmp(argv[1], "update")) return cmd_update();
    if (argc >= 2 && !m_strcmp(argv[1], "list")) return cmd_list(argc >= 3 ? argv[2] : 0);
    if (argc >= 2 && !m_strcmp(argv[1], "sources")) return cmd_sources();
    m_eputs("usage: pkg update | list [WORD] | sources\n");
    return 1;
}
