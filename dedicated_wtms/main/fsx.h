#pragma once
#include <stdbool.h>
#define FS_BASE "/lfs"                 // LittleFS (partition "storage", the same layout as the miniesp OS)
bool fs_mount(void);                   // mounts (creates /tank and /log)
void fs_migrate_legacy(void);          // once, after this image has proven healthy: removes the miniesp OS files (programs, web pages, packages), keeps /log, moves the tank history to /tank
