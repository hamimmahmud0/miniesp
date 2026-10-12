#pragma once
void web_start(void);
#include <stddef.h>
#include "esp_http_server.h"
void web_peer_ip(httpd_req_t *req, char *out, size_t n);     // client address of a request, for the log
