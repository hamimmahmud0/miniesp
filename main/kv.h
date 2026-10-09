#pragma once
#include <stddef.h>
// Small persistent key-value store in NVS (survives reboots, app updates and filesystem re-imaging).
// Keys: 1-15 chars [A-Za-z0-9_.-]; values: up to 1000 bytes. Good for settings and small state; use files for logs.
int kv_get(const char *key, void *buf, size_t n);          // length, or -1 if missing
int kv_set(const char *key, const void *buf, size_t n);    // 0 ok
int kv_del(const char *key);
typedef void (*kv_iter_cb)(const char *key, size_t len, void *ctx);
void kv_list(kv_iter_cb cb, void *ctx);
