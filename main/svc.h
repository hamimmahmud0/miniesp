#pragma once
// A small systemd-like service manager.
//   - native services: sshd, www, mdns  (start / stop / enable at boot)
//   - unit files /etc/services/NAME.service run AOT programs in the background:
//         [Service]
//         Description=Log the distance
//         ExecStart=/bin/sonar -j -s 3          program + arguments (~/.local/bin and /bin are searched)
//         Interval=10                           seconds between runs (timer style); 0 = run once / as a daemon
//         Restart=no|always|on-failure          for Interval=0 programs
//         RestartSec=5
//         Output=/www/sonar.log             also append the output to this file (optional)
//         Enabled=true                          start at boot
//   Note: programs share one runtime, so a running service program holds it; interactive programs wait their turn.
#include "io.h"

void svc_init(void);                                       // start enabled services (call once at boot)
// `service ...` command; output to out/err. Returns an exit status.
int svc_command(int argc, char **argv, io_t *out, io_t *err);
