// pkg: package manager (step 1: read the package list).
//   pkg update          fetch the journals listed in /etc/pkg/sources.list (following "journal" links) into /var/pkg/index
//   pkg list [WORD]     list the known packages (optionally only names containing WORD); [i] = installed
//   pkg info NAME       details of one package
//   pkg install NAME    download, verify (size + SHA-256 + abi) and install to ~/.local/bin/NAME.aot
//                       (a "bundle" package installs several files listed in a manifest, see journals/main.journal)
//   pkg remove NAME     delete ~/.local/bin/NAME.aot
//   pkg fix             reinstall recorded packages that are missing (e.g. after the filesystem was wiped)
//   pkg sources         show the sources
// Installed names are also kept in the persistent key-value store (key pkg.inst), which survives a wiped filesystem.
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
typedef struct { char b[128]; int n, i; } rd_t;       // one per open file: nested readers must not share state
static int next_line(int fd, rd_t *r, char *buf, int cap)
{
    int o = 0, got = 0;
    for (;;) {
        if (r->i >= r->n) { r->n = sys_read(fd, r->b, sizeof r->b); r->i = 0; if (r->n <= 0) { r->n = 0; break; } }
        char c = r->b[r->i++]; got = 1;
        if (c == '\n') break;
        if (o < cap - 1) buf[o++] = c;
    }
    buf[o] = 0;
    return got;
}

static int index_has(const char *name)
{
    int fd = sys_open(INDEX_TMP, 0), found = 0;
    if (fd < 0) return 0;
    rd_t rd = { .n = 0, .i = 0 };
    char line[LINE_LEN];
    int nl = (int)m_strlen(name);
    while (!found && next_line(fd, &rd, line, sizeof line)) {
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

static int add_pkg(int out, char **w, int nw, int bundle)       // pkg|bundle NAME VERSION URL key=value...
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
    int n = m_snprintf(rec, sizeof rec, "%s %s %d %d %s %s%s\n", w[1], w[2], size, abi, sha, w[3], bundle ? " bundle" : "");
    sys_write(out, rec, n);
    return 0;
}

static int cmd_update(void)
{
    if (sys_time_state() == 0) m_eputs("pkg: warning: the clock is not set; HTTPS certificate checks may fail (see: ntp)\n");
    sys_mkdir("/var"); sys_mkdir("/var/pkg");
    int sfd = sys_open(SRC_FILE, 0);
    if (sfd < 0) { m_eputs("pkg: no " SRC_FILE "\n"); return 1; }
    rd_t rd = { .n = 0, .i = 0 };
    char line[LINE_LEN];
    njournals = 0;
    while (next_line(sfd, &rd, line, sizeof line)) {
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
        rd_t rd = { .n = 0, .i = 0 };
        int ln = 0;
        while (next_line(fd, &rd, line, sizeof line)) {
            ln++;
            char *w[12]; int nw = split(line, w, 12);
            if (nw < 1 || w[0][0] == '#') continue;
            if (!m_strcmp(w[0], "journal") && nw >= 2 && valid_url(w[1])) queue(w[1], depth[j] + 1);
            else if (!m_strcmp(w[0], "pkg") || !m_strcmp(w[0], "bundle")) {
                int a = add_pkg(out, w, nw, w[0][0] == 'b');
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
    rd_t rd = { .n = 0, .i = 0 };
    char line[LINE_LEN], path[64]; int n = 0;
    while (next_line(fd, &rd, line, sizeof line)) {
        char *w[7] = { 0, 0, 0, 0, 0, 0, 0 }; if (split(line, w, 7) < 6) continue;          // name version size abi sha url [bundle]
        if (filter && !contains(w[0], filter)) continue;
        if (w[6]) m_snprintf(path, sizeof path, "/var/pkg/%s.manifest", w[0]); else m_snprintf(path, sizeof path, "/esp/.local/bin/%s.aot", w[0]);
        m_printf("%s %s", sys_stat(path) == 1 ? "[i]" : "   ", w[0]);
        for (int k = (int)m_strlen(w[0]); k < 18; k++) m_puts(" ");
        m_printf("%s  %s B  abi %s%s\n", w[1], w[2], w[3], w[6] ? "  (bundle)" : "");
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

/* ---- installed-package record (NVS, survives a filesystem wipe) ---- */
#define INST_KEY "pkg.inst"
static char inst[600];
static void inst_load(void) { int n = sys_kv_get(INST_KEY, inst, (int)sizeof inst - 1); inst[n > 0 ? n : 0] = 0; }
static int inst_has(const char *name)
{
    int nl = (int)m_strlen(name);
    for (const char *p = inst; *p; ) {
        while (*p == ' ') p++;
        const char *q = p; while (*q && *q != ' ') q++;
        if (q - p == nl) { int i = 0; while (i < nl && p[i] == name[i]) i++; if (i == nl) return 1; }
        p = q;
    }
    return 0;
}
static void inst_add(const char *name)
{
    inst_load();
    int l = (int)m_strlen(inst), nl = (int)m_strlen(name);
    if (inst_has(name) || l + nl + 2 >= (int)sizeof inst) return;
    if (l) inst[l++] = ' ';
    for (int i = 0; i <= nl; i++) inst[l + i] = name[i];
    sys_kv_set(INST_KEY, inst, (int)m_strlen(inst));
}
static void inst_del(const char *name)
{
    inst_load();
    char out[600]; int o = 0, nl = (int)m_strlen(name);
    for (const char *p = inst; *p; ) {
        while (*p == ' ') p++;
        const char *q = p; while (*q && *q != ' ') q++;
        int same = q - p == nl; for (int i = 0; same && i < nl; i++) if (p[i] != name[i]) same = 0;
        if (!same && q > p) { if (o) out[o++] = ' '; for (const char *c = p; c < q; c++) out[o++] = *c; }
        p = q;
    }
    out[o] = 0;
    if (o) sys_kv_set(INST_KEY, out, o); else sys_kv_del(INST_KEY);
}

/* ---- SHA-256 (streaming over a file) ---- */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
// Hex SHA-256 of a file into hex[65]; returns 0, or -1 if it cannot be read.
static int sha256_file(const char *path, char *hex)
{
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    unsigned char blk[64]; uint32_t total = 0; int n = 0, r;
    while ((r = sys_read(fd, blk + n, 64 - n)) > 0) {
        n += r; total += (uint32_t)r;
        if (n == 64) { sha_block(h, blk); n = 0; }
    }
    sys_close(fd);
    blk[n++] = 0x80;
    if (n > 56) { while (n < 64) blk[n++] = 0; sha_block(h, blk); n = 0; }
    while (n < 56) blk[n++] = 0;
    uint32_t bits_hi = total >> 29, bits_lo = total << 3;
    for (int i = 0; i < 4; i++) { blk[56 + i] = (unsigned char)(bits_hi >> (24 - 8*i)); blk[60 + i] = (unsigned char)(bits_lo >> (24 - 8*i)); }
    sha_block(h, blk);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) hex[i*8 + j] = "0123456789abcdef"[(h[i] >> (28 - 4*j)) & 15];
    hex[64] = 0;
    return 0;
}

// Find NAME in the index; fills the words (name version size abi sha url [bundle]; w[6] is NULL for a plain program) of line. Returns 0 if found.
static int find_pkg(const char *name, char *line, int cap, char **w)
{
    int fd = sys_open(INDEX, 0);
    if (fd < 0) { m_eputs("pkg: no package list yet; run: pkg update\n"); return -1; }
    rd_t rd = { .n = 0, .i = 0 };
    int found = -2;
    while (next_line(fd, &rd, line, cap)) {
        w[6] = 0;
        if (split(line, w, 7) >= 6 && !m_strcmp(w[0], name)) { found = 0; break; }
    }
    sys_close(fd);
    if (found) m_eprintf("pkg: no package named '%s'\n", name);
    return found;
}

static void bin_path(const char *name, char *path, int n) { m_snprintf(path, n, "/esp/.local/bin/%s.aot", name); }

/* ---- bundle packages: several files described by a manifest ---- */
// Manifest lines ('#' comments):  dir PATH | file PATH URL sha256=HEX size=N | copy SRC DEST | service NAME | note TEXT...
// Paths must be absolute under /esp/, /www/, /etc/ or /var/ (no "..").
static int path_ok(const char *p)
{
    if (!(starts(p, "/esp/") || starts(p, "/www/") || starts(p, "/etc/") || starts(p, "/var/")) || m_strlen(p) > 90) return 0;
    for (const char *c = p; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.' || *c == '/')) return 0;
        if (c[0] == '.' && c[1] == '.') return 0;
    }
    return 1;
}
static void mkdir_parents(const char *path)                  // create every directory above the file name
{
    char b[100];
    for (int i = 1; path[i] && i < 99; i++)
        if (path[i] == '/') { for (int k = 0; k < i; k++) b[k] = path[k]; b[i] = 0; sys_mkdir(b); }
}
static int copy_file(const char *src, const char *dst)
{
    int in = sys_open(src, 0); if (in < 0) return -1;
    int out = sys_open(dst, 1); if (out < 0) { sys_close(in); return -1; }
    char buf[256]; int r, bad = 0;
    while ((r = sys_read(in, buf, sizeof buf)) > 0) if (sys_write(out, buf, r) != r) { bad = 1; break; }
    sys_close(in); sys_close(out);
    if (bad) sys_unlink(dst);
    return bad ? -1 : 0;
}
static void run_service(const char *verb, const char *name)   // built-in "service VERB NAME" (no program memory needed)
{
    char blob[64]; int n = m_snprintf(blob, sizeof blob, "service%c%s%c%s", 0, verb, 0, name) + 1;
    sys_run(blob, n, 3, 0, 1, 2);
}
static void manifest_path(const char *name, char *out, int n) { m_snprintf(out, n, "/var/pkg/%s.manifest", name); }

// is every file of an installed bundle present?
static int bundle_complete(const char *name)
{
    char mp[64]; manifest_path(name, mp, sizeof mp);
    int fd = sys_open(mp, 0); if (fd < 0) return 0;
    rd_t rd = { .n = 0, .i = 0 }; char line[LINE_LEN], *w[6]; int ok = 1;
    while (ok && next_line(fd, &rd, line, sizeof line)) {
        int nw = split(line, w, 6);
        if (nw >= 3 && (!m_strcmp(w[0], "file") || !m_strcmp(w[0], "copy"))) { if (sys_stat(m_strcmp(w[0], "copy") ? w[1] : w[2]) != 1) ok = 0; }
    }
    sys_close(fd);
    return ok;
}

static int install_bundle(const char *name, char **w)
{
    int size = m_atoi(w[2]), abi = m_atoi(w[3]);
    if (abi > sys_abi()) { m_eprintf("pkg: %s needs syscall ABI %d, this firmware has %d: update the firmware first\n", name, abi, sys_abi()); return 1; }
    char mp[64], mnew[72], hex[65]; manifest_path(name, mp, sizeof mp); m_snprintf(mnew, sizeof mnew, "%s.new", mp);
    sys_mkdir("/var"); sys_mkdir("/var/pkg");
    m_printf("fetch manifest %s\n", w[5]);
    int r = sys_http_get(w[5], mnew, size, 20000);
    if (r < 0) { m_eprintf("pkg: manifest download failed (%d)\n", r); return r == -4 ? 130 : 1; }
    if (r != size || sha256_file(mnew, hex) || m_strcmp(hex, w[4])) { m_eputs("pkg: manifest verification failed\n"); sys_unlink(mnew); return 1; }

    char line[LINE_LEN], *f[6]; int fd, nfiles = 0, ln = 0;
    // pass 1: validate everything before touching the filesystem
    fd = sys_open(mnew, 0); if (fd < 0) return 1;
    rd_t rd = { .n = 0, .i = 0 };
    while (next_line(fd, &rd, line, sizeof line)) {
        ln++;
        int nw = split(line, f, 6), bad = 0;
        if (nw < 1 || f[0][0] == '#') continue;
        if (!m_strcmp(f[0], "dir")) bad = nw < 2 || !path_ok(f[1]);
        else if (!m_strcmp(f[0], "file")) bad = nw < 5 || !path_ok(f[1]) || !valid_url(f[2]) || !starts(f[3], "sha256=") || !is_hex64(f[3] + 7) || !starts(f[4], "size=") || m_atoi(f[4] + 5) <= 0;
        else if (!m_strcmp(f[0], "copy")) bad = nw < 3 || !path_ok(f[1]) || !path_ok(f[2]);
        else if (!m_strcmp(f[0], "service")) bad = nw < 2 || !valid_name(f[1]);
        else if (m_strcmp(f[0], "note")) bad = 1;
        if (bad) { m_eprintf("pkg: manifest line %d is invalid; nothing installed\n", ln); sys_close(fd); sys_unlink(mnew); return 1; }
    }
    sys_close(fd);

    // pass 2: create directories, download + verify files, copy
    static char notes[300]; notes[0] = 0;
    fd = sys_open(mnew, 0); if (fd < 0) return 1;
    rd.n = rd.i = 0;
    while (next_line(fd, &rd, line, sizeof line)) {
        int nw = split(line, f, 6);
        if (nw < 1 || f[0][0] == '#') continue;
        if (!m_strcmp(f[0], "dir")) { mkdir_parents(f[1]); sys_mkdir(f[1]); }
        else if (!m_strcmp(f[0], "file")) {
            int fs = m_atoi(f[4] + 5);
            mkdir_parents(f[1]);
            if (sys_stat(f[1]) == 1 && sys_fsize(f[1]) == fs && !sha256_file(f[1], hex) && !m_strcmp(hex, f[3] + 7)) { m_printf("  ok      %s\n", f[1]); nfiles++; continue; }
            m_printf("  fetch   %s (%d B)\n", f[1], fs);
            int g = sys_http_get(f[2], f[1], fs, 30000);
            if (g < 0 || g != fs || sha256_file(f[1], hex) || m_strcmp(hex, f[3] + 7)) {
                m_eprintf("pkg: %s failed (%d); the bundle is incomplete, run pkg install %s again\n", f[1], g, name);
                if (g >= 0) sys_unlink(f[1]);
                sys_close(fd); sys_unlink(mnew); return g == -4 ? 130 : 1;
            }
            nfiles++;
        }
        else if (!m_strcmp(f[0], "copy")) {
            mkdir_parents(f[2]);
            if (copy_file(f[1], f[2])) { m_eprintf("pkg: cannot copy %s to %s\n", f[1], f[2]); sys_close(fd); sys_unlink(mnew); return 1; }
            m_printf("  copy    %s\n", f[2]); nfiles++;
        }
    }
    sys_close(fd);
    // notes: re-read raw lines (split() cut them at the spaces)
    fd = sys_open(mnew, 0);
    if (fd >= 0) {
        rd.n = rd.i = 0;
        while (next_line(fd, &rd, line, sizeof line)) if (starts(line, "note ")) { int o = (int)m_strlen(notes); m_snprintf(notes + o, (int)sizeof notes - o, "%s\n", line + 5); }
        sys_close(fd);
    }
    sys_unlink(mp);
    if (sys_rename(mnew, mp)) { m_eprintf("pkg: cannot record %s\n", mp); return 1; }
    inst_add(name);
    m_printf("installed bundle %s %s (%d files)\n", name, w[1], nfiles);
    if (notes[0]) m_puts(notes);
    return 0;
}

static int remove_bundle(const char *name)
{
    char mp[64]; manifest_path(name, mp, sizeof mp);
    int fd = sys_open(mp, 0); if (fd < 0) return -1;
    rd_t rd = { .n = 0, .i = 0 }; char line[LINE_LEN], *f[6], dirs[600]; int nd = 0, nf = 0; dirs[0] = 0;
    while (next_line(fd, &rd, line, sizeof line)) {
        int nw = split(line, f, 6);
        if (nw < 2 || f[0][0] == '#') continue;
        if (!m_strcmp(f[0], "file") && path_ok(f[1])) { if (!sys_unlink(f[1])) nf++; }
        else if (!m_strcmp(f[0], "copy") && nw >= 3 && path_ok(f[2])) { if (!sys_unlink(f[2])) nf++; }
        else if (!m_strcmp(f[0], "service") && valid_name(f[1])) { run_service("stop", f[1]); run_service("rm", f[1]); }
        else if (!m_strcmp(f[0], "dir") && path_ok(f[1]) && nd + (int)m_strlen(f[1]) + 2 < (int)sizeof dirs) { nd += m_snprintf(dirs + nd, (int)sizeof dirs - nd, "%s\n", f[1]); }
    }
    sys_close(fd);
    for (int pass = 0; pass < 3; pass++) {                       // remove now-empty directories (children first, by repetition)
        char tmp[600]; int k = 0; while (dirs[k] && k < (int)sizeof tmp - 1) { tmp[k] = dirs[k]; k++; } tmp[k] = 0;
        for (char *q = tmp; *q;) { char *e = q; while (*e && *e != '\n') e++; int more = *e; *e = 0; if (*q) sys_rmdir(q); if (!more) break; q = e + 1; }
    }
    sys_unlink(mp);
    inst_del(name);
    m_printf("removed bundle %s (%d files; settings in the key-value store and data files such as history are kept)\n", name, nf);
    return 0;
}

static int cmd_install(const char *name)
{
    char line[LINE_LEN], *w[7];
    if (find_pkg(name, line, sizeof line, w)) return 1;
    if (w[6]) return install_bundle(name, w);
    int size = m_atoi(w[2]), abi = m_atoi(w[3]);
    if (abi > sys_abi()) { m_eprintf("pkg: %s needs syscall ABI %d, this firmware has %d: update the firmware first\n", name, abi, sys_abi()); return 1; }
    m_printf("fetch %s (%d B)\n", w[5], size);
    sys_mkdir("/var"); sys_mkdir("/var/pkg");
    const char *tmp = "/var/pkg/dl.tmp";
    int r = sys_http_get(w[5], tmp, size, 20000);
    if (r < 0) {
        m_eprintf("pkg: download failed (%d)%s\n", r, r == -3 ? ": larger than the journal says" : r == -404 ? ": not found" : "");
        return r == -4 ? 130 : 1;
    }
    char hex[65];
    if (r != size || sha256_file(tmp, hex) || m_strcmp(hex, w[4])) {
        m_eprintf("pkg: verification failed (size %d, expected %d; sha256 %s)\n", r, size, r == size ? hex : "-");
        sys_unlink(tmp);
        return 1;
    }
    char path[64]; bin_path(name, path, sizeof path);
    sys_mkdir("/esp/.local"); sys_mkdir("/esp/.local/bin");
    if (sys_rename(tmp, path)) { m_eprintf("pkg: cannot write %s\n", path); sys_unlink(tmp); return 1; }
    inst_add(name);
    m_printf("installed %s %s -> %s (sha256 ok)\n", name, w[1], path);
    return 0;
}

static int cmd_remove(const char *name)
{
    char path[64]; bin_path(name, path, sizeof path);
    if (valid_name(name) && !remove_bundle(name)) return 0;
    if (!valid_name(name) || sys_stat(path) != 1) { m_eprintf("pkg: %s is not installed\n", name); return 1; }
    if (sys_unlink(path)) { m_eputs("pkg: cannot remove\n"); return 1; }
    inst_del(name);
    m_printf("removed %s\n", name);
    return 0;
}

static int cmd_info(const char *name)
{
    char line[LINE_LEN], *w[7], path[64];
    if (find_pkg(name, line, sizeof line, w)) return 1;
    if (w[6]) manifest_path(name, path, sizeof path); else bin_path(name, path, sizeof path);
    m_printf("name:      %s\nversion:   %s\ntype:      %s\nsize:      %s B%s\nabi:       %s\nsha256:    %s\nurl:       %s\ninstalled: %s\n",
             w[0], w[1], w[6] ? "bundle (several files)" : "program", w[2], w[6] ? " (manifest)" : "", w[3], w[4], w[5], sys_stat(path) == 1 ? "yes" : "no");
    return 0;
}

static int cmd_fix(void)
{
    inst_load();
    if (!inst[0]) { m_puts("pkg: no packages recorded\n"); return 0; }
    static char todo[600]; int n = 0, tl = 0;
    for (const char *p = inst; *p; ) {                       // names whose file is missing
        while (*p == ' ') p++;
        const char *q = p; while (*q && *q != ' ') q++;
        char name[40], path[64]; int l = (int)(q - p);
        if (l > 0 && l < (int)sizeof name) {
            for (int i = 0; i < l; i++) name[i] = p[i];
            name[l] = 0; bin_path(name, path, sizeof path);
            char mp[64]; manifest_path(name, mp, sizeof mp);
            int missing = sys_stat(mp) == 1 ? !bundle_complete(name) : sys_stat(path) != 1;      // a bundle is complete when all its files exist
            if (missing) { for (int i = 0; i <= l; i++) todo[tl + i] = name[i]; tl += l + 1; n++; }
        }
        p = q;
    }
    if (!n) { m_puts("pkg: all recorded packages are installed\n"); return 0; }
    m_printf("pkg: %d missing package(s), reinstalling\n", n);
    if (sys_stat(INDEX) != 1 && cmd_update() == 1) return 1;
    int bad = 0;
    for (int i = 0, off = 0; i < n; i++) { if (cmd_install(todo + off)) bad++; off += (int)m_strlen(todo + off) + 1; }
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !m_strcmp(argv[1], "update")) return cmd_update();
    if (argc >= 2 && !m_strcmp(argv[1], "list")) return cmd_list(argc >= 3 ? argv[2] : 0);
    if (argc >= 2 && !m_strcmp(argv[1], "fix")) return cmd_fix();
    if (argc >= 3 && !m_strcmp(argv[1], "install")) return cmd_install(argv[2]);
    if (argc >= 3 && !m_strcmp(argv[1], "remove")) return cmd_remove(argv[2]);
    if (argc >= 3 && !m_strcmp(argv[1], "info")) return cmd_info(argv[2]);
    if (argc >= 2 && !m_strcmp(argv[1], "sources")) return cmd_sources();
    m_eputs("usage: pkg update | list [WORD] | info NAME | install NAME | remove NAME | fix | sources\n");
    return 1;
}
