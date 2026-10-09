// console - extra data for the web console (/cgi-bin/console): firmware, clock, services, installed packages, programs.
// Output is one JSON object. Runs the built-in `service list` through sys_run (built-ins need no second program memory).
#include "mini.h"

static void jstr(const char *s)                                   // JSON string with escaping
{
    m_puts("\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { char e[3] = { '\\', *s, 0 }; m_puts(e); }
        else if ((unsigned char)*s < 32) m_puts(" ");
        else { char c[2] = { *s, 0 }; m_puts(c); }
    }
    m_puts("\"");
}

static int words(char *line, char **w, int max)                   // split on spaces in place
{
    int n = 0;
    while (*line && n < max) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        w[n++] = line;
        while (*line && *line != ' ') line++;
    }
    return n;
}

static char out[2048];

int main(int argc, char **argv)
{
    m_puts("Content-Type: application/json\n\n{");
    char ver[48] = "";
    sys_version(ver, sizeof ver);
    m_puts("\"version\":"); jstr(ver);
    m_printf(",\"abi\":%d,\"time_state\":%d,\"epoch\":%u", sys_abi(), sys_time_state(), sys_time());

    // services: `service list`
    int fd = sys_membuf(), n = 0;
    if (fd >= 0) {
        const char blob[] = "service\0list";
        sys_run(blob, sizeof blob, 2, 0, fd, 2);
        sys_rewind(fd);
        int r;
        while (n < (int)sizeof out - 1 && (r = sys_read(fd, out + n, (int)sizeof out - 1 - n)) > 0) n += r;
        sys_close(fd);
    }
    out[n] = 0;
    m_puts(",\"services\":[");
    int first = 1, ln = 0;
    for (char *p = out; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        int more = *e; *e = 0;
        if (ln++ > 0 && *p) {
            char *w[16]; int nw = words(p, w, 16);
            int i = 1; char type[24];
            if (nw >= 3 && !m_strcmp(w[1], "every")) { m_snprintf(type, sizeof type, "every %s", w[2]); i = 3; }
            else { m_snprintf(type, sizeof type, "%s", nw > 1 ? w[1] : ""); i = 2; }
            if (nw >= i + 4) {
                if (!first) m_puts(",");
                first = 0;
                m_puts("{\"name\":"); jstr(w[0]);
                m_puts(",\"type\":"); jstr(type);
                m_puts(",\"state\":"); jstr(w[i]);
                m_puts(",\"boot\":"); jstr(w[i + 1]);
                m_puts(",\"runs\":"); jstr(w[i + 2]);
                m_puts(",\"exit\":"); jstr(w[i + 3]);
                m_puts(",\"desc\":\"");
                for (int k = i + 4; k < nw; k++) { if (k > i + 4) m_puts(" "); const char *s = w[k]; for (; *s; s++) { if (*s == '"' || *s == '\\') m_puts("\\"); char c[2] = { *s, 0 }; m_puts(c); } }
                m_puts("\"}");
            }
        }
        if (!more) break;
        p = e + 1;
    }
    m_puts("]");

    // packages recorded by pkg
    char inst[600]; int il = sys_kv_get("pkg.inst", inst, sizeof inst - 1);
    if (il < 0) il = 0;
    inst[il] = 0;
    m_puts(",\"packages\":[");
    first = 1;
    for (char *p = inst; *p; ) {
        while (*p == ' ') p++;
        char *q = p; while (*q && *q != ' ') q++;
        int more = *q; *q = 0;
        if (q > p) { if (!first) m_puts(","); first = 0; jstr(p); }
        if (!more) break;
        p = q + 1;
    }
    m_puts("]");

    // programs installed by the user (~/.local/bin) and system programs (/bin): names only
    static char dir[1024];
    for (int k = 0; k < 2; k++) {
        int l = sys_listdir(k ? "/bin" : "/esp/.local/bin", dir, sizeof dir);
        m_puts(k ? ",\"system_programs\":[" : ",\"programs\":[");
        first = 1;
        for (int i = 0; l > 0 && i < l; ) {
            int j = i; while (j < l && dir[j] != '\n') j++;
            dir[j] = 0;
            int nl = j - i;
            if (nl > 4 && !m_strcmp(dir + j - 4, ".aot")) { dir[j - 4] = 0; if (!first) m_puts(","); first = 0; jstr(dir + i); }
            i = j + 1;
        }
        m_puts("]");
    }
    m_puts("}\n");
    return 0;
}
