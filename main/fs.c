#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

// pkg (the package manager) is part of the firmware: it is written to /bin when missing, so a wiped or reformatted
// filesystem can always be rebuilt with "pkg update; pkg fix". (fs_image/bin/pkg.aot is embedded at build time.)
extern const uint8_t pkg_aot_start[] asm("_binary_pkg_aot_start");
extern const uint8_t pkg_aot_end[] asm("_binary_pkg_aot_end");
static const char DEFAULT_SOURCES[] =
    "# pkg sources: one journal URL per line (the first one is the default \"mother\" journal).\n"
    "https://raw.githubusercontent.com/hamimmahmud0/miniesp/main/journals/main.journal\n";

static void write_if_missing(const char *path, const void *data, size_t n)
{
    struct stat st;
    if (!stat(path, &st) && st.st_size > 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot create %s", path); return; }
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    if (w != n) { remove(path); ESP_LOGW(TAG, "short write to %s", path); }
    else ESP_LOGI(TAG, "restored %s (%u bytes)", path, (unsigned)n);
}

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
    mkdir(FS_BASE "/etc/services", 0777);
    mkdir(FS_BASE "/etc/pkg", 0777);
    mkdir(FS_BASE "/var", 0777);
    mkdir(FS_BASE "/var/pkg", 0777);                 // package index cache
    mkdir(FS_BASE "/www", 0777);                     // web root
    mkdir(FS_BASE "/www/cgi-bin", 0777);
    mkdir(FS_BASE FS_HOME, 0777);                    // home directory (~)
    mkdir(FS_BASE FS_HOME "/.local", 0777);
    mkdir(FS_BASE FS_HOME "/.local/bin", 0777);      // user-installed programs
    write_if_missing(FS_BASE "/bin/pkg.aot", pkg_aot_start, pkg_aot_end - pkg_aot_start);
    write_if_missing(FS_BASE "/etc/pkg/sources.list", DEFAULT_SOURCES, sizeof DEFAULT_SOURCES - 1);
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
