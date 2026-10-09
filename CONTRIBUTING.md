# Contributing to miniesp

Thanks for helping! miniesp is a student-run project, so reviews may take a few days.

## Quick start
1. Read `README.md` and `llms.txt` (full build and flashing guide).
2. Pick an issue labelled `good first issue`, and comment that you are taking it.
3. Build the firmware: `idf.py build` (ESP-IDF v5.3.x). Build a program: `cd programs && ./build.sh NAME.c`
   (needs wasi-sdk and a `wamrc` with the Xtensa backend, see "Toolchain" in the README).
4. Test on a real ESP32 and say which board and which commands you ran in the pull request.
5. Keep programs small: freestanding C, no libc, `#include "mini.h"`. Do not change `--size-level=0` in `programs/build.sh`
   (other levels crash the AOT code on Xtensa).

## Pull requests
- One change per pull request. Describe what you tested on hardware and what you could only compile.
- Match the surrounding code (naming, comment density, idiom).
- Update the README section for any user-visible command, and `programs/mini.h` for any new syscall.
  A new syscall also bumps `SYS_ABI` in `main/aot_run.c`; programs that use it declare the ABI in their journal entry (`abi=N`).
- Never commit secrets: `.creds/` holds Wi-Fi and SSH credentials and is gitignored.
- Do not trigger real outputs (GPIO, PWM, relays over MQTT) as a "test" on someone else's hardware.
- By contributing you agree that your work is licensed GPL-3.0 (the project links wolfSSL/wolfSSH under GPLv3).

## Memory is the constraint
About 283 KB of DRAM is usable and 70-90 KB is free at run time. Each program gets one 64 KB WebAssembly memory
(including its 8 KB stack), only one program runs at a time, and a program cannot start another program.
Check `free` before and after your change and mention the numbers if they moved.
A program loaded by the web server (CGI) or a service should stay around 40 KB: bigger ones can fail to load while an SSH session is open.

## Package authors
A package is a freestanding C file built to a small `.aot` file and listed in a journal.
Start with `docs/PACKAGES.md` (write, build, publish in about 10 minutes). Several files per package: "Bundle packages" in the README
and `tools/mkbundle.py`. Single programs: `tools/mkjournal.sh`.

## Porting to another board
Port checklist:
1. Set the target in `sdkconfig.defaults` and delete `sdkconfig` before building (defaults only apply to a fresh `sdkconfig`).
2. Rebuild `wamrc` for the new CPU (the prebuilt one has no Xtensa; RISC-V parts need the RISC-V AOT target in `programs/build.sh`).
3. Re-measure the DRAM and IRAM pools with `free` (the IRAM slot, `partitions.csv`, and the one-slot memory model depend on them).
4. Record the numbers and the pin wiring in `BOARDS.md`.
5. Boot, `ssh`, `pkg install`, OTA update and OTA rollback must all work; list what you tested and what you did not.

## Where help is most useful
Issues labelled `good first issue` and `help wanted`. Ideas: SSH public-key authentication, web server authentication, globbing and
functions in `sh`, retrying Wi-Fi from AP mode, a CI build, new packages (`tree`, `du`, `cal`, `base64`, `xxd`, `dig`, sensor drivers), and ports.
