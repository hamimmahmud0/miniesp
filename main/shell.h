#pragma once
#include "io.h"
#include "term.h"

// Interactive login shell on a pty terminal. Returns when the user logs out.
void shell_run(term_t *t, const char *user);
// Run a single command line (ssh host "cmd"), output on `t`. Returns exit status.
int shell_exec(term_t *t, const char *line);

// Run one command (built-in or program) with explicit streams. Used by sys_run (programs running commands).
int shell_run_argv(term_t *t, const char *cwd, int argc, char **argv, io_t *in, io_t *out, io_t *err);
