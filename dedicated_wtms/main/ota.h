#pragma once
#include "esp_http_server.h"
// Over-the-air update: POST /ota?sha=<sha256 hex of the image> with the .bin as the body, header  X-Ota-Key: <SSH password>  (or X-Token: <dashboard session>).
// tools/ota.sh does it. The new image must prove itself within 60 s of booting (Wi-Fi up and the control loop running) or the old one comes back.
esp_err_t ota_http_handler(httpd_req_t *req);
void ota_healthcheck_start(void);
