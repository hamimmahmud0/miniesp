#pragma once
#include "io.h"
// Over-the-air firmware update, driven from the shell:  ssh esp@host "ota --sha256 HEX" < build/esp32_unix.bin
//   ota status | ota confirm | ota rollback | ota [--sha256 HEX] [--no-reboot]   (image on stdin)
// The image goes to the inactive app slot, is validated, and becomes the boot slot. A new image must report
// itself healthy within a minute (ota_healthcheck_start); otherwise the bootloader rolls back on the next reset.
int ota_command(int argc, char **argv, io_t *in, io_t *out, io_t *err);
void ota_healthcheck_start(void);                 // call once at boot
