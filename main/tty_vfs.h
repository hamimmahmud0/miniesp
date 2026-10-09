#pragma once
#include <stdbool.h>

#define TTY_PATH "/dev/pts"
// Register a VFS device that bridges stdin/stdout of WASI programs to the current SSH terminal.
bool tty_vfs_register(void);
