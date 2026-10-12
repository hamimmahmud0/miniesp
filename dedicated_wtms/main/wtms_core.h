#pragma once
#include <stdint.h>
void wtms_start(void);                                       // starts the controller task on core 1
void wtms_init(void);
void wtms_cycle(void);
void wtms_lock(void);
void wtms_unlock(void);
int wtms_web(int argc, char **argv, char *out, int cap);     // the dashboard API (see the web pages); returns the output length
int wtms_token_ok(const char *token);                        // is this a live dashboard session token?
uint32_t wtms_cycles(void);                                  // control-loop iterations since boot (health check)
