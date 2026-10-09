// ping [-c COUNT] [-i SECONDS] [-W MS] [-s SIZE] HOST: ICMP echo (sys_ping, firmware ABI 5).
//   -c 0 pings until Ctrl-C (default 4)   -i interval (default 1)   -W wait per reply in ms (default 1000)   -s payload bytes (default 56)
// Ctrl-C stops and prints the statistics. Exit status 0 if at least one reply arrived, else 1.
#include "mini.h"

static void ms(int us, char *out, int n) { m_snprintf(out, n, "%d.%02d", us / 1000, us % 1000 / 10); }

int main(int argc, char **argv)
{
    int count = 4, interval = 1, wait = 1000, size = 56; const char *host = 0;
    for (int i = 1; i < argc; i++) {
        if (!m_strcmp(argv[i], "-c") && i + 1 < argc) count = m_atoi(argv[++i]);
        else if (!m_strcmp(argv[i], "-i") && i + 1 < argc) interval = m_atoi(argv[++i]);
        else if (!m_strcmp(argv[i], "-W") && i + 1 < argc) wait = m_atoi(argv[++i]);
        else if (!m_strcmp(argv[i], "-s") && i + 1 < argc) size = m_atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { m_eprintf("ping: unknown option %s\n", argv[i]); return 2; }
        else host = argv[i];
    }
    if (!host) { m_eputs("usage: ping [-c COUNT] [-i SECONDS] [-W MS] [-s SIZE] HOST\n"); return 2; }
    if (interval < 1) interval = 1;
    if (size < 0) size = 0;
    if (size > 400) size = 400;
    char ip[20];
    if (sys_dns(host, ip, sizeof ip) <= 0) { m_eprintf("ping: cannot resolve %s\n", host); return 2; }

    sys_sigint(1);                                              // Ctrl-C ends the loop and prints the summary
    m_printf("PING %s (%s): %d data bytes\n", host, ip, size);
    int sent = 0, got = 0, min = 0, max = 0; long long sum = 0;
    for (int seq = 1; count == 0 || seq <= count; seq++) {
        int us = sys_ping(ip, wait, seq, size);
        sent++;
        if (us >= 0) {
            char t[16]; ms(us, t, sizeof t);
            m_printf("%d bytes from %s: icmp_seq=%d time=%s ms\n", size + 8, ip, seq, t);
            if (!got || us < min) min = us;
            if (us > max) max = us;
            sum += us; got++;
        } else if (us == -3) m_printf("From %s: icmp_seq=%d Destination unreachable\n", ip, seq);
        else if (us == -2) { m_eputs("ping: cannot send (no network or raw sockets unavailable)\n"); sent--; break; }
        else if (us == -4) { sent--; break; }
        else m_printf("Request timeout for icmp_seq=%d\n", seq);
        if (sys_sigint(0)) break;
        if (count == 0 || seq < count) { sys_sleep_ms((unsigned)interval * 1000); if (sys_sigint(0)) break; }
    }
    m_printf("\n--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n", host, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) { char a[16], b[16], c[16]; ms(min, a, sizeof a); ms((int)(sum / got), b, sizeof b); ms(max, c, sizeof c); m_printf("rtt min/avg/max = %s/%s/%s ms\n", a, b, c); }
    return got ? 0 : 1;
}
