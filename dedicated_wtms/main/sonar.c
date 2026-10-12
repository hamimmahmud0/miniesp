#include "sonar.h"
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sonar";
#define APP_CORE 1
static volatile int64_t s_rise, s_fall;
static volatile int s_have_rise, s_have_fall;
static int s_trig = -1, s_echo = -1, s_ok;                 // configured pins, interrupt ready
static volatile int s_req_trig = -1, s_req_echo = -1;      // requested by another core

static void IRAM_ATTR echo_isr(void *arg)
{
    int64_t t = esp_timer_get_time();
    if (gpio_get_level((gpio_num_t)(intptr_t)arg)) { if (!s_have_rise) { s_rise = t; s_have_rise = 1; } }
    else if (s_have_rise && !s_have_fall) { s_fall = t; s_have_fall = 1; }
}

static void setup(int trig, int echo)
{
    static int installed;
    if (s_echo >= 0 && s_ok) gpio_isr_handler_remove(s_echo);
    s_ok = 0; s_trig = trig; s_echo = echo;
    if (trig < 0 || echo < 0 || trig > 33 || echo > 39 || (trig >= 6 && trig <= 11) || (echo >= 6 && echo <= 11)) { ESP_LOGE(TAG, "bad pins trig %d echo %d", trig, echo); return; }
    gpio_reset_pin(trig); gpio_set_direction(trig, GPIO_MODE_INPUT_OUTPUT); gpio_set_level(trig, 0);
    gpio_reset_pin(echo); gpio_set_direction(echo, GPIO_MODE_INPUT); gpio_set_pull_mode(echo, GPIO_PULLUP_ONLY);
    if (!installed) {
        esp_err_t e = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service failed"); return; }
        installed = 1;
    }
    if (gpio_set_intr_type(echo, GPIO_INTR_ANYEDGE) != ESP_OK || gpio_isr_handler_add(echo, echo_isr, (void *)(intptr_t)echo) != ESP_OK) { ESP_LOGE(TAG, "echo interrupt failed"); return; }
    gpio_intr_disable(echo);
    s_ok = 1;
    ESP_LOGI(TAG, "trig %d echo %d, echo interrupt on core %d", trig, echo, (int)xPortGetCoreID());
}

void sonar_service(void)
{
    int t = s_req_trig, e = s_req_echo;
    if (t >= 0 && (t != s_trig || e != s_echo)) setup(t, e);
}

int sonar_pulse(int trig, int echo, int timeout_us)
{
    if (trig != s_trig || echo != s_echo) {
        s_req_trig = trig; s_req_echo = echo;
        if (xPortGetCoreID() != APP_CORE) return -1;       // the application core sets it up (next sonar_service() call)
        setup(trig, echo);
    }
    if (!s_ok) return -1;
    if (timeout_us <= 0) timeout_us = 30000;
    s_have_rise = s_have_fall = 0;
    gpio_intr_enable(echo);
    gpio_set_level(trig, 0); esp_rom_delay_us(4);
    gpio_set_level(trig, 1); esp_rom_delay_us(10);
    gpio_set_level(trig, 0);
    int64_t t0 = esp_timer_get_time();
    int r = -1;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (s_have_fall) { r = (int)(s_fall - s_rise); break; }
        if (s_have_rise) { if (now - s_rise > 60000) { r = -2; break; } }
        else if (now - t0 > timeout_us) break;
        esp_rom_delay_us(100);
    }
    gpio_intr_disable(echo);
    return r;
}
