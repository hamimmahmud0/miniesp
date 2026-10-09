---
name: Package request or submission
about: Ask for a package, or offer one
labels: package
---

**Package name and what it does** (one line)

**Type**
- [ ] single program (one `.aot`)
- [ ] bundle (program plus web pages / config / service)

**Syscalls it needs** (see `programs/mini.h`; this sets the `abi=` in the journal)

**Size** (a program started by the web server or a service should stay around 40 KB)

**Status**
- [ ] idea only
- [ ] source ready, tested on hardware (board:           )
- [ ] I can send a pull request that adds it to `pkgs/` and `journals/main.journal`
