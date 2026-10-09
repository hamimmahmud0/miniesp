#pragma once
// Basic hardware driver access, shared by the shell built-ins (gpio, adc, pwm, i2c) and the sys_* calls
// of AOT programs. All functions return >= 0 on success and a negative value on error.
#include <stddef.h>
#include <stdint.h>

uint32_t drv_micros(void);                          // microseconds since boot (wraps after ~71 min)

// GPIO: pins 6-11 (flash) are refused; 34-39 are input-only.
int drv_gpio_mode(int pin, int out);                // out: 1 output, 0 input with pull-up
int drv_gpio_write(int pin, int level);
int drv_gpio_read(int pin);

// ADC1 (GPIO32-39), 12-bit raw 0..4095 at ~0-3.1 V (11 dB attenuation). ADC2 is not usable together with WiFi.
int drv_adc_read(int pin);

// PWM via LEDC on any output pin: freq 1..40000 Hz, duty in 1/1000 (0..1000); duty < 0 stops and releases the pin.
int drv_pwm(int pin, int freq_hz, int duty_permille);

// DAC on GPIO25 / GPIO26: 8-bit value 0..255 (0..3.3 V).
int drv_dac_write(int pin, int value);

// I2C master (one bus). init defaults: SDA 21, SCL 22, 100 kHz.
int drv_i2c_init(int sda, int scl, int hz);
int drv_i2c_probe(int addr7);                       // 0 = a device acknowledged
int drv_i2c_write(int addr7, const uint8_t *buf, int n);
int drv_i2c_read(int addr7, uint8_t *buf, int n);
int drv_i2c_write_read(int addr7, const uint8_t *w, int wn, uint8_t *r, int rn);

// UART 1 and 2 (UART0 carries the console).
int drv_uart_open(int port, int tx, int rx, int baud);
int drv_uart_write(int port, const uint8_t *buf, int n);
int drv_uart_read(int port, uint8_t *buf, int n, int timeout_ms);
int drv_uart_close(int port);

// ---- precise timing helpers (native, so there is no AOT call overhead inside the measurement) ----
void drv_delay_us(int us);                          // busy-wait, up to 100000 us
int drv_pulse_in(int pin, int level, int timeout_us);   // width in us of the next pulse at `level`; -1 timeout waiting for it, -2 too long
int drv_sonar_pulse(int trig, int echo, int timeout_us);// HC-SR04: 10 us trigger, then the echo pulse width in us (or -1/-2)
int drv_ds18b20(int pin);                           // DS18B20 (single device on the pin, 4.7k pull-up): temperature in 1/100 degC, -9999 on error (~0.8 s)
int drv_adc_read_mv(int pin);                       // ADC1 with the factory calibration (mV), -1 on error

// ---- SPI master (one bus, up to 64 bytes per transfer) ----
int drv_spi_open(int sck, int mosi, int miso, int hz, int mode);
int drv_spi_xfer(int cs, const uint8_t *tx, uint8_t *rx, int n);   // tx and/or rx may be NULL; cs is a GPIO driven low during the transfer (-1 = none)

// ---- pulse counters (flow meters, rain gauges, tachometers): 2 units, count rising edges ----
int drv_pcnt_open(int unit, int pin);
int drv_pcnt_read(int unit);                        // total count since open/clear (wraps at 2^31)
int drv_pcnt_clear(int unit);
