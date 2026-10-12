#pragma once
// SSH server (wolfSSH, password login) that only shows the log: no shell, no commands.
//   ssh esp@wtms.local            live log: the last 40 lines, then new lines as they appear (q or Ctrl-C quits)
//   ssh esp@wtms.local log [N]    the last N lines (default 100), then exits
void ssh_log_start(void);
