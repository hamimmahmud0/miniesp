// sonar - read an HC-SR04 ultrasonic distance sensor.
//
//   sonar                 one reading (median of 5), e.g.  "distance 23.4 cm"
//   sonar -j              the same as one line of JSON (for scripts and the web dashboard)
//   sonar -n 0 -i 500     keep reading every 500 ms until Ctrl-C   (-n COUNT, -i MS)
//   sonar -t 33 -e 32     trigger / echo GPIO pins (defaults 33 and 32, see BOARDS.md)
//   sonar -s 7            samples per reading (median), default 5
//
// The echo pulse is timed with sys_micros(). WiFi interrupts can disturb single samples, so each reading is
// the median of several. Wiring: the HC-SR04 echo pin is 5 V - use a voltage divider (e.g. 1k/2k) to the ESP32.
#include "mini.h"

static int trig = 33, echo = 32;

static void delay_us(unsigned us) { unsigned t0 = sys_micros(); while (sys_micros() - t0 < us) {} }

// Echo pulse width in microseconds, or -1 (no echo started) / -2 (never ended).
static int pulse_us(void)
{
    sys_gpio_write(trig, 0);
    delay_us(5);
    sys_gpio_write(trig, 1);
    delay_us(12);                                    // >= 10 us trigger pulse
    sys_gpio_write(trig, 0);
    unsigned t0 = sys_micros();
    while (!sys_gpio_read(echo)) if (sys_micros() - t0 > 30000) return -1;
    unsigned rise = sys_micros();
    while (sys_gpio_read(echo)) if (sys_micros() - rise > 40000) return -2;
    return (int)(sys_micros() - rise);
}

static void sort(int *a, int n) { for (int i = 1; i < n; i++) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; } }

int main(int argc, char **argv)
{
    int count = 1, interval = 1000, samples = 5, json = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!m_strcmp(a, "-j")) json = 1;
        else if (!m_strcmp(a, "-n") && i + 1 < argc) count = m_atoi(argv[++i]);
        else if (!m_strcmp(a, "-i") && i + 1 < argc) interval = m_atoi(argv[++i]);
        else if (!m_strcmp(a, "-t") && i + 1 < argc) trig = m_atoi(argv[++i]);
        else if (!m_strcmp(a, "-e") && i + 1 < argc) echo = m_atoi(argv[++i]);
        else if (!m_strcmp(a, "-s") && i + 1 < argc) samples = m_atoi(argv[++i]);
        else { m_eputs("usage: sonar [-j] [-n COUNT] [-i MS] [-t TRIG] [-e ECHO] [-s SAMPLES]\n"); return 2; }
    }
    if (samples < 1) samples = 1;
    if (samples > 15) samples = 15;
    if (sys_gpio_mode(trig, 1) < 0 || sys_gpio_mode(echo, 0) < 0) { m_eprintf("sonar: bad pin\n"); return 1; }

    sys_sigint(1);                                   // Ctrl-C ends a continuous run cleanly
    int rc = 0;
    for (int k = 0; count == 0 || k < count; k++) {
        int v[15], n = 0;
        for (int s = 0; s < samples; s++) {
            int us = pulse_us();
            if (us >= 100) v[n++] = us;              // < ~1.7 cm is below the sensor's range: ignore
            sys_sleep_ms(60);                        // let the echo of the previous burst die out
            if (sys_sigint(0)) return rc;
        }
        if (n == 0) {
            rc = 1;
            if (json) m_printf("{\"ok\":false,\"error\":\"no echo\",\"t\":%u}\n", sys_millis());
            else m_puts("no echo (sensor not connected, wrong pins, or nothing in range)\n");
        } else {
            sort(v, n);
            int us = v[n / 2];
            int mm = (int)((long long)us * 1715 / 10000);                  // 343 m/s, there and back
            if (json) m_printf("{\"ok\":true,\"cm\":%d.%d,\"mm\":%d,\"us\":%d,\"samples\":%d,\"t\":%u}\n", mm / 10, mm % 10, mm, us, n, sys_millis());
            else m_printf("distance %d.%d cm   (echo %d us, %d/%d samples)\n", mm / 10, mm % 10, us, n, samples);
        }
        if (count == 0 || k + 1 < count) sys_sleep_ms((unsigned)interval);
    }
    return rc;
}
