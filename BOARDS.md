# Boards and wiring

One section per board. Say what you tested and what you did not.

## Reference board: classic ESP32, 4 MB flash (the author's board)
- Chip: ESP32 (Xtensa LX6, single core build), 4 MB flash, no PSRAM
- Memory (from `free`): DRAM about 284 KB, IRAM pool 98.8 KB (largest block 62 KB), one 64 KB program slot
- Status: boots, SSH, `pkg`, OTA, services, web console: tested. OTA rollback after a crash: not exercised on hardware.

| Function | Pin | Notes |
|---|---|---|
| HC-SR04 trigger | GPIO33 | `sonar -t PIN` changes it |
| HC-SR04 echo | GPIO32 | the sensor's echo is 5 V: use a voltage divider (1k/2k) to the ESP32 |
| I2C SDA / SCL | GPIO21 / GPIO22 | default 100 kHz (`i2c scan`) |
| Status LED (`heartbeat` unit, `blink`) | GPIO2 | |
| SPI (`sys_spi_open`) | any free pins | bus on the SPI3 host, 64 bytes per transfer |
| Flash pins | GPIO6-11 | refused by the GPIO commands |
| Input only | GPIO34-39 | |
| ADC | GPIO32-39 (ADC1 only) | `adc PIN` |
| DAC | GPIO25 / GPIO26 | |

## Template for a new board
```
## BOARD NAME (chip, flash size, PSRAM yes/no)
- Memory from `free`: DRAM ..., IRAM pool ... (largest block ...)
- Status: boot / ssh / pkg / OTA / OTA rollback: tested yes/no
| Function | Pin | Notes |
```
