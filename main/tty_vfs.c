#include "tty_vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs.h"
#include "term.h"

// Local fd numbers are handed out per open(); all map to the single current terminal.
#define MAX_FDS 8
static bool s_used[MAX_FDS];

static int tty_open(const char *path, int flags, int mode)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (!s_used[i]) { s_used[i] = true; return i; }
    errno = EMFILE;
    return -1;
}

static int tty_close(int fd)
{
    if (fd >= 0 && fd < MAX_FDS) s_used[fd] = false;
    return 0;
}

static ssize_t tty_write(int fd, const void *data, size_t size)
{
    term_t *t = term_current();
    if (!t || t->closed) { errno = EIO; return -1; }
    return term_write(t, data, size) == 0 ? (ssize_t)size : -1;
}

static ssize_t tty_read(int fd, void *dst, size_t size)
{
    term_t *t = term_current();
    if (!t || t->closed) return 0;
    int r = term_read_stdin(t, dst, size);
    if (r < 0) { errno = EINTR; return -1; }
    return r;
}

static int tty_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0620;
    return 0;
}

static int tty_fcntl(int fd, int cmd, int arg)
{
    if (cmd == F_GETFL) return O_RDWR;
    return 0;
}

bool tty_vfs_register(void)
{
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = &tty_open,
        .close = &tty_close,
        .write = &tty_write,
        .read = &tty_read,
        .fstat = &tty_fstat,
        .fcntl = &tty_fcntl,
    };
    return esp_vfs_register(TTY_PATH, &vfs, NULL) == ESP_OK;
}
