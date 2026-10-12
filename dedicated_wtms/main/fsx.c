#include "fsx.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

static void rm_rf(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st)) return;
    if (S_ISDIR(st.st_mode) && depth < 6) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[160]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                rm_rf(sub, depth + 1);
            }
            closedir(d);
        }
        rmdir(path);
    } else remove(path);
}

// The flash may still hold the miniesp OS filesystem (programs, web pages, packages): this firmware needs none of it. The tank history is kept.
void fs_migrate_legacy(void)
{
    struct stat st;
    if (!stat(FS_BASE "/.dedicated", &st)) return;
    mkdir(FS_BASE "/tank", 0777);
    DIR *d = opendir(FS_BASE "/www/tank");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".csv")) continue;
            char from[160], to[160];
            snprintf(from, sizeof from, FS_BASE "/www/tank/%s", e->d_name);
            snprintf(to, sizeof to, FS_BASE "/tank/%s", e->d_name);
            struct stat ts;
            if (!stat(to, &ts)) snprintf(to, sizeof to, FS_BASE "/tank/%s.miniesp", e->d_name);       // this firmware already wrote its own: keep the old one aside
            if (rename(from, to) == 0) ESP_LOGI(TAG, "kept %s", e->d_name);
        }
        closedir(d);
    }
    static const char *const old[] = { "/bin", "/esp", "/etc", "/www", "/tmp", "/pkg" };
    for (unsigned i = 0; i < sizeof old / sizeof *old; i++) { char p[40]; snprintf(p, sizeof p, FS_BASE "%s", old[i]); if (!stat(p, &st)) { ESP_LOGI(TAG, "removing %s (miniesp OS file)", old[i]); rm_rf(p, 0); } }
    FILE *f = fopen(FS_BASE "/.dedicated", "w");
    if (f) { fputs("dedicated_wtms\n", f); fclose(f); }
}

bool fs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = { .base_path = FS_BASE, .partition_label = "storage", .format_if_mount_failed = true, .dont_mount = false };
    esp_err_t e = esp_vfs_littlefs_register(&conf);
    if (e != ESP_OK) { ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(e)); return false; }
    mkdir(FS_BASE "/tank", 0777);
    mkdir(FS_BASE "/log", 0777);
    return true;
}
