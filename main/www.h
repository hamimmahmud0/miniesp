#pragma once
#include <stdint.h>

#define WWW_ROOT "/www"                 // virtual path of the web root

// Start the HTTP server (port 80): static files from /www, programs in /www/cgi-bin run as CGI.
void www_start(void);
int www_port(void);                         // configured port (kv key "www.port", default 80)
uint32_t www_requests(void);                // requests served since boot
void www_stop(void);
int www_running(void);
