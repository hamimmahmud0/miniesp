// blink [pin] [count]: toggles a GPIO (default: GPIO2 = on-board LED on most dev boards, 5 times).
// Shows an AOT program driving real hardware; Ctrl-C stops it.
#include "mini.h"

int main(int argc, char **argv)
{
    int pin = argc > 1 ? m_atoi(argv[1]) : 2;
    int count = argc > 2 ? m_atoi(argv[2]) : 5;
    if (sys_gpio_mode(pin, 1) < 0) { m_eprintf("blink: bad pin %d\n", pin); return 1; }
    for (int i = 0; i < count; i++) {
        sys_gpio_write(pin, 1); sys_sleep_ms(250);
        sys_gpio_write(pin, 0); sys_sleep_ms(250);
        m_printf("blink %d/%d\n", i + 1, count);
    }
    return 0;
}
