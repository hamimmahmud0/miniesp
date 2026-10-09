// greet: reads a line from stdin (the SSH terminal) - shows interactive input.
#include "mini.h"

int main(int argc, char **argv)
{
    char name[64];
    m_puts("What is your name? ");
    int n = sys_read(0, name, sizeof name - 1);        // the terminal delivers one edited line
    if (n <= 0) { m_puts("\n(no input)\n"); return 1; }
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) n--;
    name[n] = 0;
    m_printf("Nice to meet you, %s!\n", n ? name : "stranger");
    return 0;
}
