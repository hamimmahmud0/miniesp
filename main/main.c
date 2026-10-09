// esp32-unix: ESP-IDF firmware exposing a unix-like shell over SSH, running WAMR AOT programs from LittleFS.
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "aot_run.h"
#include "fs.h"
#include "ssh_server.h"
#include "mqtt.h"
#include "ota.h"
#include "svc.h"
#include "timesync.h"
#include "tty_vfs.h"
#include "wifi_mgr.h"
#include "www.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    fs_mount();
    tty_vfs_register();
    if (!aot_init()) ESP_LOGE(TAG, "WASM runtime unavailable: programs will not run");
    wifi_mgr_start();          // STA with predefined SSID, else AP
    time_init();               // restore the saved clock, TZ, start NTP (needs the network stack)
    ota_healthcheck_start();   // confirms a freshly updated image once the system is up
    svc_init();                // service manager: starts sshd, www, mdns and the enabled unit files
    ESP_LOGI(TAG, "up. ssh esp@<ip> (see 'ifconfig' in the shell)");
}
