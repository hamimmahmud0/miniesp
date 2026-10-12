// dedicated_wtms: a firmware that only runs the water tank manager.
//   core 1: the tank controller (sonar echo interrupt, control loop)
//   core 0: Wi-Fi / TCP/IP, MQTT, dashboard + API, SSH log viewer, OTA, log files
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "fsx.h"
#include "klog.h"
#include "mqttc.h"
#include "net.h"
#include "ota.h"
#include "ssh_log.h"
#include "timex.h"
#include "web.h"
#include "wtms_core.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init()); }
    fs_mount();
    klog_init();               // log -> /lfs/log/kernel.log + kernel.1, live view for SSH, finished files go out over MQTT
    net_start();               // Wi-Fi (reconnects forever) + mDNS
    time_init();               // saved time until SNTP answers
    mqtt_start();
    wtms_start();              // controller on core 1
    web_start();
    ssh_log_start();
    ota_healthcheck_start();
    ESP_LOGI(TAG, "dedicated_wtms %s up: controller on core 1, system on core 0", "1.0.0");
}
