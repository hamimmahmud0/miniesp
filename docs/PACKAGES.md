# Write, build and publish a package (about 10 minutes)

A package is a freestanding C program compiled to WebAssembly, then AOT-compiled for the ESP32 (WAMR), listed in a *journal*.
`pkg install NAME` downloads it, checks its size and SHA-256 and the firmware's syscall ABI, and puts it in `~/.local/bin`.

## 1. Write it
Copy `programs/hello.c` (or `wc.c`, `sonar.c`). No libc: `#include "mini.h"` gives the syscalls (`sys_open`, `sys_read`, `sys_tcp_connect`,
`sys_gpio_*`, `sys_kv_*`, `sys_http_get`, `sys_ping` ...) and small helpers (`m_printf`, `m_snprintf`, `m_atoi`).
```c
#include "mini.h"
int main(int argc, char **argv)
{
    m_printf("hello %s\n", argc > 1 ? argv[1] : "world");
    return 0;
}
```
Limits: one 64 KB memory (8 KB of it is the stack), so keep buffers small; a program cannot start another program; Ctrl-C works
whenever the program calls a `sys_*` function. Keep a program that a service or the web server runs near 40 KB.

## 2. Build it
```
cd programs
OUT=../pkgs ./build.sh mytool.c        # -> pkgs/mytool.aot   (without OUT it goes to fs_image/bin = shipped inside the firmware image)
```
Needs wasi-sdk and a `wamrc` with the Xtensa backend (README, "Toolchain"). Test it on a board first:
`ssh esp@HOST "put ~/.local/bin/mytool.aot" < pkgs/mytool.aot`, then run `mytool` there.

## 3. List it in the journal
```
ABI=3 tools/mkjournal.sh pkgs https://raw.githubusercontent.com/OWNER/REPO/main/pkgs 1.0 | grep mytool >> journals/main.journal
```
`ABI` is the lowest `sys_abi()` your program needs (the syscall list in `programs/mini.h` says in which ABI each call appeared).
A journal line: `pkg NAME VERSION URL sha256=HEX size=BYTES abi=N`.

## 4. Publish
Commit and push. GitHub's raw URLs are cached for about 5 minutes, so wait before testing. On the board:
```
pkg update
pkg install mytool
```
If you replace a file at the same URL, regenerate its journal line (the hash and size must match, otherwise `pkg install` refuses it).

## Your own journal
A journal can link to other journals with `journal URL` lines, and the board reads `/etc/pkg/sources.list` (one URL per line).
Host a journal and its files anywhere that serves plain HTTPS files, then add its URL to `sources.list` on the board.
The first journal that lists a name wins.

## Several files in one package (a bundle)
For a program plus web pages, config or a service, write `packages/NAME/bundle.spec` (see `packages/wtms/bundle.spec`) and run
`tools/mkbundle.py packages/NAME`. It builds the program, hashes every file, writes `pkgs/NAME/MANIFEST` and the `bundle` journal line.
`pkg install NAME` fetches the manifest, validates it, then downloads and verifies each file. Paths must be under `/esp/`, `/www/`, `/etc/` or `/var/`.
