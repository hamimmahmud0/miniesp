#include "drivers.h"
#include <string.h>
#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "drv";

uint32_t drv_micros(void) { return (uint32_t)esp_timer_get_time(); }

/* ---------------- GPIO ---------------- */
static int pin_ok(int p) { return p >= 0 && p <= 39 && GPIO_IS_VALID_GPIO(p) && !(p >= 6 && p <= 11); }
static int pin_ok_out(int p) { return pin_ok(p) && GPIO_IS_VALID_OUTPUT_GPIO(p); }

int drv_gpio_mode(int pin, int out)
{
    if (out ? !pin_ok_out(pin) : !pin_ok(pin)) return -1;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, out ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
    if (!out) gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    return 0;
}
int drv_gpio_write(int pin, int level) { return pin_ok_out(pin) ? (gpio_set_level(pin, level != 0), 0) : -1; }
int drv_gpio_read(int pin) { if (!pin_ok(pin)) return -1; PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[pin]); return gpio_get_level(pin); }

/* ---------------- ADC1 ---------------- */
static adc_oneshot_unit_handle_t s_adc;
static uint8_t s_adc_cfg[8];

int drv_adc_read(int pin)
{
    adc_unit_t unit; adc_channel_t ch;
    if (!pin_ok(pin) || adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK || unit != ADC_UNIT_1) return -1;
    if (!s_adc) {
        adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) return -1;
    }
    if (!s_adc_cfg[ch]) {
        adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_oneshot_config_channel(s_adc, ch, &c) != ESP_OK) return -1;
        s_adc_cfg[ch] = 1;
    }
    int raw = 0;
    return adc_oneshot_read(s_adc, ch, &raw) == ESP_OK ? raw : -1;
}

/* ---------------- PWM (LEDC, low-speed mode) ---------------- */
#define PWM_CH 8
#define PWM_TIMERS 4
static struct { int pin; int timer; } s_pwm[PWM_CH];                // pin 0 = free (GPIO0 is not used for PWM)
static int s_pwm_freq[PWM_TIMERS];

int drv_pwm(int pin, int freq, int permille)
{
    if (!pin_ok_out(pin) || pin == 0) return -1;
    int ch = -1;
    for (int i = 0; i < PWM_CH; i++) if (s_pwm[i].pin == pin) { ch = i; break; }
    if (permille < 0) {                                              // stop
        if (ch < 0) return 0;
        ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0);
        s_pwm[ch].pin = 0;
        gpio_reset_pin(pin);
        return 0;
    }
    if (freq < 1 || freq > 40000 || permille > 1000) return -1;
    if (ch < 0) for (int i = 0; i < PWM_CH; i++) if (!s_pwm[i].pin) { ch = i; break; }
    if (ch < 0) return -1;
    int tm = -1;                                                     // a timer already running at this frequency, else a free one
    for (int t = 0; t < PWM_TIMERS; t++) if (s_pwm_freq[t] == freq) { tm = t; break; }
    if (tm < 0) {
        for (int t = 0; t < PWM_TIMERS && tm < 0; t++) {
            int used = 0;
            for (int i = 0; i < PWM_CH; i++) if (s_pwm[i].pin && s_pwm[i].timer == t && i != ch) used = 1;
            if (!used) tm = t;
        }
        if (tm < 0) return -1;
        ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = tm,
                                   .freq_hz = (uint32_t)freq, .clk_cfg = LEDC_AUTO_CLK };
        if (ledc_timer_config(&tc) != ESP_OK) return -1;
        s_pwm_freq[tm] = freq;
    }
    ledc_channel_config_t cc = { .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch, .intr_type = LEDC_INTR_DISABLE,
                                 .timer_sel = tm, .duty = (uint32_t)(permille * 1023 / 1000), .hpoint = 0 };
    if (ledc_channel_config(&cc) != ESP_OK) return -1;
    s_pwm[ch].pin = pin; s_pwm[ch].timer = tm;
    return 0;
}

/* ---------------- DAC (GPIO25 / GPIO26) ---------------- */
static dac_oneshot_handle_t s_dac[2];

int drv_dac_write(int pin, int value)
{
    int i = pin == 25 ? 0 : pin == 26 ? 1 : -1;
    if (i < 0 || value < 0 || value > 255) return -1;
    if (!s_dac[i]) {
        dac_oneshot_config_t c = { .chan_id = i == 0 ? DAC_CHAN_0 : DAC_CHAN_1 };
        if (dac_oneshot_new_channel(&c, &s_dac[i]) != ESP_OK) return -1;
    }
    return dac_oneshot_output_voltage(s_dac[i], (uint8_t)value) == ESP_OK ? 0 : -1;
}

/* ---------------- I2C ---------------- */
#define I2C_PORT I2C_NUM_0
static bool s_i2c_up;

int drv_i2c_init(int sda, int scl, int hz)
{
    if (sda <= 0) sda = 21;
    if (scl <= 0) scl = 22;
    if (hz <= 0) hz = 100000;
    if (!pin_ok_out(sda) || !pin_ok_out(scl) || hz > 1000000) return -1;
    if (s_i2c_up) { i2c_driver_delete(I2C_PORT); s_i2c_up = false; }
    i2c_config_t c = { .mode = I2C_MODE_MASTER, .sda_io_num = sda, .scl_io_num = scl, .sda_pullup_en = true, .scl_pullup_en = true,
                       .master.clk_speed = (uint32_t)hz };
    if (i2c_param_config(I2C_PORT, &c) != ESP_OK || i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return -1;
    s_i2c_up = true;
    return 0;
}

static int i2c_ready(void) { return s_i2c_up ? 0 : drv_i2c_init(0, 0, 0); }

int drv_i2c_probe(int addr)
{
    if (addr < 1 || addr > 127 || i2c_ready()) return -1;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return e == ESP_OK ? 0 : -1;
}
int drv_i2c_write(int addr, const uint8_t *buf, int n)
{
    if (addr < 1 || addr > 127 || n < 0 || i2c_ready()) return -1;
    return i2c_master_write_to_device(I2C_PORT, (uint8_t)addr, buf, (size_t)n, pdMS_TO_TICKS(200)) == ESP_OK ? 0 : -1;
}
int drv_i2c_read(int addr, uint8_t *buf, int n)
{
    if (addr < 1 || addr > 127 || n <= 0 || i2c_ready()) return -1;
    return i2c_master_read_from_device(I2C_PORT, (uint8_t)addr, buf, (size_t)n, pdMS_TO_TICKS(200)) == ESP_OK ? n : -1;
}
int drv_i2c_write_read(int addr, const uint8_t *w, int wn, uint8_t *r, int rn)
{
    if (addr < 1 || addr > 127 || wn < 0 || rn <= 0 || i2c_ready()) return -1;
    return i2c_master_write_read_device(I2C_PORT, (uint8_t)addr, w, (size_t)wn, r, (size_t)rn, pdMS_TO_TICKS(200)) == ESP_OK ? rn : -1;
}

/* ---------------- UART 1/2 ---------------- */
static bool s_uart_up[3];

int drv_uart_open(int port, int tx, int rx, int baud)
{
    if ((port != 1 && port != 2) || baud < 300 || baud > 2000000 || !pin_ok_out(tx) || !pin_ok(rx)) return -1;
    if (s_uart_up[port]) uart_driver_delete(port);
    uart_config_t c = { .baud_rate = baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    if (uart_param_config(port, &c) != ESP_OK || uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK
        || uart_driver_install(port, 1024, 0, 0, NULL, 0) != ESP_OK) return -1;
    s_uart_up[port] = true;
    return 0;
}
int drv_uart_write(int port, const uint8_t *buf, int n)
{
    if ((port != 1 && port != 2) || !s_uart_up[port] || n < 0) return -1;
    return uart_write_bytes(port, buf, (size_t)n);
}
int drv_uart_read(int port, uint8_t *buf, int n, int timeout_ms)
{
    if ((port != 1 && port != 2) || !s_uart_up[port] || n <= 0) return -1;
    return uart_read_bytes(port, buf, (uint32_t)n, pdMS_TO_TICKS(timeout_ms < 0 ? 0 : timeout_ms));
}
int drv_uart_close(int port)
{
    if ((port != 1 && port != 2) || !s_uart_up[port]) return -1;
    s_uart_up[port] = false;
    return uart_driver_delete(port) == ESP_OK ? 0 : -1;
}


/* ---------------- precise timing ---------------- */
void drv_delay_us(int us)
{
    if (us <= 0) return;
    if (us > 100000) us = 100000;
    int64_t end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end) {}
}

int drv_pulse_in(int pin, int level, int timeout_us)
{
    if (!pin_ok(pin) || timeout_us <= 0) return -1;
    level = level != 0;
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[pin]);
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(pin) != level) if (esp_timer_get_time() - t0 > timeout_us) return -1;
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(pin) == level) if (esp_timer_get_time() - rise > 60000) return -2;
    return (int)(esp_timer_get_time() - rise);
}

int drv_sonar_pulse(int trig, int echo, int timeout_us)
{
    static int s_t = -1, s_e = -1;
    if (trig != s_t || echo != s_e) {                    // configure the pins once (and again only if they change)
        if (drv_gpio_mode(trig, 1) < 0 || drv_gpio_mode(echo, 0) < 0) return -1;
        s_t = trig; s_e = echo;
    }
    if (timeout_us <= 0) timeout_us = 30000;
    gpio_set_level(trig, 0);
    drv_delay_us(4);
    gpio_set_level(trig, 1);
    drv_delay_us(10);
    gpio_set_level(trig, 0);
    return drv_pulse_in(echo, 1, timeout_us);
}

/* ---------------- ADC with calibration ---------------- */
int drv_adc_read_mv(int pin)
{
    int raw = drv_adc_read(pin);
    if (raw < 0) return -1;
    static adc_cali_handle_t cali;
    static bool tried;
    if (!tried) {
        tried = true;
        adc_cali_line_fitting_config_t c = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_cali_create_scheme_line_fitting(&c, &cali) != ESP_OK) cali = NULL;
    }
    int mv = 0;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3100 / 4095;                             // uncalibrated estimate
}

/* ---------------- DS18B20 (1-Wire bit-banged; each bit slot runs with interrupts off, ~70 us) ---------------- */
static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_ow_pin = -1;

static int ow_reset(void)
{
    int present;
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(s_ow_pin, 0); esp_rom_delay_us(480);
    gpio_set_level(s_ow_pin, 1); esp_rom_delay_us(70);
    present = !gpio_get_level(s_ow_pin);
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);
    return present;
}
static void ow_write_bit(int b)
{
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(s_ow_pin, 0);
    esp_rom_delay_us(b ? 6 : 60);
    gpio_set_level(s_ow_pin, 1);
    esp_rom_delay_us(b ? 64 : 10);
    portEXIT_CRITICAL(&s_ow_mux);
}
static int ow_read_bit(void)
{
    int b;
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(s_ow_pin, 0); esp_rom_delay_us(3);
    gpio_set_level(s_ow_pin, 1); esp_rom_delay_us(10);
    b = gpio_get_level(s_ow_pin);
    esp_rom_delay_us(53);
    portEXIT_CRITICAL(&s_ow_mux);
    return b;
}
static void ow_write(uint8_t v) { for (int i = 0; i < 8; i++) { ow_write_bit(v & 1); v >>= 1; } }
static uint8_t ow_read(void) { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= (uint8_t)(ow_read_bit() << i); return v; }
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0;
    for (int i = 0; i < n; i++) { uint8_t b = d[i]; for (int k = 0; k < 8; k++) { uint8_t m = (c ^ b) & 1; c >>= 1; if (m) c ^= 0x8C; b >>= 1; } }
    return c;
}

int drv_ds18b20(int pin)
{
    if (!pin_ok_out(pin)) return -9999;
    s_ow_pin = pin;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);   // open drain: the external pull-up makes the line high
    gpio_set_level(pin, 1);
    if (!ow_reset()) return -9999;                       // no device answered
    ow_write(0xCC); ow_write(0x44);                      // skip ROM, convert T
    vTaskDelay(pdMS_TO_TICKS(800));
    if (!ow_reset()) return -9999;
    ow_write(0xCC); ow_write(0xBE);                      // read scratchpad
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read();
    if (crc8(sp, 8) != sp[8]) return -9999;
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    if (raw == 0x0550 && sp[4] == 0x7F) return -9999;    // power-on default 85 C with an unconfigured sensor
    return (int)raw * 100 / 16;
}

/* ---------------- SPI ---------------- */
static spi_device_handle_t s_spi;
static bool s_spi_bus;

int drv_spi_open(int sck, int mosi, int miso, int hz, int mode)
{
    if (!pin_ok_out(sck) || !pin_ok_out(mosi) || (miso >= 0 && !pin_ok(miso)) || hz < 1000 || hz > 20000000 || mode < 0 || mode > 3) return -1;
    if (s_spi) { spi_bus_remove_device(s_spi); s_spi = NULL; }
    if (s_spi_bus) { spi_bus_free(SPI3_HOST); s_spi_bus = false; }
    spi_bus_config_t b = { .sclk_io_num = sck, .mosi_io_num = mosi, .miso_io_num = miso, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64 };
    if (spi_bus_initialize(SPI3_HOST, &b, SPI_DMA_DISABLED) != ESP_OK) return -1;
    s_spi_bus = true;
    spi_device_interface_config_t d = { .clock_speed_hz = hz, .mode = mode, .spics_io_num = -1, .queue_size = 1 };
    if (spi_bus_add_device(SPI3_HOST, &d, &s_spi) != ESP_OK) return -1;
    return 0;
}

int drv_spi_xfer(int cs, const uint8_t *tx, uint8_t *rx, int n)
{
    if (!s_spi || n < 1 || n > 64) return -1;
    if (cs >= 0) { if (!pin_ok_out(cs)) return -1; gpio_set_direction(cs, GPIO_MODE_OUTPUT); gpio_set_level(cs, 0); }
    spi_transaction_t t = { .length = (size_t)n * 8, .tx_buffer = tx, .rx_buffer = rx };
    uint8_t dummy[64];
    if (!tx) { memset(dummy, 0, sizeof dummy); t.tx_buffer = dummy; }
    esp_err_t e = spi_device_polling_transmit(s_spi, &t);
    if (cs >= 0) gpio_set_level(cs, 1);
    return e == ESP_OK ? n : -1;
}

/* ---------------- pulse counters ---------------- */
#define PCNT_HIGH 30000
static pcnt_unit_handle_t s_pcnt[2];
static volatile int64_t s_pcnt_acc[2];

static bool pcnt_overflow(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *ev, void *ctx)
{
    s_pcnt_acc[(int)(intptr_t)ctx] += PCNT_HIGH;                  // the hardware counter was cleared by the limit
    return false;
}

int drv_pcnt_open(int unit, int pin)
{
    if (unit < 0 || unit > 1 || !pin_ok(pin)) return -1;
    if (s_pcnt[unit]) { pcnt_unit_stop(s_pcnt[unit]); pcnt_unit_disable(s_pcnt[unit]); pcnt_del_unit(s_pcnt[unit]); s_pcnt[unit] = NULL; }
    pcnt_unit_config_t uc = { .low_limit = -1, .high_limit = PCNT_HIGH, .flags.accum_count = 0 };
    if (pcnt_new_unit(&uc, &s_pcnt[unit]) != ESP_OK) return -1;
    pcnt_glitch_filter_config_t fc = { .max_glitch_ns = 1000 };
    pcnt_unit_set_glitch_filter(s_pcnt[unit], &fc);
    pcnt_chan_config_t cc = { .edge_gpio_num = pin, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch;
    if (pcnt_new_channel(s_pcnt[unit], &cc, &ch) != ESP_OK) return -1;
    pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_unit_add_watch_point(s_pcnt[unit], PCNT_HIGH);
    pcnt_event_callbacks_t cb = { .on_reach = pcnt_overflow };
    pcnt_unit_register_event_callbacks(s_pcnt[unit], &cb, (void *)(intptr_t)unit);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    s_pcnt_acc[unit] = 0;
    pcnt_unit_enable(s_pcnt[unit]);
    pcnt_unit_clear_count(s_pcnt[unit]);
    pcnt_unit_start(s_pcnt[unit]);
    return 0;
}

int drv_pcnt_read(int unit)
{
    if (unit < 0 || unit > 1 || !s_pcnt[unit]) return -1;
    int c = 0;
    pcnt_unit_get_count(s_pcnt[unit], &c);
    return (int)(s_pcnt_acc[unit] + c);
}

int drv_pcnt_clear(int unit)
{
    if (unit < 0 || unit > 1 || !s_pcnt[unit]) return -1;
    pcnt_unit_clear_count(s_pcnt[unit]);
    s_pcnt_acc[unit] = 0;
    return 0;
}
