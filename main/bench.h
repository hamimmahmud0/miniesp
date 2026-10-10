#pragma once
#include <stdint.h>
// sys_bench: native CPU kernels (programs/bench_kernels.h) on core 0 / core 1 / both; see programs/mini.h
int bench_run(int kind, int cores, int ms, int32_t *out, int outn);
int bench_cores(void);
