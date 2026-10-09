// touch FILE...: create empty files that do not exist (there are no file timestamps on this filesystem).
// -c: do not create. Other options are ignored.
#include "mini.h"

int main(int argc, char **argv)
{
    int nocreate = 0, rc = 0, files = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { if (argv[i][1] == 'c') nocreate = 1; continue; }
        files++;
        if (sys_stat(argv[i]) > 0 || nocreate) continue;      // exists (file or directory), or -c
        int fd = sys_open(argv[i], 2);
        if (fd < 0) { m_eprintf("touch: cannot create '%s'\n", argv[i]); rc = 1; continue; }
        sys_close(fd);
    }
    if (!files) { m_eputs("usage: touch [-c] FILE...\n"); return 1; }
    return rc;
}
