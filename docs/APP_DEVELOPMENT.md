# Application development for miniesp

A miniesp application is a **`.aot` file**: a small C program compiled to WebAssembly, then ahead-of-time (AOT) compiled to native
Xtensa code for the ESP32 with WAMR. You write ordinary `int main(int argc, char **argv)` C, talk to the OS through `sys_*`
calls declared in `programs/mini.h`, and install the result on a running board over SSH. **You never reflash the firmware to
add or change an application.**

```
  hello.c --clang (wasm32, freestanding)--> hello.wasm --wamrc (xtensa)--> hello.aot --ssh put--> /esp/.local/bin/hello.aot
```

Contents: [1. Getting started](#1-getting-started) · [2. Installing the toolchain](#2-installing-the-toolchain) ·
[3. Hello world](#3-hello-world) · [4. How a program runs](#4-how-a-program-runs) · [5. mini.h reference](#5-minih-reference) ·
[6. Worked examples](#6-worked-examples) · [7. Other ways to run a program](#7-other-ways-to-run-a-program-shell-pipes-web-services) ·
[8. Packaging and publishing](#8-packaging-and-publishing) · [9. Limits and gotchas](#9-limits-and-gotchas) ·
[10. Debugging](#10-debugging) · [11. Adding a syscall](#11-adding-a-syscall-firmware-change)

## 1. Getting started

You need:

| Need | Why |
|---|---|
| A miniesp board (classic ESP32, 4 MB flash) running the firmware | the place programs run. See `README.md` / `llms.txt` for flashing. |
| SSH access to it (`ssh esp@esp-minix.local`, default password `esp32`, change it with `passwd`) | installing and running programs |
| A Linux host with **wasi-sdk** (clang) and an **Xtensa-capable `wamrc`** | building programs, see section 2 |

Check the board first:

```
ssh esp@esp-minix.local "uname"          # prints the hostname, version and ESP-IDF
ssh esp@esp-minix.local "hello"          # runs the preinstalled hello program
ssh esp@esp-minix.local "ls /bin"        # the preinstalled programs
```

Programs are searched in **`~/.local/bin`** (`/esp/.local/bin`, yours, searched first) and then **`/bin`** (system programs shipped in
the firmware image). The file name without `.aot` is the command name.

OS programs (`sh`, `pkg`, `nano`, `touch`, `ping`, `btop`, `hello`...) are in the `miniesp` repo, in `programs/`. Non-OS packages
(`nmap`, `curl`, `neofetch`, `wtms`) are in the separate `miniesp-pkg` repo and are good extra examples to read.

## 2. Installing the toolchain

One-time, on your development machine. Build both tools once; they are outside the repo (`~/esp/...` is what `programs/build.sh` expects).

**wasi-sdk** (only the clang binary is used; programs are freestanding, no libc):

```
mkdir -p ~/esp/tools/wasi && cd ~/esp/tools/wasi
# download wasi-sdk-34.0-x86_64-linux.tar.gz from https://github.com/WebAssembly/wasi-sdk/releases (tag wasi-sdk-34)
tar xzf wasi-sdk-34.0-x86_64-linux.tar.gz        # -> ~/esp/tools/wasi/wasi-sdk-34.0-x86_64-linux/bin/clang
```

**`wamrc` with the Xtensa backend.** The prebuilt `wamrc` releases do **not** contain Xtensa. Build it from source, from the same WAMR
tag as the firmware runtime (the AOT file format must match: currently **WAMR-2.4.5**). About 40 minutes and 8+ GB of RAM:

```
git clone --depth 1 --branch WAMR-2.4.5 https://github.com/bytecodealliance/wasm-micro-runtime.git ~/esp/wamr
pip install requests ninja
cd ~/esp/wamr/build-scripts
CXXFLAGS="-include cstdint" python3 build_llvm.py --platform xtensa --arch X86 Xtensa     # -include cstdint is needed with GCC >= 14
cd ../wamr-compiler && mkdir build && cd build && cmake .. && make -j8                    # use make, not ninja
~/esp/wamr/wamr-compiler/build/wamrc --target=help | grep xtensa                          # must list xtensa
```

If you install elsewhere, set `WASI_SDK=/path/to/wasi-sdk-34.0-x86_64-linux` and `WAMRC=/path/to/wamrc`.

Then get the repo (it provides `mini.h` and `build.sh`):

```
git clone https://github.com/hamimmahmud0/miniesp && cd miniesp/programs
```

You do **not** need ESP-IDF to write applications, only to build the firmware itself.

## 3. Hello world

Create `programs/hello2.c`:

```c
// hello2: prints a greeting plus its arguments.
#include "mini.h"

int main(int argc, char **argv)
{
    m_puts("Hello from miniesp!\n");
    for (int i = 0; i < argc; i++)
        m_printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
```

Build it (the `.aot` is written to `../fs_image/bin/` unless you set `OUT`):

```
cd programs
OUT=/tmp/out ./build.sh hello2.c           # hello2   wasm  1.4 KB  ->  aot  3.4 KB
```

Install it on the board and run it. `put` reads the file from stdin, so there is no scp:

```
ssh esp@esp-minix.local "put ~/.local/bin/hello2.aot" < /tmp/out/hello2.aot
ssh esp@esp-minix.local "hello2 one two"
```

You can also log in and run it from the prompt: `hello2 one two`. The return value of `main` is the command's exit status
(`ssh host cmd; echo $?`). To remove it: `rm ~/.local/bin/hello2.aot`.

Programs you put in `fs_image/bin/` (the default `OUT`) become **preinstalled**: they are packed into the LittleFS image by
`idf.py build` and shipped with the firmware (this needs a firmware build and OTA/flash, so use `~/.local/bin` while developing).

What `build.sh` does (you rarely need to touch it):

```
clang --target=wasm32 -nostdlib -ffreestanding -fno-builtin -Oz -z stack-size=8192 \
      -Wl,--no-entry -Wl,--export=esp_main -Wl,--gc-sections -Wl,--strip-all \
      -Wl,--initial-memory=65536 -Wl,--max-memory=65536  -o X.wasm X.c
wamrc --target=xtensa --opt-level=2 --size-level=0 -o X.aot X.wasm
```

Do not change `--size-level=0`: other levels produce an AOT that crashes at its first function on Xtensa.
Environment knobs: `OUT=dir`, `CFLAGS_EXTRA="-DNAME -I..."`, `WASI_SDK`, `WAMRC`, `WAMRC_FLAGS`.

## 4. How a program runs

- **Freestanding C.** No libc, no WASI, no `printf`, `malloc`, `stdio`, `string.h`. `mini.h` gives you `m_printf`, `m_snprintf`, `m_atoi`,
  `m_strcmp`, `m_strlen`, and `memcpy/memmove/memset/memcmp`. Everything else is a `sys_*` call into the OS.
- **Entry point.** `mini.h` defines the exported `esp_main()`, which builds `argc`/`argv` (up to 32 arguments, 1280 bytes of text in
  total) and calls your `main`. Include `mini.h` in exactly one `.c` file: it defines functions, so it is not a normal header.
- **Memory.** Each program gets **one 64 KB linear memory**: 8 KB is the stack, the rest holds globals and whatever your code
  uses. There is no heap: use fixed-size buffers on the stack or in `static` arrays. Pointers you pass to `sys_*` must point
  into this memory (the OS checks them). Keep buffers small (a 100-byte line buffer is fine, a 20 KB array is not).
- **One program at a time.** A run lock serializes SSH commands, web CGI requests and services. A second request waits (SSH up to
  10 s, web about 1 s, then HTTP 503). A program may run **one nested command** with `sys_run`.
- **Ctrl-C** stops a program the next time it calls any `sys_*` function. A loop that never calls the OS cannot be interrupted.
  Call `sys_sigint(1)` to take over: Ctrl-C then no longer kills the program, and you poll `sys_sigint(0)` (see 6.3).
- **Size matters.** The `.aot` file and its code sit in RAM while running. Typical programs are 3-20 KB. A program that
  a service or the web server runs should stay around 40 KB (bigger ones can fail to load while an SSH session is open; ship a
  reduced build with `CFLAGS_EXTRA=-DNAME` and `#ifdef`).
- **Integers only (practically).** Write fixed-point (`distance_mm`, centi-degrees) instead of relying on floating-point formatting:
  `m_printf` has no `%f`. Split values into integer and fraction yourself (`m_printf("%d.%d", v / 10, v % 10)`).

### Standard streams and pipes

| fd | meaning |
|---|---|
| 0 | stdin: on a terminal, **one edited line per `sys_read`**; in a pipe or `< file` it is the data stream; 0 bytes = EOF |
| 1 | stdout (the terminal, a pipe, or `> file`) |
| 2 | stderr |
| 3+ | streams you open: files (`sys_open`), sockets, in-RAM streams |

A program that reads fd 0 when no file is given and writes fd 1 works in pipelines for free: `cat notes | mycat | grep todo`.
Use `m_eprintf` for errors so they do not pollute a pipe. `sys_isatty(fd)` tells you whether output is a terminal (use it to decide about colors).

## 5. mini.h reference

All calls return a negative number or `-1` on failure unless stated. Strings are NUL-terminated. The comment next to each
declaration in `programs/mini.h` is the authoritative one-liner; this section groups them and adds the details.

### 5.1 Output and helpers (no syscall needed)

| Helper | Description |
|---|---|
| `m_puts(s)`, `m_eputs(s)` | write a string to stdout / stderr |
| `m_write(fd, buf, n)` | write all `n` bytes (loops on short writes) |
| `m_printf(fmt, ...)`, `m_eprintf(fmt, ...)` | formatted output. Supports `%s %c %d %u %x %%`, with optional `0` and a width (`%02d`, `%5u`). No `%f`, `%l`, `%p`, no precision, no `-` flag |
| `m_snprintf(buf, n, fmt, ...)` | same formats into a buffer; returns the length it wanted, like `snprintf` |
| `m_vfprintf`, `m_vsnprintf` | `va_list` versions |
| `m_atoi(s)` | decimal to int, optional `-` |
| `m_strcmp(a, b)`, `m_strlen(s)` | as in libc |
| `memcpy`, `memmove`, `memset`, `memcmp` | defined by `mini.h` (the compiler may emit calls to them) |

### 5.2 Streams and files

| Call | Description |
|---|---|
| `sys_write(fd, buf, n)` / `sys_read(fd, buf, n)` | raw I/O; returns bytes (0 = EOF on read; `-1` error or, for sockets with a timeout, expiry) |
| `sys_open(path, mode)` | mode 0 read, 1 write/truncate, 2 append. Returns an fd (>= 3) or `-1`. Relative paths use the shell's current directory |
| `sys_close(fd)` | close a file or socket |
| `sys_seek(fd, off, whence)` | 0 set, 1 current, 2 end; returns the position (`sys_seek(fd, 0, 1)` = tell) |
| `sys_stat(path)` | 1 file, 2 directory, -1 missing |
| `sys_fsize(path)` | size in bytes, -1 missing |
| `sys_listdir(path, buf, n)` | names separated by newlines, a trailing `/` marks a directory |
| `sys_unlink`, `sys_mkdir`, `sys_rmdir`, `sys_rename(from, to)` | file management (`rename` replaces the target) |
| `sys_getcwd(buf, n)`, `sys_chdir(path)` | the program's working directory |
| `sys_membuf()`, `sys_rewind(fd)` | an in-RAM stream (what pipes are made of); `rewind` re-reads it from the start |
| `sys_isatty(fd)` | 1 if the fd is a terminal |

The filesystem is LittleFS (about 1.1 MB, shared by everything) with no timestamps and no permissions. Home is `/esp`, system
programs `/bin`, web root `/esp/www`, services `/etc/services`, scratch `/tmp`.

### 5.3 Arguments, processes and the terminal

| Call | Description |
|---|---|
| `sys_argc()`, `sys_arg(i, buf, n)` | what `argc`/`argv` are built from (you normally just use `main`'s) |
| `sys_run(blob, blob_len, argc, fd_in, fd_out, fd_err)` | run one command (built-in or program) and wait; the words are NUL-separated in `blob`. Returns its exit status. This is how `sh` is written. Only one level of nesting |
| `sys_sigint(op)` | 1 = catch Ctrl-C, 2 = stop catching, 0 = "was Ctrl-C pressed?" (returns nonzero and clears the flag) |
| `sys_getkey(ms)` | one raw key for full-screen programs: the byte, `-1` timeout, `-2` end of input, `-3` Ctrl-C. |
| `sys_sysinfo(SI_COLS)` / `SI_ROWS` | terminal size (from the client's pty) |

### 5.4 Time

| Call | Description |
|---|---|
| `sys_millis()`, `sys_micros()` | since boot. `micros` wraps after about 71 minutes: always subtract (`sys_micros() - t0`) |
| `sys_sleep_ms(ms)` | sleep; Ctrl-C-interruptible |
| `sys_delay_us(us)` | busy wait, up to 100 ms |
| `sys_time()` | epoch seconds, 0 if the clock was never set. There is no RTC: NTP sets it after WiFi comes up |
| `sys_time_state()` | 0 none, 1 restored/approximate (from before the reboot), 2 NTP-synced |
| `sys_localtime(epoch, out, bytes)` | fills `int out[8]`: sec, min, hour, mday, mon (0-11), year, wday, yday |
| `sys_tz(spec)` | set the time zone: `"+6"`, `"-5:30"` or a POSIX TZ string |

### 5.5 System information

| Call | Description |
|---|---|
| `sys_bench(kind,cores,ms,out,n)` (ABI 6) | native benchmark kernels pinned to core 0/1 (used by esp-bench); `SI_CORES`, `SI_CORE` in `sys_sysinfo` |
| `sys_sysinfo(SI_*)` | `SI_UPTIME_S, SI_DRAM_FREE, SI_DRAM_TOTAL, SI_DRAM_MINFREE, SI_DRAM_LARGEST, SI_IRAM8_FREE, SI_IRAM8_TOTAL, SI_EXEC_FREE, SI_EXEC_TOTAL, SI_FS_USED, SI_FS_TOTAL, SI_RSSI, SI_CPU_MHZ, SI_NTASKS, SI_COLS, SI_ROWS` |
| `sys_netinfo(what, buf, n)` | 0 hostname, 1 IP address, 2 SSID |
| `sys_version(buf, n)` | firmware version string |
| `sys_tasks(buf, n)` | fills `esp_task_t` records (`name, state, prio, stack, cpu10, pid`; `cpu10` = CPU% x 10, measured since the previous call). Used by `btop` |
| `sys_abi()` | the syscall ABI version the firmware implements (currently 5) |
| `sys_random()` | hardware random number |
| `sys_log(msg)` | write a line to the serial log |
| `sys_reboot()` | reboot the board |

### 5.6 Persistent key-value store (NVS)

| Call | Description |
|---|---|
| `sys_kv_get(key, buf, n)` | length, `-1` missing. Values are up to 1000 bytes |
| `sys_kv_set(key, buf, n)` | store bytes |
| `sys_kv_del(key)` | delete |
| `sys_kv_key(idx, buf, n)` | name of the idx-th key, `-1` past the end (to list everything) |

Keys survive reboots, OTA and `app-flash` (not `erase_flash`). Namespace your keys (`myapp.threshold`). Avoid writing every few seconds:
flash wears out. Use it for settings and small counters, files for logs.

### 5.7 Networking

| Call | Description |
|---|---|
| `sys_tcp_connect(host, port, timeout_ms)` | an fd usable with `sys_read`/`sys_write`/`sys_close`, `-1` on error. `host` can be a name or an address |
| `sys_tcp_listen(port)`, `sys_tcp_accept(lfd, timeout_ms)` | a server socket; `accept` returns a new fd, `0` on timeout, `-1` on error |
| `sys_sock_timeout(fd, ms)` | make `sys_read` return `-1` after `ms` without data (`0` = the peer closed) |
| `sys_udp_open(port)`, `sys_udp_sendto(fd, host, port, buf, n)` | UDP (port 0 = ephemeral); receive with `sys_read` |
| `sys_dns(host, buf, n)` | resolve to a dotted IPv4 string |
| `sys_http_get(url, path, max_bytes, timeout_ms)` | ABI 4: HTTP(S) GET saved to a file. Returns the byte count, `-1` network/TLS, `-2` file, `-3` too big, `-4` Ctrl-C, or `-STATUS` (`-404`) |
| `sys_ping(host, timeout_ms, seq, payload_bytes)` | ABI 5: one ICMP echo. Round trip in microseconds, `-1` timeout, `-2` cannot send, `-3` unreachable, `-4` Ctrl-C |
| `sys_mqtt_*` | see 5.8 |

`sys_tcp_listen` lets a program serve, but remember it holds the only runtime while it does.

### 5.8 MQTT

The firmware keeps one MQTT client and caches the latest message per subscribed topic, so programs poll instead of staying resident.

| Call | Description |
|---|---|
| `sys_mqtt_config(host, port, user, pass, client_id)` | set the broker (stored in NVS) |
| `sys_mqtt_info(what, buf, n)` | ABI 3: 0 broker host, 1 user, 2 client id, 3 port; never the password |
| `sys_mqtt_ctl(on)` | start (1) / stop (0) the native client |
| `sys_mqtt_state()` | 0 off, 1 connecting, 2 connected |
| `sys_mqtt_pub(topic, payload, n, retain, qos)` | publish |
| `sys_mqtt_sub(filter, qos)`, `sys_mqtt_unsub(filter)` | subscriptions persist across runs of your program |
| `sys_mqtt_get(topic, buf, n)` | latest payload length, `-1` none yet |
| `sys_mqtt_age(topic)` | milliseconds since that message arrived, `-1` never |

### 5.9 Hardware (GPIO, ADC, PWM, DAC, I2C, SPI, UART, sensors)

**Do not drive pins with real hardware attached unless you know what is wired.** Flash pins 6-11 are refused; GPIO34-39 are input-only;
ADC works on ADC1 (GPIO32-39) only, because ADC2 conflicts with WiFi. See `BOARDS.md` for pins used by the stock programs.

| Call | Description |
|---|---|
| `sys_gpio_mode(pin, out)` | 1 = output, 0 = input with pull-up |
| `sys_gpio_write(pin, level)`, `sys_gpio_read(pin)` | digital I/O |
| `sys_adc_read(pin)`, `sys_adc_mv(pin)` | raw 0..4095 (about 0-3.1 V) / calibrated millivolts |
| `sys_pwm(pin, freq_hz, duty_permille)` | duty 0..1000; a negative duty stops it |
| `sys_dac_write(pin, value)` | GPIO25/26, 0..255 |
| `sys_i2c_init(sda, scl, hz)` | `(0,0,0)` = SDA 21, SCL 22, 100 kHz (initialized automatically on first use) |
| `sys_i2c_probe(addr)` | 0 if a device answers |
| `sys_i2c_write/read(addr, buf, n)`, `sys_i2c_wr(addr, w, wn, r, rn)` | transfers; `wr` is write, restart, read |
| `sys_spi_open(sck, mosi, miso, hz, mode)`, `sys_spi_xfer(cs, tx, n, rx, n2)` | `n == n2 <= 64`; the same buffer can be used for both |
| `sys_uart_open(port, tx, rx, baud)`, `_write`, `_read(port, buf, n, timeout_ms)`, `_close` | UART 1 or 2 |
| `sys_pulse_in(pin, level, timeout_us)` | pulse width in us, `-1` timeout, `-2` too long |
| `sys_sonar_pulse(trig, echo, timeout_us)` | HC-SR04 echo width in us (distance in cm = us / 58). The echo pin is 5 V: use a voltage divider |
| `sys_ds18b20(pin)` | temperature in 1/100 degC, `-9999` error (blocks about 0.8 s) |
| `sys_pcnt_open(unit, pin)`, `sys_pcnt_read(unit)`, `sys_pcnt_clear(unit)` | hardware rising-edge counter, units 0/1 |

### 5.10 ABI versions

`sys_abi()` returns the firmware's syscall ABI. `programs/mini.h` documents which ABI each call appeared in (ABI 2 added most of the
above; MQTT info 3; `sys_http_get` 4; `sys_ping` 5). **An `.aot` that imports a syscall the firmware does not have fails to load.**
The import is resolved when the program loads, so a program that uses an ABI 5 call will not load at all on ABI 4 firmware. Update the firmware
(OTA) first, then deploy the programs. When you publish a package, record the lowest ABI it needs in its journal line (`abi=N`).

## 6. Worked examples

All of these compile with `build.sh` and use only the calls above.

### 6.1 A filter that works in pipes (`mycat.c`)

```c
// mycat [FILE...]: copy files (or stdin) to stdout, so it also works in pipes.
#include "mini.h"

int main(int argc, char **argv)
{
    int rc = 0, nfiles = argc > 1 ? argc - 1 : 1;
    for (int i = 1; i <= nfiles; i++) {
        int fd = argc > 1 ? sys_open(argv[i], 0) : 0;      // fd 0 = stdin
        if (fd < 0) { m_eprintf("mycat: %s: cannot open\n", argv[i]); rc = 1; continue; }
        char buf[128]; int n;
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) m_write(1, buf, (size_t)n);
        if (argc > 1) sys_close(fd);
    }
    return rc;
}
```

```
echo hi | mycat          mycat /etc/motd          mycat nosuchfile; echo $?     (prints the error on stderr, then 1)
```

### 6.2 State that survives reboots (`counter.c`)

```c
// counter [reset]: a run counter that survives reboots (NVS key "counter.runs").
#include "mini.h"

int main(int argc, char **argv)
{
    char buf[16];
    int n = sys_kv_get("counter.runs", buf, sizeof buf - 1);
    int runs = 0;
    if (n > 0) { buf[n] = 0; runs = m_atoi(buf); }
    if (argc > 1 && !m_strcmp(argv[1], "reset")) runs = 0; else runs++;
    int len = m_snprintf(buf, sizeof buf, "%d", runs);
    sys_kv_set("counter.runs", buf, len);
    m_printf("run #%d\n", runs);
    return 0;
}
```

### 6.3 A long-running program that stops cleanly on Ctrl-C (`tick.c`)

```c
// tick [N]: prints N lines, one per second; Ctrl-C ends it cleanly.
#include "mini.h"

int main(int argc, char **argv)
{
    int n = argc > 1 ? m_atoi(argv[1]) : 10;
    sys_sigint(1);                                  // Ctrl-C no longer kills us; poll for it
    for (int i = 1; i <= n; i++) {
        m_printf("tick %d\n", i);
        sys_sleep_ms(1000);
        if (sys_sigint(0)) { m_puts("interrupted\n"); return 130; }
    }
    return 0;
}
```

Use this pattern to restore a terminal, close a file or print a summary (`ping` and `sonar` do). `btop` uses `sys_getkey` instead, where Ctrl-C
arrives as the key `-3`.

### 6.4 A web endpoint (`hi.c`)

```c
// hi: a CGI program. Browse to  http://BOARD/cgi-bin/hi?name=Ada
#include "mini.h"

int main(int argc, char **argv)
{
    m_puts("Content-Type: text/plain\n\n");
    m_puts("hello");
    for (int i = 1; i < argc; i++) m_printf(" [%s]", argv[i]);
    m_puts("\n");
    return 0;
}
```

More worked programs to read: `hello.c` (arguments), `greet.c` (terminal input), `wc.c` (files and pipes), `blink.c` (GPIO),
`sonar.c` (timing a sensor, median filtering, JSON output, Ctrl-C), `ping.c` (a syscall plus a statistics summary), `nano.c` and `btop.c`
(full-screen terminal UIs with `sys_getkey`), `sh.c` (`sys_run` and pipes with `sys_membuf`), `pkg.c` (HTTP download, SHA-256, files).

## 7. Other ways to run a program: shell, pipes, web, services

**Shell and SSH.** `name args`, pipes (`a | b`), redirections (`< in`, `> out`, `>> out`, `2> err`, `2>&1`) and `;`, `&&`, `||` work in the
built-in shell. Only the built-in `grep`, `head` and `wc` can sit in a script run by `sh`, because `sh` itself holds the single runtime and cannot start `.aot`
children. `ssh host "cmd args"` runs one command and passes the exit status back; output starts with a banner line (`miniesp`).

**Web (CGI).** Put the `.aot` in `~/www/cgi-bin/NAME.aot` and request `http://BOARD/cgi-bin/NAME?arg1+arg2`.
- Query words are split on `+` and `&` and become `argv[1..]`. Only `A-Za-z0-9_.,:=@/-` is allowed (no percent-encoding); the request line is limited to about 200 characters.
- A browser cache-buster such as `&_=ts` becomes an extra argument: do not add it.
- Start the output with `Content-Type: ...` and a blank line (as in 6.4); otherwise it is served as plain text. JSON helpers: see `sysinfo.c`.
- If an SSH program is running, the request waits ~1 s and then gets HTTP 503. Keep CGI programs small (about 40 KB) and fast.
- Build straight into the web directory: `OUT=../fs_image/www/cgi-bin ./build.sh x.c`, or `put ~/www/cgi-bin/x.aot` to a running board.
- The web server has no authentication: do not expose it to untrusted networks.

**Services (background jobs).** A unit file in `/etc/services/NAME.service`:

```
[Service]
Description=Blink the on-board LED once every 5 s
ExecStart=/bin/blink 2 1
Interval=5
Enabled=false
```

Manage it with `service new NAME --every SEC -- cmd args`, then `service enable|start|stop|restart|status|log NAME` and `service list`.
A service program must be **short-lived**: it holds the one runtime while it runs, and a long-running daemon makes SSH sessions and
web requests wait. Prefer an interval service that does its work and exits, keeping long-lived state in `sys_kv_*` or files.

## 8. Packaging and publishing

To let other people install your program with `pkg install NAME`, publish it in a **journal** (a plain text index). The
full guide is `docs/PACKAGES.md`; in short:

```
OUT=../pkgs ./build.sh mytool.c
ABI=3 tools/mkjournal.sh pkgs https://raw.githubusercontent.com/OWNER/REPO/main/pkgs 1.0 | grep mytool >> journals/main.journal
git commit && git push           # then on the board:  pkg update && pkg install mytool
```

A journal line is `pkg NAME VERSION URL sha256=HEX size=BYTES abi=N`. `pkg install` verifies size, SHA-256 and the firmware ABI.
Packages with several files (web pages, config, a service) use a `bundle.spec` and `tools/mkbundle.py`.

Where does your code belong? **OS programs** (needed on every board: shell tools, `pkg`, `ping`, `nano`) live in `miniesp/programs/` and ship in
`fs_image/bin`. Everything else belongs in a **package repo** such as `miniesp-pkg`, which has its own `programs/build.sh`, journal and tools, and
is linked from the mother journal with a `journal URL` line.

## 9. Limits and gotchas

- **Memory is the constraint.** 64 KB per program (8 KB stack), no heap, about 70-90 KB of RAM free on the whole board at run time. Prefer small
  fixed buffers; avoid big `static` arrays.
- **One program at a time**, one nested command via `sys_run`. A program that runs a long time blocks SSH, web and services.
- **No libc**: no `printf("%f")`, `malloc`, `qsort`, `strtol`, `strcpy`... write the few helpers you need (copy small ones from existing programs).
- **Uninterruptible loops**: Ctrl-C only works when the program calls a `sys_*` function. Put a `sys_sleep_ms(1)` or other call in busy loops.
- **`micros()` wraps** after about 71 min; compare with subtraction.
- **Hardware safety**: pins may be wired to pumps and relays. Treat `sys_gpio_write`, `sys_pwm` and MQTT publishing as real actions; never use them
  as a casual test.
- **`--size-level=0`** must stay in the `wamrc` flags.
- **Large `.aot` files** may fail to load in CGI or a service while an SSH session is open: keep those near 40 KB.
- **NVS wear**: do not write a key every few seconds.
- **Clock**: there is no RTC; `sys_time()` is 0 or approximate until NTP syncs (`sys_time_state()`).
- **One SSH session at a time**, and uploads (`put`) run about 30 KB/s.

## 10. Debugging

- Print with `m_eprintf` to the terminal, or `sys_log("...")` to the **serial log** (115200 baud: `idf.py monitor`) when stdout is not a terminal (CGI, services).
- `service log NAME` shows what a service printed.
- "program could not be run: ..." means loading failed: wrong ABI (an unknown syscall), AOT/WAMR version mismatch (rebuild `wamrc` from the tag the
  firmware uses), or not enough free RAM (`free`; close other sessions, shrink the program). Check `uname` and `sys_abi`.
- A reboot right after running your program means it crashed the runtime or the watchdog fired: check the serial log.
- Free memory before and after many runs of a program should stay within about 1 KB; a drift means a leak in a native you call.
- Test Ctrl-C, bad arguments, missing files and the empty-input case, not only the happy path.

## 11. Adding a syscall (firmware change)

If your application needs something the OS does not offer (a new peripheral, say), a syscall is added in the firmware:

1. Implement the hardware function (drivers go in `main/drivers.c`).
2. In `main/aot_run.c` add `static int sys_xxx_(wasm_exec_env_t env, ...)` and a line in `s_natives[]`: `{ "sys_xxx", sys_xxx_, "(sig)ret" }`.
   Signature letters: `i` int32, `I` int64, `f`/`F` float/double, `$` NUL-terminated string (validated), `*` pointer (validated) followed by `~` = byte
   length of that buffer (`"(i*~)i"` is `(int fd, void *buf, int n)`).
3. Declare it in `programs/mini.h` with `SYS("sys_xxx") int sys_xxx(...);`, document it, and **bump `SYS_ABI`** in `main/aot_run.c`.
4. Build and OTA the firmware first (`tools/ota.sh HOST`), then rebuild and deploy the programs that use it.

Natives must not block for long without a way out (check for Ctrl-C the way `sys_sleep_ms_` does): Ctrl-C and the web timeout rely on it.
