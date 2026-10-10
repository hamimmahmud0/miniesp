// Native benchmark workers for esp-bench (syscall sys_bench). Each worker task is pinned to one core, runs the shared kernel
// (programs/bench_kernels.h) for a fixed time and reports iterations per second. Both cores start together so the windows overlap.
#include "bench.h"
#include <stdlib.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../programs/bench_kernels.h"

#define BG 8                                    // flag in `cores`: start the workers and return; join with kind = -1

static struct { SemaphoreHandle_t done; volatile int go; int kind, ms, active; volatile int32_t rate[2]; volatile uint32_t chk[2]; } B;

static void worker(void *arg)
{
    int core = (int)(intptr_t)arg;
    uint32_t *sc = malloc(BK_SCRATCH);
    if (!sc) { B.rate[core] = -2; xSemaphoreGive(B.done); vTaskDelete(NULL); }
    bk_prepare(sc);
    while (!B.go) taskYIELD();
    int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)B.ms * 1000, t;
    uint32_t acc = 0, n = 0;
    do {
        for (int i = 0; i < 8; i++) acc ^= bk_iter(B.kind, sc, acc + (uint32_t)n++);
        t = esp_timer_get_time();
    } while (t < end);
    B.chk[core] = acc;
    B.rate[core] = (int32_t)((int64_t)n * 1000000 / (t - t0));
    free(sc);
    xSemaphoreGive(B.done);
    vTaskDelete(NULL);
}

int bench_cores(void) { return portNUM_PROCESSORS; }

int bench_run(int kind, int cores, int ms, int32_t *out, int outn)
{
    if (!B.done) B.done = xSemaphoreCreateCounting(2, 0);
    if (kind < 0) {                             // join a background run
        if (!B.active) return -1;
        for (int i = 0; i < B.active; i++) xSemaphoreTake(B.done, pdMS_TO_TICKS(B.ms + 3000));
        B.active = 0;
        goto report;
    }
    int bg = cores & BG; cores &= 3;
    if (kind >= BK_KINDS || !cores || ms < 20 || B.active) return -1;
    if (ms > 3000) ms = 3000;
    if ((cores & 2) && portNUM_PROCESSORS < 2) return -1;
    B.kind = kind; B.ms = ms; B.go = 0; B.rate[0] = B.rate[1] = 0;
    int n = 0;
    for (int c = 0; c < 2; c++) if (cores & (1 << c)) {
        if (xTaskCreatePinnedToCore(worker, "bench", 3072, (void *)(intptr_t)c, 6, NULL, c) != pdPASS) return -3;
        n++;
    }
    B.active = n;
    vTaskDelay(2);                              // let the workers reach their start barrier
    B.go = 1;
    if (bg) return 0;
    for (int i = 0; i < n; i++) xSemaphoreTake(B.done, pdMS_TO_TICKS(ms + 3000));
    B.active = 0;
report:
    if (out && outn >= 8) { out[0] = B.rate[0]; out[1] = B.rate[1]; }
    return B.rate[0] + B.rate[1];
}
