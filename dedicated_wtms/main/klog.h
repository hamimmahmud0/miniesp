#pragma once
#include <stdint.h>
// Log (dedicated_wtms):  everything the firmware and the programs print through ESP_LOG (and sys_log) is also kept in two files,
// /log/kernel.log (active) and /log/kernel.1 (the previous one, replaced when the active one is full).
// Each file may grow to half of min(512 KB, free filesystem space - 96 KB) (at least 8 KB each). When the active file fills up it is
// published over MQTT (topic <client-id>/log/kernel, text chunks of lines) as soon as the broker is connected.
void klog_init(void);                  // after fs_mount(): hooks esp_log, starts the writer task
int klog_tail(int lines, void (*out)(const char *s, int n, void *ctx), void *ctx);   // last N lines of both files, oldest first; returns the number printed
void klog_send_soon(void);             // rotate now and publish the finished file
void klog_info(int *cap_kb, int *size_cur, int *size_old, int *pending, unsigned *dropped);
int klog_live_read(uint32_t *cursor, char *dst, int max);   // new text since *cursor (SSH viewer); advances the cursor
uint32_t klog_live_end(void);
void klog_flush_now(void);               // write the buffered lines to the file now (used for crash tracing)
void klog_trace_persist(int on);            // also write the decision trace ("trace:" lines) to the log files (default: live view only)
