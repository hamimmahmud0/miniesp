# dedicated_wtms

A stand-alone ESP-IDF firmware that runs **only the water tank manager (wtms)**: no shell, no programs, no WebAssembly runtime, no unix-like OS.
It is a sub-project of this repository; the miniesp OS next to it is not part of this build and is never modified by it.

| | |
|---|---|
| **Core 1** | the tank controller: sonar (echo timed by an IRAM GPIO interrupt), level, pump control, history, Home Assistant discovery |
| **Core 0** | the system: Wi-Fi + TCP/IP, MQTT I/O, dashboard + API (port 80), SSH log viewer (port 22), OTA, log files |
| **Dashboard** | `http://<board>/tank/` - the same pages and API as the wtms package (login, settings, force control, charts) |
| **SSH** | `ssh esp@<board>` shows the live log (last 40 lines, then new ones; `q` quits); `ssh esp@<board> log 200` prints the last 200 lines. No shell, no other commands |
| **Log** | everything logged by the firmware and by the controller goes to `/log/kernel.log` + `/log/kernel.1` (two files cycling, each half of min(512 KB, free space - 96 KB)); a finished file is published over MQTT to `<client-id>/log/kernel` |
| **OTA** | `tools/ota.sh HOST` (HTTP POST of the image with its SHA-256, key = SSH password); two app slots, automatic rollback if the new image is not healthy within ~60 s |

## Build
```
tools/build.sh            # -> build/dedicated_wtms.bin  (~1.2 MB; the OTA slot is 1.4 MB)
```
Credentials are read (never written) from `../.creds/` (`wifi_ssid`, `wifi_password`, `esp32_ssh_password`, `esp32_hostkey.der`), like the miniesp OS.
Dependencies (wolfSSH, wolfSSL, mDNS, LittleFS) come from the ESP component registry into `managed_components/`.

## Compatibility with a board that runs the miniesp OS
The partition table is identical to miniesp's, and the settings live in the same NVS namespaces, so nothing has to be configured again:
`kv` (wtms configuration, state, web password, sessions), `mqtt` (broker, user, password, client id), `net` (hostname, static IP), `wifi`, `time` (time zone).
- **First install** over the network, from the miniesp OS: `NO_SYNC=1 ../tools/ota.sh HOST dedicated_wtms/build/dedicated_wtms.bin` (the OS's own `ota` command; `NO_SYNC=1` skips its file sync). If the new image is unhealthy the board returns to the OS by itself.
- **Afterwards** `tools/ota.sh HOST`.
- Once the image has been confirmed healthy it removes the OS's files from the filesystem (`/bin /esp /etc /www /tmp /pkg`) and keeps `/log` and the tank history (moved to `/tank`). Until then a rollback finds the OS intact.
- Going back to the OS needs a USB flash of its image (the OS does not contain a rollback path to this firmware).

## Layout
```
main/wtms_core.c   the wtms control logic (a native fork of miniesp-pkg/programs/wtms.c 1.3.7: same decisions, config keys, MQTT topics, history files, web API)
main/compat.c      the file / NVS / MQTT / clock calls that code uses, implemented natively (state.json lives in RAM)
main/sonar.c       interrupt-timed HC-SR04      main/net.c  Wi-Fi + mDNS      main/mqttc.c  MQTT client (cache of subscribed topics)
main/web.c         dashboard + API + csv files  main/ssh_log.c  log-only SSH   main/ota.c  OTA + health check     main/klog.c  log files + MQTT shipping
web/               the pages (embedded in the firmware; style.css and ui.js are copies of the miniesp design system)
```
## Differences from the program
- No DS18B20 temperature probe (the speed of sound uses `temp_c`).
- `tank.st` (state) is written to NVS only when something important changed or every 2 minutes (the program wrote it every cycle).
- state.json is served from RAM instead of a file.
- Setup is done on the web page (Settings) or by keeping the miniesp settings; there is no `wtms setup`.
