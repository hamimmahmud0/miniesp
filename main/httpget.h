// HTTPS/HTTP download to a file, for the sys_http_get syscall (package manager).
#pragma once
#include <stdbool.h>

// Download url into host_path (written as host_path.part, renamed on success; certificates are checked
// against the built-in bundle, so the clock must be right). Returns bytes saved, or
//   -1 connection/TLS/transport error     -2 file write error    -3 larger than max_bytes
//   -4 cancelled                          -HTTPSTATUS (e.g. -404) for a non-200 answer.
int http_download(const char *url, const char *host_path, int max_bytes, int timeout_ms,
                  bool (*cancel)(void *), void *arg);
