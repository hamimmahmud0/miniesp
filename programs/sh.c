// sh - a small bash-like shell, running as an AOT program on miniesp.
//
//   sh                   interactive (when stdin is a terminal) or reads a script from stdin
//   sh script.sh [args]  run a script ($0, $1.. $9, $#, $@)
//   sh -c 'cmd args'     run one command line
//
// Supported: pipelines (a | b | c), redirects (< > >> 2> 2>&1), lists (; && || newline),
// variables (NAME=value, $NAME, ${NAME}, $?, $#, $0-$9, $@), quoting ('..' ".." \x),
// command substitution $(...), comments, and
//   if/elif/else/fi, for/in/do/done, while/do/done, break, continue,
//   builtins: cd exit export unset shift read true false : test [ . source
// Everything else runs through the OS (sys_run): built-in commands such as ls/cat/echo, or
// programs from ~/.local/bin and /bin. Not supported: globbing, functions, background jobs, subshells.
#include "mini.h"

#define MAXW 24            // words per command
#define MAXSTG 6           // pipeline stages
#define MAXVARS 32
#define NAMELEN 20
#define VALLEN 100

/* ---------------- state ---------------- */
typedef struct { char *s; unsigned char t; } tok_t;
enum { T_WORD, T_OP, T_NL };

typedef struct ctx {
    tok_t *tk; int ntk, maxtk;
    char *tx; int txn, txmax;
    int pos;
    int out_fd;                  // default stdout of commands run in this context (1, or a membuf for $(...))
    char *xa; int xn, xmax;      // expansion arena
} ctx_t;

static tok_t tk0[320], tk1[64], tk2[64];
static char tx0[8192], tx1[1536], tx2[1536];
static char xa0[2048], xa1[1536], xa2[1536];
static ctx_t ctxs[3] = {
    { tk0, 0, 320, tx0, 0, sizeof tx0, 0, 1, xa0, 0, sizeof xa0 },
    { tk1, 0, 64, tx1, 0, sizeof tx1, 0, 1, xa1, 0, sizeof xa1 },
    { tk2, 0, 64, tx2, 0, sizeof tx2, 0, 1, xa2, 0, sizeof xa2 },
};
static ctx_t *C = &ctxs[0];
static int depth;

static struct { char n[NAMELEN]; char v[VALLEN]; char used; } vars[MAXVARS];
static const char *pargv[10];     // $0..$9
static int pargc;                 // number of positional parameters ($1..)
static int status;                // $?
enum { CTL_NONE, CTL_BREAK, CTL_CONT, CTL_EXIT };
static int ctl;                   // pending break/continue/exit
static int exit_code;

/* ---------------- small helpers ---------------- */
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static size_t slen(const char *s) { return m_strlen(s); }
static void out(int fd, const char *s) { m_write(fd, s, slen(s)); }
static void err(const char *s) { out(2, "sh: "); out(2, s); out(2, "\n"); }
static void err2(const char *s, const char *t) { out(2, "sh: "); out(2, s); out(2, t); out(2, "\n"); }

static int is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

static const char *var_get(const char *n, int len)
{
    static char tmp[12];
    if (len == 1) {
        char c = n[0];
        if (c == '?') { int v = status, i = 11; tmp[i] = 0; if (v < 0) v = 0; do { tmp[--i] = '0' + v % 10; v /= 10; } while (v); return tmp + i; }
        if (c == '#') { int v = pargc, i = 11; tmp[i] = 0; do { tmp[--i] = '0' + v % 10; v /= 10; } while (v); return tmp + i; }
        if (c >= '0' && c <= '9') { int k = c - '0'; return (k == 0 || k <= pargc) && pargv[k] ? pargv[k] : ""; }
    }
    for (int i = 0; i < MAXVARS; i++) {
        if (!vars[i].used) continue;
        int j = 0;
        while (j < len && vars[i].n[j] == n[j]) j++;
        if (j == len && vars[i].n[j] == 0) return vars[i].v;
    }
    return "";
}

static void var_set(const char *n, int nlen, const char *v)
{
    int free_slot = -1;
    for (int i = 0; i < MAXVARS; i++) {
        if (!vars[i].used) { if (free_slot < 0) free_slot = i; continue; }
        int j = 0;
        while (j < nlen && vars[i].n[j] == n[j]) j++;
        if (j == nlen && vars[i].n[j] == 0) { free_slot = i; goto set; }
    }
    if (free_slot < 0) { err("too many variables"); return; }
set:
    vars[free_slot].used = 1;
    for (int j = 0; j < nlen && j < NAMELEN - 1; j++) vars[free_slot].n[j] = n[j];
    vars[free_slot].n[nlen < NAMELEN - 1 ? nlen : NAMELEN - 1] = 0;
    int k = 0;
    while (v[k] && k < VALLEN - 1) { vars[free_slot].v[k] = v[k]; k++; }
    vars[free_slot].v[k] = 0;
}

static void var_unset(const char *n)
{
    for (int i = 0; i < MAXVARS; i++)
        if (vars[i].used && streq(vars[i].n, n)) vars[i].used = 0;
}

/* ---------------- lexer ---------------- */
static int add_tok(ctx_t *c, char *s, int t)
{
    if (c->ntk >= c->maxtk) return -1;
    c->tk[c->ntk].s = s; c->tk[c->ntk].t = (unsigned char)t; c->ntk++;
    return 0;
}

// Returns 0, -1 (too big) or -2 (unterminated quote / substitution: input is incomplete).
static int lex(ctx_t *c, const char *p)
{
    c->ntk = 0; c->txn = 0;
#define PUT(ch) do { if (c->txn >= c->txmax - 1) return -1; c->tx[c->txn++] = (ch); } while (0)
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (*p == '\n') { if (add_tok(c, "\n", T_NL)) return -1; p++; continue; }
        if (*p == ';') { if (add_tok(c, ";", T_OP)) return -1; p++; continue; }
        if (*p == '&') {
            if (p[1] == '&') { if (add_tok(c, "&&", T_OP)) return -1; p += 2; }
            else { if (add_tok(c, ";", T_OP)) return -1; p++; }                 // background: run in the foreground
            continue;
        }
        if (*p == '|') {
            if (p[1] == '|') { if (add_tok(c, "||", T_OP)) return -1; p += 2; }
            else { if (add_tok(c, "|", T_OP)) return -1; p++; }
            continue;
        }
        if (*p == '<') { if (add_tok(c, "<", T_OP)) return -1; p++; continue; }
        if (*p == '>') {
            if (p[1] == '>') { if (add_tok(c, ">>", T_OP)) return -1; p += 2; }
            else { if (add_tok(c, ">", T_OP)) return -1; p++; }
            continue;
        }
        if (*p == '2' && p[1] == '>') {
            if (p[2] == '&' && p[3] == '1') { if (add_tok(c, "2>&1", T_OP)) return -1; p += 4; }
            else { if (add_tok(c, "2>", T_OP)) return -1; p += 2; }
            continue;
        }
        char *w = c->tx + c->txn;
        while (*p) {
            char ch = *p;
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ';' || ch == '|' || ch == '&' || ch == '<' || ch == '>') break;
            if (ch == '\\' && p[1]) { PUT(ch); PUT(p[1]); p += 2; continue; }
            if (ch == '\'' || ch == '"') {
                PUT(ch); p++;
                while (*p && *p != ch) {
                    if (ch == '"' && *p == '\\' && p[1]) { PUT(*p); p++; }
                    PUT(*p); p++;
                }
                if (!*p) return -2;
                PUT(*p); p++;
                continue;
            }
            if (ch == '$' && p[1] == '(') {
                int d = 0;
                PUT(ch); p++;                                      // the '$', then the balanced (...)
                do { if (*p == '(') d++; else if (*p == ')') d--; PUT(*p); p++; } while (*p && d > 0);
                if (d > 0) return -2;
                continue;
            }
            if (ch == '$' && p[1] == '{') {
                while (*p && *p != '}') { PUT(*p); p++; }
                if (!*p) return -2;
                PUT(*p); p++;
                continue;
            }
            PUT(ch); p++;
        }
        PUT(0);
        if (add_tok(c, w, T_WORD)) return -1;
    }
#undef PUT
    return 0;
}

/* ---------------- arithmetic: $(( expr )) ---------------- */
static const char *ap;
static long ar_or(void);
static void ar_ws(void) { while (*ap == ' ' || *ap == '\t') ap++; }
static long ar_prim(void)
{
    ar_ws();
    if (*ap == '(') { ap++; long v = ar_or(); ar_ws(); if (*ap == ')') ap++; return v; }
    if (*ap == '-') { ap++; return -ar_prim(); }
    if (*ap == '+') { ap++; return ar_prim(); }
    if (*ap == '!') { ap++; return !ar_prim(); }
    if (*ap == '$') ap++;                                   // $i is accepted like i
    if (*ap >= '0' && *ap <= '9') { long v = 0; while (*ap >= '0' && *ap <= '9') v = v * 10 + (*ap++ - '0'); return v; }
    if (is_name_start(*ap)) { const char *s = ap; while (is_name_char(*ap)) ap++; return m_atoi(var_get(s, (int)(ap - s))); }
    return 0;
}
static long ar_mul(void)
{
    long v = ar_prim();
    for (;;) {
        ar_ws();
        char o = *ap;
        if (o != '*' && o != '/' && o != '%') return v;
        ap++;
        long r = ar_prim();
        if (o == '*') v *= r;
        else if (r == 0) { err("division by zero"); v = 0; }
        else v = o == '/' ? v / r : v % r;
    }
}
static long ar_add(void)
{
    long v = ar_mul();
    for (;;) { ar_ws(); if (*ap == '+') { ap++; v += ar_mul(); } else if (*ap == '-') { ap++; v -= ar_mul(); } else return v; }
}
static long ar_cmp(void)
{
    long v = ar_add();
    for (;;) {
        ar_ws();
        if (ap[0] == '=' && ap[1] == '=') { ap += 2; v = v == ar_add(); }
        else if (ap[0] == '!' && ap[1] == '=') { ap += 2; v = v != ar_add(); }
        else if (ap[0] == '<' && ap[1] == '=') { ap += 2; v = v <= ar_add(); }
        else if (ap[0] == '>' && ap[1] == '=') { ap += 2; v = v >= ar_add(); }
        else if (ap[0] == '<') { ap++; v = v < ar_add(); }
        else if (ap[0] == '>') { ap++; v = v > ar_add(); }
        else return v;
    }
}
static long ar_and(void) { long v = ar_cmp(); for (;;) { ar_ws(); if (ap[0] == '&' && ap[1] == '&') { ap += 2; long r = ar_cmp(); v = v && r; } else return v; } }
static long ar_or(void) { long v = ar_and(); for (;;) { ar_ws(); if (ap[0] == '|' && ap[1] == '|') { ap += 2; long r = ar_and(); v = v || r; } else return v; } }

/* ---------------- expansion ---------------- */
static int run_text(const char *text, int out_fd);

static char *xalloc(ctx_t *c, int n)
{
    if (c->xn + n > c->xmax) return 0;
    char *r = c->xa + c->xn;
    c->xn += n;
    return r;
}

typedef struct { char *w[MAXW]; int n; } wl_t;

// Expand one raw word into zero or more words appended to wl. Unquoted expansion results are split on blanks.
static int expand(ctx_t *c, const char *raw, wl_t *wl)
{
    char *cur = c->xa + c->xn;
    int len = 0, in_word = 0;
    char subbuf[512], abuf[16];                      // expansion results (kept out of the arena: it holds the word being built)
#define EMIT(ch) do { if (c->xn + len + 2 >= c->xmax) return -1; cur[len++] = (ch); in_word = 1; } while (0)
#define FLUSH() do { if (in_word) { if (wl->n >= MAXW - 1) return -1; cur[len] = 0; wl->w[wl->n++] = cur; c->xn += len + 1; cur = c->xa + c->xn; len = 0; in_word = 0; } } while (0)
    int q = 0;                               // 0 none, '"' in double quotes
    for (const char *p = raw; *p; p++) {
        char ch = *p;
        if (!q && ch == '\\' && p[1]) { EMIT(p[1]); p++; continue; }
        if (!q && ch == '\'') { in_word = 1; p++; while (*p && *p != '\'') { EMIT(*p); p++; } if (!*p) break; continue; }
        if (ch == '"') { q = q ? 0 : '"'; in_word = 1; continue; }
        if (q && ch == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { EMIT(p[1]); p++; continue; }
        if (ch != '$' || !p[1]) { EMIT(ch); continue; }

        // ---- $ expansions ----
        const char *val = 0; char *sub = 0;
        if (p[1] == '(' && p[2] == '(') {                              // $(( arithmetic ))
            int d = 0; const char *e = p + 1;
            do { if (*e == '(') d++; else if (*e == ')') d--; e++; } while (*e && d > 0);
            char ex[96]; int n = (int)(e - p) - 5;                     // text between "$((" and "))"
            if (n < 0) n = 0;
            if (n >= (int)sizeof ex) n = (int)sizeof ex - 1;
            for (int i = 0; i < n; i++) ex[i] = p[3 + i];
            ex[n] = 0;
            ap = ex;
            long v = ar_or();
            sub = abuf;
            int k = 14; sub[k] = 0; int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
            do { sub[--k] = '0' + u % 10; u /= 10; } while (u);
            if (neg) sub[--k] = '-';
            val = sub + k;
            p = e - 1;
        } else if (p[1] == '(') {
            int d = 0; const char *e = p + 1;
            do { if (*e == '(') d++; else if (*e == ')') d--; e++; } while (*e && d > 0);
            int n = (int)(e - (p + 2)) - 1;               // text between $( and )
            if (n < 0) n = 0;
            char inner[256];
            if (n >= (int)sizeof inner) { err("command substitution too long"); return -1; }
            for (int i = 0; i < n; i++) inner[i] = p[2 + i];
            inner[n] = 0;
            int fd = sys_membuf();
            if (fd < 0) { err("out of memory"); return -1; }
            run_text(inner, fd);
            sys_rewind(fd);
            sub = subbuf;
            int got = 0, r;
            while (got < 510 && (r = sys_read(fd, sub + got, 510 - got)) > 0) got += r;
            sys_close(fd);
            while (got > 0 && (sub[got - 1] == '\n' || sub[got - 1] == '\r')) got--;
            sub[got] = 0;
            val = sub;
            p = e - 1;
        } else if (p[1] == '{') {
            const char *e = p + 2; while (*e && *e != '}') e++;
            val = var_get(p + 2, (int)(e - (p + 2)));
            p = *e ? e : e - 1;
        } else if (p[1] == '@' || p[1] == '*') {
            static char all[VALLEN * 2]; int k = 0;
            for (int i = 1; i <= pargc && i < 10; i++) { if (i > 1) all[k++] = ' '; for (const char *s = pargv[i]; *s && k < (int)sizeof all - 2; s++) all[k++] = *s; }
            all[k] = 0; val = all; p++;
        } else if (is_name_start(p[1])) {
            const char *e = p + 1; while (is_name_char(*e)) e++;
            val = var_get(p + 1, (int)(e - (p + 1)));
            p = e - 1;
        } else if (p[1] == '?' || p[1] == '#' || (p[1] >= '0' && p[1] <= '9')) {
            val = var_get(p + 1, 1); p++;
        } else { EMIT('$'); continue; }

        if (q) { for (const char *s = val; *s; s++) EMIT(*s); in_word = 1; }
        else {
            for (const char *s = val; *s; s++) {
                if (*s == ' ' || *s == '\t' || *s == '\n') FLUSH(); else EMIT(*s);
            }
        }
    }
    FLUSH();
#undef EMIT
#undef FLUSH
    return 0;
}

/* ---------------- test / [ ---------------- */
static int num(const char *s) { return m_atoi(s); }

static int do_test(int argc, char **argv)          // returns 0 true / 1 false / 2 error
{
    if (argc && streq(argv[argc - 1], "]")) argc--;
    int neg = 0;
    if (argc && streq(argv[0], "!")) { neg = 1; argv++; argc--; }
    int r;
    if (argc == 0) r = 1;
    else if (argc == 1) r = slen(argv[0]) ? 0 : 1;
    else if (argc == 2) {
        const char *o = argv[0], *a = argv[1];
        if (streq(o, "-z")) r = slen(a) ? 1 : 0;
        else if (streq(o, "-n")) r = slen(a) ? 0 : 1;
        else if (streq(o, "-e")) r = sys_stat(a) > 0 ? 0 : 1;
        else if (streq(o, "-f")) r = sys_stat(a) == 1 ? 0 : 1;
        else if (streq(o, "-d")) r = sys_stat(a) == 2 ? 0 : 1;
        else { err2("test: unknown operator ", o); return 2; }
    } else if (argc == 3) {
        const char *a = argv[0], *o = argv[1], *b = argv[2];
        if (streq(o, "=") || streq(o, "==")) r = streq(a, b) ? 0 : 1;
        else if (streq(o, "!=")) r = streq(a, b) ? 1 : 0;
        else if (streq(o, "-eq")) r = num(a) == num(b) ? 0 : 1;
        else if (streq(o, "-ne")) r = num(a) != num(b) ? 0 : 1;
        else if (streq(o, "-lt")) r = num(a) < num(b) ? 0 : 1;
        else if (streq(o, "-le")) r = num(a) <= num(b) ? 0 : 1;
        else if (streq(o, "-gt")) r = num(a) > num(b) ? 0 : 1;
        else if (streq(o, "-ge")) r = num(a) >= num(b) ? 0 : 1;
        else { err2("test: unknown operator ", o); return 2; }
    } else { err("test: too many arguments"); return 2; }
    return neg ? !r : r;
}

/* ---------------- executor ---------------- */
typedef struct {
    const char *w[MAXW]; int nw;                 // raw words
    const char *in_f, *out_f, *err_f; int out_app, err_to_out;
} stage_t;

static tok_t *cur(void) { return C->pos < C->ntk ? &C->tk[C->pos] : 0; }
static int is_op(const char *s) { tok_t *t = cur(); return t && t->t == T_OP && streq(t->s, s); }
static int kw(const char *s) { tok_t *t = cur(); return t && t->t == T_WORD && streq(t->s, s); }
static int is_stop(const char *s)
{
    return streq(s, "then") || streq(s, "elif") || streq(s, "else") || streq(s, "fi") || streq(s, "do") || streq(s, "done");
}
static void skip_seps(void)
{
    while (C->pos < C->ntk && (C->tk[C->pos].t == T_NL || (C->tk[C->pos].t == T_OP && streq(C->tk[C->pos].s, ";")))) C->pos++;
}
static void skip_nl(void) { while (C->pos < C->ntk && C->tk[C->pos].t == T_NL) C->pos++; }

static int parse_list(int ex);

static int read_line_fd(int fd, char *buf, int n)       // returns length, -1 on EOF with nothing read
{
    int l = 0; char ch;
    while (l < n - 1) {
        int r = sys_read(fd, &ch, 1);
        if (r <= 0) { if (l == 0) return -1; break; }
        if (ch == '\n') break;
        buf[l++] = ch;
    }
    buf[l] = 0;
    return l;
}

static int is_assign(const char *w)
{
    if (!is_name_start(*w)) return 0;
    const char *p = w;
    while (is_name_char(*p)) p++;
    return *p == '=';
}

// Run one expanded simple command. Returns the exit status.
static int run_simple(char **argv, int argc, int fin, int fout, int ferr)
{
    if (argc == 0) return 0;
    const char *c0 = argv[0];
    if (streq(c0, "cd")) {
        if (sys_chdir(argc > 1 ? argv[1] : "~") < 0) { err2("cd: no such directory: ", argc > 1 ? argv[1] : "~"); return 1; }
        return 0;
    }
    if (streq(c0, "exit")) { exit_code = argc > 1 ? num(argv[1]) : status; ctl = CTL_EXIT; return exit_code; }
    if (streq(c0, "true") || streq(c0, ":")) return 0;
    if (streq(c0, "false")) return 1;
    if (streq(c0, "break")) { ctl = CTL_BREAK; return 0; }
    if (streq(c0, "continue")) { ctl = CTL_CONT; return 0; }
    if (streq(c0, "export") || streq(c0, "local") || streq(c0, "readonly")) {
        for (int i = 1; i < argc; i++) {
            const char *eq = argv[i]; while (*eq && *eq != '=') eq++;
            if (*eq == '=') var_set(argv[i], (int)(eq - argv[i]), eq + 1);
        }
        return 0;
    }
    if (streq(c0, "unset")) { for (int i = 1; i < argc; i++) var_unset(argv[i]); return 0; }
    if (streq(c0, "shift")) {
        int n = argc > 1 ? num(argv[1]) : 1;
        if (n > pargc) return 1;
        for (int i = 1; i + n <= pargc; i++) pargv[i] = pargv[i + n];
        pargc -= n;
        return 0;
    }
    if (streq(c0, "read")) {
        char line[VALLEN];
        int l = read_line_fd(fin, line, sizeof line);
        if (l < 0) { if (argc > 1) var_set(argv[1], (int)slen(argv[1]), ""); return 1; }
        if (argc > 1) var_set(argv[1], (int)slen(argv[1]), line);
        return 0;
    }
    if (streq(c0, "test") || streq(c0, "[")) return do_test(argc - 1, argv + 1);
    if (streq(c0, ".") || streq(c0, "source")) {
        if (argc < 2) { err("source: file name required"); return 2; }
        int fd = sys_open(argv[1], 0);
        if (fd < 0) { err2("source: cannot open ", argv[1]); return 1; }
        static char sbuf[1400];
        int got = 0, r;
        while (got < (int)sizeof sbuf - 1 && (r = sys_read(fd, sbuf + got, (int)sizeof sbuf - 1 - got)) > 0) got += r;
        sys_close(fd);
        sbuf[got] = 0;
        return run_text(sbuf, fout);
    }

    // external: build the NUL-separated blob for sys_run
    char blob[512]; int bl = 0;
    for (int i = 0; i < argc; i++) {
        int l = (int)slen(argv[i]) + 1;
        if (bl + l > (int)sizeof blob) { err("command line too long"); return 126; }
        for (int k = 0; k < l; k++) blob[bl + k] = argv[i][k];
        bl += l;
    }
    int rc = sys_run(blob, bl, argc, fin, fout, ferr);
    if (rc == 130 || sys_sigint(0)) { ctl = CTL_EXIT; exit_code = 130; return 130; }   // Ctrl-C aborts the script
    return rc;
}

// Execute a pipeline of simple commands that has been collected into st[0..ns-1].
static int exec_pipeline(stage_t *st, int ns)
{
    int rc = 0, prev = -1;                        // prev: membuf holding the previous stage's output
    for (int i = 0; i < ns; i++) {
        stage_t *s = &st[i];
        // expand words
        C->xn = 0;
        wl_t wl; wl.n = 0;
        for (int k = 0; k < s->nw; k++) if (expand(C, s->w[k], &wl)) { err("expansion failed"); rc = 1; goto done; }
        // variable assignment only: NAME=value
        if (ns == 1 && wl.n >= 1 && is_assign(s->w[0]) && !(s->w[0][0] == '"')) {
            int allassign = 1;
            for (int k = 0; k < s->nw; k++) if (!is_assign(s->w[k])) allassign = 0;
            if (allassign) {
                for (int k = 0; k < wl.n; k++) {
                    const char *eq = wl.w[k]; while (*eq && *eq != '=') eq++;
                    var_set(wl.w[k], (int)(eq - wl.w[k]), eq + 1);
                }
                rc = 0; goto done;
            }
        }
        wl.w[wl.n] = 0;
        int fin = 0, fout = C->out_fd, ferr = 2, f_in = -1, f_out = -1, f_err = -1, pout = -1;
        if (prev >= 0) fin = prev;
        if (s->in_f) {
            wl_t t; t.n = 0; expand(C, s->in_f, &t);
            if (t.n != 1 || (f_in = sys_open(t.w[0], 0)) < 0) { err2("cannot open ", t.n ? t.w[0] : s->in_f); rc = 1; goto done; }
            fin = f_in;
        }
        if (i < ns - 1) { pout = sys_membuf(); if (pout < 0) { err("out of memory"); rc = 1; goto done; } fout = pout; }
        if (s->out_f) {
            wl_t t; t.n = 0; expand(C, s->out_f, &t);
            if (t.n != 1 || (f_out = sys_open(t.w[0], s->out_app ? 2 : 1)) < 0) { err2("cannot write ", t.n ? t.w[0] : s->out_f); rc = 1; goto done; }
            fout = f_out;
        }
        if (s->err_to_out) ferr = fout;
        if (s->err_f) {
            wl_t t; t.n = 0; expand(C, s->err_f, &t);
            if (t.n == 1 && (f_err = sys_open(t.w[0], 1)) >= 0) ferr = f_err;
        }
        rc = run_simple(wl.w, wl.n, fin, fout, ferr);
        if (f_in >= 0) sys_close(f_in);
        if (f_out >= 0) sys_close(f_out);
        if (f_err >= 0) sys_close(f_err);
        if (prev >= 0) { sys_close(prev); prev = -1; }
        if (pout >= 0) { sys_rewind(pout); prev = pout; }
        if (ctl == CTL_EXIT) break;
    }
done:
    if (prev >= 0) sys_close(prev);
    return rc;
}

// Parse (and, if ex, run) one pipeline or compound command.
static int parse_pipeline(int ex);

static int expect_kw(const char *k)
{
    if (!kw(k)) { err2("syntax error: expected ", k); ctl = CTL_EXIT; exit_code = 2; return -1; }
    C->pos++;
    return 0;
}

static int do_if(int ex)
{
    C->pos++;                                      // if
    int cond = parse_list(ex);
    if (expect_kw("then")) return 2;
    int taken = ex && cond == 0 && !ctl;
    int st = parse_list(taken);
    while (kw("elif")) {
        C->pos++;
        int c2 = parse_list(ex && !taken && !ctl);
        if (expect_kw("then")) return 2;
        int run = ex && !taken && c2 == 0 && !ctl;
        int s2 = parse_list(run);
        if (run) { taken = 1; st = s2; }
    }
    if (kw("else")) {
        C->pos++;
        int run = ex && !taken && !ctl;
        int s3 = parse_list(run);
        if (run) { taken = 1; st = s3; }
    }
    if (expect_kw("fi")) return 2;
    return taken ? st : 0;
}

static int do_while(int ex)
{
    C->pos++;                                      // while
    int start = C->pos, st = 0;
    for (;;) {
        C->pos = start;
        int cond = parse_list(ex);
        if (expect_kw("do")) return 2;
        int run = ex && cond == 0 && !ctl;
        int s = parse_list(run);
        if (run) st = s;
        if (expect_kw("done")) return 2;
        if (!run) break;
        if (ctl == CTL_BREAK) { ctl = CTL_NONE; break; }
        if (ctl == CTL_CONT) ctl = CTL_NONE;
        if (ctl == CTL_EXIT) break;
    }
    return st;
}

static int do_for(int ex)
{
    C->pos++;                                      // for
    tok_t *v = cur();
    if (!v || v->t != T_WORD) { err("syntax error: for needs a variable name"); ctl = CTL_EXIT; exit_code = 2; return 2; }
    const char *name = v->s;
    C->pos++;
    // collect the list
    char *items[MAXW]; int ni = 0;
    char store[400]; int sn = 0;                   // items are copied here: the expansion arena is reused by every command
    if (kw("in")) {
        C->pos++;
        wl_t wl; wl.n = 0;
        C->xn = 0;
        while (cur() && cur()->t == T_WORD && !streq(cur()->s, "do")) {
            if (ex && expand(C, cur()->s, &wl)) { err("expansion failed"); ctl = CTL_EXIT; exit_code = 1; return 1; }
            C->pos++;
        }
        for (int i = 0; i < wl.n && ni < MAXW; i++) {
            int l = (int)slen(wl.w[i]) + 1;
            if (sn + l > (int)sizeof store) break;
            for (int k = 0; k < l; k++) store[sn + k] = wl.w[i][k];
            items[ni++] = store + sn; sn += l;
        }
    } else {
        for (int i = 1; i <= pargc && ni < MAXW; i++) items[ni++] = (char *)pargv[i];
    }
    skip_seps();
    if (expect_kw("do")) return 2;
    int bodystart = C->pos, st = 0;
    if (!ex || ni == 0) { parse_list(0); if (expect_kw("done")) return 2; return 0; }
    for (int i = 0; i < ni; i++) {
        var_set(name, (int)slen(name), items[i]);
        C->pos = bodystart;
        st = parse_list(1);
        if (expect_kw("done")) return 2;
        if (ctl == CTL_BREAK) { ctl = CTL_NONE; break; }
        if (ctl == CTL_CONT) ctl = CTL_NONE;
        if (ctl == CTL_EXIT) break;
    }
    return st;
}

static int parse_pipeline(int ex)
{
    if (kw("if")) return do_if(ex);
    if (kw("while") || kw("until")) return do_while(ex);
    if (kw("for")) return do_for(ex);

    stage_t st[MAXSTG];
    int ns = 0;
    for (;;) {
        if (ns >= MAXSTG) { err("pipeline too long"); ctl = CTL_EXIT; exit_code = 2; return 2; }
        stage_t *s = &st[ns++];
        s->nw = 0; s->in_f = s->out_f = s->err_f = 0; s->out_app = s->err_to_out = 0;
        while (cur()) {
            tok_t *t = cur();
            if (t->t == T_NL) break;
            if (t->t == T_OP) {
                const char *o = t->s;
                if (streq(o, "|") || streq(o, ";") || streq(o, "&&") || streq(o, "||")) break;
                if (streq(o, "2>&1")) { s->err_to_out = 1; C->pos++; continue; }
                C->pos++;
                if (!cur() || cur()->t != T_WORD) { err2("syntax error after ", o); ctl = CTL_EXIT; exit_code = 2; return 2; }
                const char *f = cur()->s; C->pos++;
                if (streq(o, "<")) s->in_f = f;
                else if (streq(o, ">")) { s->out_f = f; s->out_app = 0; }
                else if (streq(o, ">>")) { s->out_f = f; s->out_app = 1; }
                else if (streq(o, "2>")) s->err_f = f;
                continue;
            }
            if (t->t == T_WORD && s->nw == 0 && is_stop(t->s)) break;
            if (s->nw >= MAXW - 1) { err("too many words"); ctl = CTL_EXIT; exit_code = 2; return 2; }
            s->w[s->nw++] = t->s;
            C->pos++;
        }
        if (is_op("|")) { C->pos++; skip_nl(); continue; }
        break;
    }
    if (ns == 1 && st[0].nw == 0 && !st[0].in_f && !st[0].out_f) return status;   // empty
    if (!ex) return 0;
    int rc = exec_pipeline(st, ns);
    return rc;
}

static int parse_andor(int ex)
{
    int st = parse_pipeline(ex);
    if (ex) status = st;
    while (is_op("&&") || is_op("||")) {
        int isand = is_op("&&");
        C->pos++; skip_nl();
        int run = ex && !ctl && (isand ? st == 0 : st != 0);
        int s2 = parse_pipeline(run);
        if (run) { st = s2; status = st; }
    }
    return st;
}

static int parse_list(int ex)
{
    int st = status;
    for (;;) {
        skip_seps();
        tok_t *t = cur();
        if (!t) break;
        if (t->t == T_WORD && is_stop(t->s)) break;
        int before = C->pos;
        st = parse_andor(ex);
        if (ctl) ex = 0;
        if (C->pos == before) C->pos++;                 // never loop on an unparsable token
    }
    return st;
}

// Lex and run a piece of script text in a nested context (used by $(...), source). Output goes to out_fd.
static int run_text(const char *text, int out_fd)
{
    if (depth >= 2) { err("nesting too deep"); return 1; }
    ctx_t *saved = C;
    depth++;
    C = &ctxs[depth];
    C->out_fd = out_fd;
    C->pos = 0;
    int rc = 1;
    int lr = lex(C, text);
    if (lr == 0) {
        int sc = ctl; ctl = CTL_NONE;
        rc = parse_list(1);
        if (ctl == CTL_EXIT) { /* exit inside $() only ends the substitution */ ctl = sc; }
        else ctl = sc;
    } else err(lr == -1 ? "input too large" : "unterminated quote");
    depth--;
    C = saved;
    status = rc;
    return rc;
}

/* ---------------- top level ---------------- */
static int run_top(const char *text)               // run text in the top-level context
{
    C = &ctxs[0]; depth = 0;
    C->out_fd = 1; C->pos = 0;
    int lr = lex(C, text);
    if (lr != 0) { err(lr == -1 ? "input too large" : "unterminated quote"); return 2; }
    ctl = CTL_NONE;
    int rc = parse_list(1);
    if (ctl == CTL_EXIT) return exit_code;
    return rc;
}

// Is the accumulated interactive input complete (no open if/for/while, quote or trailing operator)?
static int complete(const char *text)
{
    C = &ctxs[0];
    int lr = lex(C, text);
    if (lr == -2) return 0;
    if (lr == -1) return 1;
    int d = 0, at_start = 1;
    for (int i = 0; i < C->ntk; i++) {
        tok_t *t = &C->tk[i];
        if (t->t == T_WORD) {
            if (at_start) {
                if (streq(t->s, "if") || streq(t->s, "for") || streq(t->s, "while") || streq(t->s, "until")) d++;
                else if (streq(t->s, "fi") || streq(t->s, "done")) d--;
            }
            at_start = 0;
        } else at_start = (t->t == T_NL) || (t->t == T_OP && (streq(t->s, ";") || streq(t->s, "&&") || streq(t->s, "||") || streq(t->s, "|")));
    }
    if (d > 0) return 0;
    if (C->ntk) { tok_t *l = &C->tk[C->ntk - 1]; if (l->t == T_OP && (streq(l->s, "|") || streq(l->s, "&&") || streq(l->s, "||"))) return 0; }
    return 1;
}

static char script[8000];

static int read_all(int fd, char *buf, int cap)
{
    int got = 0, r;
    while (got < cap - 1 && (r = sys_read(fd, buf + got, cap - 1 - got)) > 0) got += r;
    buf[got] = 0;
    return got;
}

int main(int argc, char **argv)
{
    pargv[0] = "sh";
    if (argc >= 3 && streq(argv[1], "-c")) {                      // sh -c 'cmd'
        int l = 0; const char *s = argv[2];
        while (*s && l < (int)sizeof script - 1) script[l++] = *s++;
        script[l] = 0;
        pargv[0] = argv[0];
        for (int i = 3; i < argc && i < 12; i++) pargv[i - 2] = argv[i];
        pargc = argc > 3 ? (argc - 3 < 9 ? argc - 3 : 9) : 0;
        return run_top(script);
    }
    if (argc >= 2) {                                              // sh script [args]
        int fd = sys_open(argv[1], 0);
        if (fd < 0) { err2("cannot open ", argv[1]); return 127; }
        read_all(fd, script, sizeof script);
        sys_close(fd);
        pargv[0] = argv[1];
        pargc = 0;
        for (int i = 2; i < argc && pargc < 9; i++) pargv[++pargc] = argv[i];
        return run_top(script);
    }
    if (!sys_isatty(0)) {                                         // script on stdin
        read_all(0, script, sizeof script);
        return run_top(script);
    }

    // interactive
    sys_sigint(1);                                                // Ctrl-C cancels the line/command, it does not kill the shell
    m_puts("sh: type 'exit' to leave\n");
    int rc_fd = sys_open("/esp/.shrc", 0);                        // ~/.shrc: run once, variables stay
    if (rc_fd >= 0) { read_all(rc_fd, script, sizeof script); sys_close(rc_fd); run_top(script); ctl = CTL_NONE; }
    for (;;) {
        int len = 0, more = 0;
        script[0] = 0;
        for (;;) {
            m_puts(more ? "> " : "$ ");
            int r = sys_read(0, script + len, (int)sizeof script - 1 - len);
            if (r < 0 || sys_sigint(0)) { len = 0; script[0] = 0; more = 0; if (r < 0) continue; break; }   // terminal already echoed ^C
            if (r == 0) { m_puts("\n"); return exit_code; }       // Ctrl-D
            len += r; script[len] = 0;
            if (len >= (int)sizeof script - 2) { err("line too long"); script[0] = 0; len = 0; break; }
            if (complete(script)) break;
            more = 1;
        }
        if (!script[0]) continue;
        status = run_top(script);
        if (ctl == CTL_EXIT && exit_code != 130) return exit_code;
        ctl = CTL_NONE;
    }
}
