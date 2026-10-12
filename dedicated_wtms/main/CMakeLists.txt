file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(
    SRCS "main.c" "net.c" "timex.c" "fsx.c" "mqttc.c" "sonar.c" "compat.c" "wtms_core.c" "web.c" "klog.c" "ssh_log.c" "ota.c" "auth.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/generated"
    EMBED_FILES "../web/index.html" "../web/config.html" "../web/login.html" "../web/tank.css" "../web/tank.js" "../web/chart.js" "../web/style.css" "../web/ui.js"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer vfs lwip esp_driver_gpio mbedtls esp_http_server app_update esp_app_format mqtt esp_hw_support esp_system esp_rom)

# ---- Build-time credentials: read from the parent project's .creds/ (read only; the same Wi-Fi, SSH password and SSH host key as miniesp) ----
set(CREDS_DIR "${CMAKE_SOURCE_DIR}/../.creds")
function(read_cred file out_var)
    set(v "")
    if(EXISTS "${CREDS_DIR}/${file}")
        file(READ "${CREDS_DIR}/${file}" v)
        string(STRIP "${v}" v)
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CREDS_DIR}/${file}")
    endif()
    string(REPLACE "\\" "\\\\" v "${v}")
    string(REPLACE "\"" "\\\"" v "${v}")
    set(${out_var} "${v}" PARENT_SCOPE)
endfunction()
read_cred(wifi_ssid          C_WIFI_SSID)
read_cred(wifi_password      C_WIFI_PASS)
read_cred(esp32_ssh_password C_SSH_PASS)
if(NOT EXISTS "${CREDS_DIR}/esp32_hostkey.der")
    message(FATAL_ERROR "dedicated_wtms: ${CREDS_DIR}/esp32_hostkey.der is missing (build the miniesp project once, or create the SSH host key there)")
endif()
configure_file("${CREDS_DIR}/esp32_hostkey.der" "${CMAKE_BINARY_DIR}/generated/hostkey.der" COPYONLY)
file(WRITE "${CMAKE_BINARY_DIR}/generated/creds.h"
"#pragma once\n"
"#define CRED_WIFI_SSID \"${C_WIFI_SSID}\"\n"
"#define CRED_WIFI_PASS \"${C_WIFI_PASS}\"\n"
"#define CRED_SSH_USER  \"esp\"\n"
"#define CRED_SSH_PASS  \"${C_SSH_PASS}\"\n")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/generated/hostkey.der" BINARY)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WOLFSSL_USER_SETTINGS WOLFSSH_USER_SETTINGS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-unused-function)

# ---- wolfSSH half-close fix (same idempotent patch as miniesp, applied to this project's own managed component copy) ----
set(WOLFSSH_INTERNAL "${CMAKE_SOURCE_DIR}/managed_components/wolfssl__wolfssh/src/internal.c")
if(EXISTS "${WOLFSSH_INTERNAL}")
    file(READ "${WOLFSSH_INTERNAL}" _wssh_src)
    if(NOT _wssh_src MATCHES "DEDICATED_WTMS_PATCH_NO_AUTO_EOF")
        string(REPLACE
"        channel->eofRxd = 1;
        if (!channel->eofTxd) {
            ret = SendChannelEof(ssh, channel->peerChannel);
        }
"
"        channel->eofRxd = 1;
        /* DEDICATED_WTMS_PATCH_NO_AUTO_EOF: keep the sending side open after the peer's EOF (half-close) */
"
            _wssh_patched "${_wssh_src}")
        if(NOT _wssh_patched STREQUAL _wssh_src)
            file(WRITE "${WOLFSSH_INTERNAL}" "${_wssh_patched}")
            message(STATUS "dedicated_wtms: patched wolfSSH DoChannelEof")
        endif()
    endif()
endif()
