// hello: prints a greeting plus its arguments. The smallest useful WAMR AOT program.
#include "mini.h"

int main(int argc, char **argv)
{
    m_puts("Hello from WAMR AOT on the ESP32!\n");
    for (int i = 0; i < argc; i++)
        m_printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
