// fib [n]: naive recursive Fibonacci (default 24) - a CPU-bound demo that shows AOT speed.
#include "mini.h"

static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)m_atoi(argv[1]) : 24;
    if (n > 40) { m_eputs("fib: n too large\n"); return 1; }
    m_printf("fib(%u) = %u\n", n, fib(n));
    return 0;
}
