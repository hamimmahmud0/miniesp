#include "fs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

bool fs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_BASE,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t e = esp_vfs_littlefs_register(&conf);
    if (e != ESP_OK) { ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(e)); return false; }
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used", FS_BASE, (unsigned)used, (unsigned)total);
    mkdir(FS_BASE "/bin", 0777);
    mkdir(FS_BASE "/etc", 0777);
    mkdir(FS_BASE "/tmp", 0777);
    mkdir(FS_BASE FS_HOME, 0777);                    // home directory (~)
    mkdir(FS_BASE FS_HOME "/.local", 0777);
    mkdir(FS_BASE FS_HOME "/.local/bin", 0777);      // user-installed programs
    return true;
}

void fs_resolve(const char *cwd, const char *arg, char *out, size_t n)
{
    char tmp[256];
    if (arg[0] == '~' && (arg[1] == '/' || arg[1] == 0)) snprintf(tmp, sizeof tmp, "%s%s", FS_HOME, arg + 1);   // ~ -> /esp
    else if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);

    // Normalise: collapse //, handle . and ..
    char *parts[32]; int np = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np < 32) parts[np++] = t;
    }
    size_t o = 0;
    out[o++] = '/';
    for (int i = 0; i < np && o < n - 1; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 1 >= n) break;
        memcpy(out + o, parts[i], l); o += l;
        if (i != np - 1) out[o++] = '/';
    }
    out[o] = 0;
}

void fs_host_path(const char *vpath, char *out, size_t n)
{
    if (!strcmp(vpath, "/")) snprintf(out, n, "%s", FS_BASE);
    else snprintf(out, n, "%s%s", FS_BASE, vpath);
}
