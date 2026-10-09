#pragma once
#include <stdbool.h>
#include "io.h"
#include "term.h"

bool aot_init(void);                                    // wasm_runtime_full_init + register the sys_* natives
// Load a .aot file from LittleFS (virtual path) and run it with the given standard streams.
// `cwd` is the working directory (relative paths opened by the program resolve against it).
// Returns the exit code, or <0 if not loadable. May be called from inside a running program (sys_run).
int aot_run(term_t *t, const char *cwd, const char *vpath, int argc, char **argv, io_t *in, io_t *out, io_t *err);
void aot_interrupt(void);                               // Ctrl-C: terminate the running program
void aot_kill(void);                                   // terminate the running program (timeouts)
bool aot_lock(int timeout_ms);                         // take the run lock (only one top-level program at a time)
void aot_unlock(void);
// Run without taking the lock: the caller must hold it (aot_lock).
int aot_run_nolock(term_t *t, const char *cwd, const char *vpath, int argc, char **argv, io_t *in, io_t *out, io_t *err);
bool aot_interactive_busy(void);                    // a program attached to a terminal holds the runtime (waiting for it is pointless)
