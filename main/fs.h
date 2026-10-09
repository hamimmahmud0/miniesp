#pragma once
#include <stdbool.h>
#include <stddef.h>

#define FS_BASE "/lfs"          // host mount point; the shell shows it as "/"
#define FS_HOME "/esp"          // home directory of the user ("~")

bool fs_mount(void);
// Resolve `arg` against virtual cwd into a normalised virtual path ("/a/b").
void fs_resolve(const char *cwd, const char *arg, char *out, size_t n);
// Virtual path ("/a/b") -> host path ("/lfs/a/b").
void fs_host_path(const char *vpath, char *out, size_t n);
