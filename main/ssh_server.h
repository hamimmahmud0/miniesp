#pragma once
#include <stdint.h>
// Start the SSH server task (port 22). One interactive/exec session at a time.
void ssh_server_start(void);
void ssh_server_stop(void);              // stop accepting connections (sessions in progress continue)
int ssh_server_running(void);
int ssh_session_active(void);                    // a client session is being served (the sshd task is busy with it)
const char *ssh_where(uint32_t *age_ms);          // breadcrumb: what the sshd task last announced, and how long ago
void ssh_kill_session(void);                      // shut the live session socket down (unblocks a stuck session)
