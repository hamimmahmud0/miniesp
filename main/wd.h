#pragma once
// Watchdog for the network services. Every 20 s it connects to port 22 and 80 from inside the device;
// repeated failures mean the service is wedged (e.g. a stuck session holding the only connection slot).
// It then writes diagnostics (serial log + /www/stall.log, served over HTTP), unsticks the session,
// restarts the listener and, as a last resort, reboots (with a loop guard).
void wd_start(void);
void wd_stop(void);
int wd_running(void);
void wd_diagnostics(const char *why);       // dump diagnostics now (serial + /www/stall.log)
