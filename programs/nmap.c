// nmap - a small TCP connect scanner (not the real nmap: no raw packets, no OS detection).
//   nmap [-p PORTS] [-F] [-T MS] [-sn] [-v] TARGET...
//   TARGET : host name, IPv4 address, 192.168.0.1-50 (last octet range) or 192.168.0.0/24 (CIDR, /22 or smaller)
//   -p     : ports, e.g. 22,80,8000-8100   (default: about 100 common ones)    -F  fast: the 20 most common
//   -T MS  : connect timeout in milliseconds (default 300; a closed port answers at once, a filtered one waits this long)
//   -sn    : only find hosts that are up (probes 22, 80, 443, 445)                  -v  also list hosts that are down
// "open" = connection accepted, "closed" = refused (the host answered), "filtered" = no answer within the timeout.
// Ctrl-C stops the scan. Only scan machines you own or may test.
#include "mini.h"

static const unsigned short COMMON[] = {
    80, 443, 22, 21, 25, 53, 110, 139, 445, 8080, 23, 3389, 143, 993, 995, 3306, 1883, 5900, 8443, 8000,
    111, 135, 199, 554, 587, 1723, 1025, 8888, 5432, 6379, 27017, 9100, 631, 123, 161, 1433, 2049, 5060, 5000, 49152,
    81, 88, 113, 179, 389, 443, 465, 515, 548, 623, 873, 1080, 1194, 1900, 2000, 2121, 3000, 3128, 3478, 4443, 5001,
    5357, 5555, 5601, 5672, 5800, 5985, 6000, 6001, 6667, 7000, 7070, 7547, 8008, 8009, 8081, 8082, 8090, 8181, 8883,
    9000, 9001, 9090, 9200, 9443, 10000, 11211, 15672, 32768, 50000, 49153, 8554, 8291, 37777, 5353, 2375, 1521, 4000, 6443 };
#define NCOMMON ((int)(sizeof COMMON / sizeof COMMON[0]))

typedef struct { const char *name; unsigned short port; } svc_t;
static const svc_t SVC[] = { {"ftp",21},{"ssh",22},{"telnet",23},{"smtp",25},{"dns",53},{"http",80},{"pop3",110},{"ntp",123},{"netbios",139},{"imap",143},
    {"snmp",161},{"https",443},{"smb",445},{"imaps",993},{"pop3s",995},{"mqtt",1883},{"mysql",3306},{"rdp",3389},{"postgres",5432},{"vnc",5900},
    {"redis",6379},{"http-alt",8080},{"https-alt",8443},{"mqtt-tls",8883},{"mongodb",27017},{"rtsp",554},{"ipp",631},{"upnp",1900},{"mdns",5353},{"docker",2375} };
static const char *svc_name(int p) { for (unsigned i = 0; i < sizeof SVC / sizeof SVC[0]; i++) if (SVC[i].port == p) return SVC[i].name; return "unknown"; }

static int pstart[64], pend[64], nranges;                  // the ports to scan, as inclusive ranges
static int timeout_ms = 300, ping_only, verbose;

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_ports(const char *s)
{
    nranges = 0;
    while (*s) {
        if (!is_digit(*s) || nranges >= 64) return -1;
        int a = 0, b;
        while (is_digit(*s)) a = a * 10 + (*s++ - '0');
        b = a;
        if (*s == '-') { s++; if (!is_digit(*s)) return -1; b = 0; while (is_digit(*s)) b = b * 10 + (*s++ - '0'); }
        if (a < 1 || b > 65535 || b < a) return -1;
        pstart[nranges] = a; pend[nranges++] = b;
        if (*s == ',') s++; else if (*s) return -1;
    }
    return nranges ? 0 : -1;
}

// connect to ip:port: 1 open, 0 closed (refused quickly), -1 no answer
static int probe(const char *ip, int port)
{
    unsigned t0 = sys_millis();
    int fd = sys_tcp_connect(ip, port, timeout_ms);
    if (fd >= 0) { sys_close(fd); return 1; }
    return (int)(sys_millis() - t0) < timeout_ms * 3 / 4 ? 0 : -1;
}

static const int PING_PORTS[] = { 80, 443, 22, 445 };
static int host_up(const char *ip, int *ms)                // any answer (open or refused) means the host is up
{
    unsigned t0 = sys_millis();
    for (int i = 0; i < 4; i++) { int r = probe(ip, PING_PORTS[i]); if (r >= 0) { *ms = (int)(sys_millis() - t0); return 1; } }
    return 0;
}

static void scan_host(const char *ip, const char *name, int single)
{
    int up = 0, ms = 0, open = 0, closed = 0, filtered = 0, total = 0;
    if (ping_only || !single) { up = host_up(ip, &ms); if (!up && !single && !verbose) return; }
    if (ping_only) {
        if (up) m_printf("Host %s%s%s%s is up (%d ms)\n", ip, name ? " (" : "", name ? name : "", name ? ")" : "", ms);
        else if (verbose || single) m_printf("Host %s is down (no answer on 22, 80, 443, 445)\n", ip);
        return;
    }
    if (!up && !single) { if (verbose) m_printf("Host %s is down\n", ip); return; }
    m_printf("\nScan report for %s%s%s%s\n", name ? name : ip, name ? " (" : "", name ? ip : "", name ? ")" : "");
    unsigned t0 = sys_millis();
    int printed_hdr = 0;
    int ranges = nranges;
    for (int r = 0; r < (ranges ? ranges : 1); r++) {
        int a = ranges ? pstart[r] : 0, b = ranges ? pend[r] : NCOMMON - 1;
        for (int p = a; p <= b; p++) {
            int port = ranges ? p : COMMON[p];
            total++;
            int s = probe(ip, port);
            if (s == 1) {
                if (!printed_hdr) { m_puts("PORT      STATE  SERVICE\n"); printed_hdr = 1; }
                char pp[12]; m_snprintf(pp, sizeof pp, "%d/tcp", port);
                m_printf("%s", pp); for (int k = (int)m_strlen(pp); k < 10; k++) m_puts(" ");
                m_printf("open   %s\n", svc_name(port));
                open++;
            } else if (s == 0) closed++; else filtered++;
        }
    }
    if (!open && closed + filtered == total && total) m_printf("All %d scanned ports are %s.\n", total, filtered == total ? "filtered (no answer: host down or firewalled)" : "closed");
    else if (closed || filtered) m_printf("Not shown: %d closed, %d filtered\n", closed, filtered);
    m_printf("Done: %d ports in %d.%d s\n", total, (int)(sys_millis() - t0) / 1000, (int)(sys_millis() - t0) % 1000 / 100);
}

static int parse_ip(const char *s, int *o)                 // dotted quad -> 4 octets, or 0
{
    for (int i = 0; i < 4; i++) {
        if (!is_digit(*s)) return 0;
        int v = 0; while (is_digit(*s)) { v = v * 10 + (*s++ - '0'); if (v > 255) return 0; }
        o[i] = v;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    return 1;
}

static void scan_target(const char *t)
{
    int o[4]; char ip[20];
    const char *slash = t; while (*slash && *slash != '/') slash++;
    const char *dash = t; while (*dash && *dash != '-') dash++;
    if (*slash == '/' || *dash == '-') {                    // CIDR or last-octet range
        char base[24]; int n = 0; const char *e = *slash == '/' ? slash : dash;
        while (t + n < e && n < 23) { base[n] = t[n]; n++; }
        base[n] = 0;
        int lo, hi;
        if (*slash == '/') {
            int bits = m_atoi(slash + 1);
            if (!parse_ip(base, o) || bits < 22 || bits > 32) { m_eprintf("nmap: bad target %s (CIDR /22 .. /32)\n", t); return; }
            unsigned a = ((unsigned)o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3], mask = bits == 32 ? 0xffffffffu : ~((1u << (32 - bits)) - 1);
            unsigned first = a & mask, last = first | ~mask;
            if (bits <= 30) { first++; last--; }
            for (unsigned x = first; x <= last; x++) {
                m_snprintf(ip, sizeof ip, "%d.%d.%d.%d", (int)(x >> 24), (int)((x >> 16) & 255), (int)((x >> 8) & 255), (int)(x & 255));
                scan_host(ip, 0, first == last);
            }
            return;
        }
        if (!parse_ip(base, o) || !is_digit(dash[1])) { m_eprintf("nmap: bad target %s\n", t); return; }
        lo = o[3]; hi = m_atoi(dash + 1);
        if (hi > 255 || hi < lo) { m_eprintf("nmap: bad range %s\n", t); return; }
        for (int x = lo; x <= hi; x++) { m_snprintf(ip, sizeof ip, "%d.%d.%d.%d", o[0], o[1], o[2], x); scan_host(ip, 0, lo == hi); }
        return;
    }
    if (parse_ip(t, o)) { scan_host(t, 0, 1); return; }
    int l = sys_dns(t, ip, sizeof ip);                      // a host name
    if (l <= 0) { m_eprintf("nmap: cannot resolve %s\n", t); return; }
    scan_host(ip, t, 1);
}

int main(int argc, char **argv)
{
    int fast = 0, ntargets = 0; const char *targets[16]; const char *ports = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!m_strcmp(a, "-p") && i + 1 < argc) ports = argv[++i];
        else if (a[0] == '-' && a[1] == 'p' && a[2]) ports = a + 2;
        else if (!m_strcmp(a, "-T") && i + 1 < argc) { timeout_ms = m_atoi(argv[++i]); if (timeout_ms < 50) timeout_ms = 50; if (timeout_ms > 10000) timeout_ms = 10000; }
        else if (!m_strcmp(a, "-F")) fast = 1;
        else if (!m_strcmp(a, "-sn")) ping_only = 1;
        else if (!m_strcmp(a, "-v")) verbose = 1;
        else if (a[0] == '-' && a[1]) { m_eprintf("nmap: unknown option %s\n", a); return 2; }
        else if (ntargets < 16) targets[ntargets++] = a;
    }
    if (!ntargets) { m_eputs("usage: nmap [-p PORTS] [-F] [-T MS] [-sn] [-v] TARGET...   (TARGET: host, 1.2.3.4, 1.2.3.1-50, 1.2.3.0/24)\n"); return 2; }
    if (ports) { if (parse_ports(ports)) { m_eprintf("nmap: bad port list '%s'\n", ports); return 2; } }
    if (fast && !ports) {                                   // first 20 common ports as separate ranges
        for (int i = 0; i < 20; i++) { pstart[i] = pend[i] = COMMON[i]; }
        nranges = 20;
    }
    for (int i = 0; i < ntargets; i++) scan_target(targets[i]);
    return 0;
}
