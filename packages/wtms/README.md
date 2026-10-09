# Smart water tank manager

HC-SR04 (top of the tank, facing down) → ESP32 → MQTT → relay node (`relay_node_1/relay/waterpump`) + Home Assistant + web dashboard.

## Install (on the board)
```
pkg update && pkg install wtms      # program + dashboard + CGI copy
wtms setup                          # interactive (web port, dashboard password, MQTT broker/port, tank size, pins, limits, time zone)
wtms setup --headless mqtt_host=... mqtt_pass=... web_port=80 length=100 width=100 height=100 full_cm=15   # no prompts; add --start to start pump control
```
Setup validates first, saves the settings (NVS `tank.*`), creates the `wtms` service (not started unless you agree / pass `--start`), checks the sensor and prints the dashboard URL. Re-running it keeps the current values as defaults. `pkg remove wtms` removes the files and the service but keeps the settings.
The relay node's auto-off timer (60 s) is not set by setup any more: set it in the relay node's own web UI.

## What runs
`wtms run` every 5 s (service `wtms`):
median of 7 echoes (temperature compensated if a DS18B20 is set) → level % / litres → smoothing → pump decision → MQTT + `/www/tank/state.json` + history (`history.csv`, 5 min samples, rotated at 16 KB).

Pump control: start at `on_pct` (25) if auto is on, no fault, relay node online, rest time passed and 3 good readings in a row; stop at `off_pct` (95).
The pump is re-commanded ON every cycle while it should run → with the relay's auto-off at 60 s the pump stops by itself if the ESP32/WiFi/broker dies (dead-man).
Faults (latched except `sensor`): `sensor` (no valid echo → pump stopped), `max_run` (ran longer than `max_run` minutes), `no_flow` (level did not rise `rise_pm`/10 % in `flow_min` minutes: dry pump / no source). Clear with the HA "Tank reset fault" button or `wtms cmd reset`.

## Interfaces
| | |
|---|---|
| Dashboard | `http://<board>[:port]/tank/` (level, pump, fault, history chart; read-only without login). `login.html`, `config.html` (Settings) |
| MQTT state | `tank1/state` (retained JSON: level, litres, distance_cm, pump, auto, fault, temp, ...) |
| MQTT control | `tank1/cmd` = `reset`, `force_on[:minutes]`, `force_off[:minutes]`, `release`, `discover`;  `tank1/auto/set` = `ON`/`OFF` (state: `tank1/auto/state`) |
| Home Assistant | auto-discovered device "Water tank": level, volume, sensor distance, fault, auto-fill switch, reset / force ON / force OFF / release buttons (the pump switch comes from the relay node) |
| CLI (ssh) | `wtms status`, `wtms read`, `wtms config key=value ...`, `wtms cmd reset|force_on:30|force_off|release`, `wtms passwd NEW`, `service status wtms` |

## Config keys (`wtms config`)
`name pump ha trig echo temp empty_mm full_mm vol_l on_pct off_pct max_run rest flow_min rise_pm auto hist` — see the header of `programs/tank.c`. Stored in NVS (survives reboots and firmware updates).

## Calibrate
With the tank empty/full run `wtms read` and set `empty_mm` / `full_mm` to the measured distances (mm). Litres assume straight walls (linear).

## Wiring
HC-SR04 VCC 5 V, GND, TRIG → GPIO33, ECHO → GPIO32 **through a 1k/2k divider** (5 V → 3.3 V). Optional DS18B20 data pin with a 4.7k pull-up to 3.3 V.

## Updating the program later
Edit `programs/wtms.c` (or the pages in `packages/wtms/web/`), run `tools/mkbundle.py packages/wtms`, push, then `pkg update && pkg install wtms` on the board (unchanged files are skipped; settings are kept). No firmware flash needed.

## Force control and login
- **Force pump ON** (dashboard / Settings → "Force control", HA button, `tank cmd force_on:30`): runs the pump for the chosen time regardless of level, auto mode and faults; only an overflow guard at 100 % still stops it. **Force pump OFF** (`force_off[:minutes]`) keeps it off until **Release** (`release`). Both survive until their time is up or released.
- **Login**: `login.html`. The dashboard is read-only for visitors; controls and Settings need a password. First visit with no password set asks you to create one (or set it at install, or `ssh esp@board "tank passwd NEWPASS"`). Sessions last 7 days (max 6), password changes log everyone out, wrong passwords are delayed 1.2 s. It is plain HTTP: protects against casual use on your LAN, not against someone sniffing it.
- The CGI query is limited to ~240 characters; the Settings page therefore saves in small pieces (`stage` … `commit`), and a cut-off request is rejected instead of half-applied.

## Status of testing
Tested on hardware: installer end to end, upload verification, service, MQTT publish/subscribe, Home Assistant discovery messages, dashboard/settings/login pages (screenshots at phone and desktop sizes), login/token flow, staged settings save, force OFF/release, pump ON/OFF over MQTT earlier, safe behaviour with no sensor.
NOT tested: a real HC-SR04 in a tank (level maths, start/stop thresholds, `no_flow` / `max_run` faults, DS18B20), **force ON** on the real pump, relay auto-off arming through the relay node API, tablet-size layout, OTA rollback.
With no sensor wired the echo pin floats and gives plausible random readings: keep auto-fill off (`wtms config auto=0`) and the service stopped until the sensor is connected.
