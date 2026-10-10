file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(
    SRCS "main.c" "io.c" "auth.c" "fs.c" "wifi_mgr.c" "term.c" "tty_vfs.c" "aot_run.c" "shell.c" "ssh_server.c" "drivers.c" "www.c" "svc.c" "wd.c" "ota.c" "timesync.c" "kv.c" "mqtt.c" "httpget.c" "bench.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/generated"
    EMBED_FILES "${CMAKE_SOURCE_DIR}/fs_image/bin/pkg.aot"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer vfs lwip esp_driver_gpio mbedtls esp_http_server esp_adc esp_driver_ledc esp_driver_i2c esp_driver_dac esp_driver_uart esp_driver_spi esp_driver_pcnt driver app_update esp_app_format mqtt esp_hw_support esp_http_client)

# ---- Build-time credentials from <project>/.creds (gitignored). Missing file => empty string. ----
set(CREDS_DIR "${CMAKE_SOURCE_DIR}/.creds")
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
read_cred(esp32_ap_password  C_AP_PASS)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

# SSH host key: generated once into .creds/ (stable across reflashes), embedded in the firmware.
if(NOT EXISTS "${CREDS_DIR}/esp32_hostkey.der")
    file(MAKE_DIRECTORY "${CREDS_DIR}")
    execute_process(COMMAND openssl ecparam -name prime256v1 -genkey -noout -outform DER
                    -out "${CREDS_DIR}/esp32_hostkey.der" RESULT_VARIABLE hk_rc)
    if(NOT hk_rc EQUAL 0)
        message(FATAL_ERROR "could not generate SSH host key with openssl")
    endif()
    file(CHMOD "${CREDS_DIR}/esp32_hostkey.der" PERMISSIONS OWNER_READ OWNER_WRITE)
endif()
configure_file("${CREDS_DIR}/esp32_hostkey.der" "${CMAKE_BINARY_DIR}/generated/hostkey.der" COPYONLY)
file(WRITE "${CMAKE_BINARY_DIR}/generated/creds.h"
"#pragma once\n"
"#define CRED_WIFI_SSID \"${C_WIFI_SSID}\"\n"
"#define CRED_WIFI_PASS \"${C_WIFI_PASS}\"\n"
"#define CRED_SSH_USER  \"esp\"\n"
"#define CRED_SSH_PASS  \"${C_SSH_PASS}\"\n"
"#define CRED_AP_SSID   \"miniesp\"\n"
"#define CRED_AP_PASS   \"${C_AP_PASS}\"\n")

# ---- Pack fs_image (programs in /bin, /etc ...) into the LittleFS partition ----
littlefs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/fs_image" FLASH_IN_PROJECT)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WOLFSSL_USER_SETTINGS WOLFSSH_USER_SETTINGS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/generated/hostkey.der" BINARY)


# Link-time hook used by aot_run.c (linear-memory fallback into the IRAM pool)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=os_mmap" "-Wl,--wrap=os_munmap")

# ---- wolfSSH half-close fix (applied to the managed component, idempotent) ----
# wolfSSH answers a client channel-EOF (e.g. `ssh host cmd < file`) with its own EOF straight away, after which
# OpenSSH discards everything the command prints. A half-close must not end the server's output: drop the
# automatic reply; EOF/close are still sent when the session really ends (wolfSSH_shutdown).
set(WOLFSSH_INTERNAL "${CMAKE_SOURCE_DIR}/managed_components/wolfssl__wolfssh/src/internal.c")
if(EXISTS "${WOLFSSH_INTERNAL}")
    file(READ "${WOLFSSH_INTERNAL}" _wssh_src)
    if(NOT _wssh_src MATCHES "ESP32_UNIX_PATCH_NO_AUTO_EOF")
        string(REPLACE
"        channel->eofRxd = 1;
        if (!channel->eofTxd) {
            ret = SendChannelEof(ssh, channel->peerChannel);
        }
"
"        channel->eofRxd = 1;
        /* ESP32_UNIX_PATCH_NO_AUTO_EOF: keep the sending side open after the peer's EOF (half-close) */
"
            _wssh_patched "${_wssh_src}")
        if(NOT _wssh_patched STREQUAL _wssh_src)
            file(WRITE "${WOLFSSH_INTERNAL}" "${_wssh_patched}")
            message(STATUS "miniesp: patched wolfSSH DoChannelEof (no automatic EOF reply)")
        else()
            message(WARNING "miniesp: wolfSSH DoChannelEof patch did not apply (library layout changed?)")
        endif()
    endif()
endif()
