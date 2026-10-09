// curl [-o FILE] URL: HTTP/HTTPS GET (sys_http_get, ABI 4). Without -o the body goes to stdout.
// Certificates are checked against the built-in CA bundle, so the clock must be set (NTP).
#include "mini.h"

int main(int argc, char **argv)
{
    const char *url = 0, *out = 0;
    for (int i = 1; i < argc; i++) {
        if (!m_strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) { m_eprintf("curl: unknown option %s\n", argv[i]); return 2; }
        else url = argv[i];
    }
    if (!url) { m_eputs("usage: curl [-o FILE] URL\n"); return 2; }
    const char *dst = out ? out : "/tmp/curl.tmp";
    int r = sys_http_get(url, dst, 1000000, 15000);
    if (r < 0) {
        if (r == -1) m_eputs("curl: connection or TLS error (clock set? URL right?)\n");
        else if (r == -2) m_eputs("curl: cannot write the file\n");
        else if (r == -3) m_eputs("curl: response too large\n");
        else if (r == -4) m_eputs("curl: interrupted\n");
        else m_eprintf("curl: HTTP error %d\n", -r);
        return 1;
    }
    if (!out) {
        int fd = sys_open(dst, 0);
        if (fd >= 0) {
            char buf[256]; int n;
            while ((n = sys_read(fd, buf, sizeof buf)) > 0) m_write(1, buf, (size_t)n);
            sys_close(fd);
        }
        sys_unlink(dst);
    } else m_eprintf("saved %d bytes to %s\n", r, out);
    return 0;
}
