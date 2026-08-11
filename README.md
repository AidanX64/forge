# Forge

A build orchestration system for **C, C++, and Assembly**.

Forge exists to eliminate verbose hand-written build scripts and painful
cross-platform / cross-architecture compilation.
The goal is a build UX as simple as:

```sh
forge run --release
```

regardless of target OS, architecture, or toolchain.

## Philosophy

Forge takes heavy inspiration from **Cargo** — the same ergonomic,
"it just works" feel applied to a language ecosystem that has never really
had it.

Implementation language rules for Forge itself:

- **C** — primary, default language for everything.
- **C++** — only when a feature is materially harder or impossible in C.
- **Assembly** — only in hot paths worth hand-optimizing, not
  speculatively, and always commented so the optimization is legible
  rather than a black box.

(Rust was considered for implementing Forge itself, but dropped in favor
of raw C.)

## Project layout

- `src/cli.c` parses command-line arguments.
- `src/commands.c` owns `run`, `debug`, and `clean` invocation lifecycles.
- `src/orchestrator.c` is the build engine: source discovery, compiler dispatch,
  output layout, and build execution.
- `src/manifest.c`, `src/compiler.c`, `src/log.c`, and `src/debug.c` provide the
  focused subsystems used by the build engine.
- `src/forge_util.h` / `src/forge_util.c` hold small shared helpers (error
  formatting, trimming, PATH lookups) used across modules.
- `include/forge/` contains the interfaces between those modules.

## Building from source

Forge is written in C11 and builds itself with `gcc` or `clang` — no other
toolchain required. The only dependency is a C compiler.

### Make (Linux / macOS / Windows with GNU make)

```sh
make                    # builds build/forge
make test               # builds and runs `forge --help`
```

### Direct (no make)

```sh
# Linux / macOS
gcc -Wall -Wextra -Werror -std=c11 -Iinclude src/*.c -o build/forge

# Windows
gcc -Wall -Wextra -Werror -std=c11 -Iinclude src/*.c -o build\forge.exe
```

## Installation

Forge installs to a user-level bin directory — no `sudo` needed.

### Windows

Easiest (builds with the bundled `install.ps1`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
```

Or with GNU make (e.g. MSYS2 / Git Bash):

```sh
make CC=gcc          # mingw-w64 gcc; builds build/forge.exe
make install          # copies it to %USERPROFILE%\bin\forge.exe
```

Use the **mingw-w64** toolchain (package `mingw-w64-x86_64-gcc` on MSYS2), not
the MSYS2 runtime's own `gcc` ports compiler — Forge expects a native Windows
toolchain. `CC=gcc` selects it explicitly; otherwise GNU make falls back to
its built-in `cc`.

Installs to `%USERPROFILE%\bin\forge.exe` and prints the exact `PATH` command
to run (persist it with `setx` or the `SetEnvironmentVariable` snippet it
shows).

### Linux / macOS

```sh
sh scripts/install.sh     # or: chmod +x scripts/install.sh && ./scripts/install.sh
```

Installs to `~/.local/bin/forge` (override with `XDG_BIN_HOME`) and prints the
`export PATH=...` line to add to your shell profile.

### Via make

```sh
make install            # PREFIX defaults to ~/.local/bin (Linux/macOS)
make uninstall          # removes it again
```

After installing, add the bin directory to `PATH` and verify:

```sh
forge --help
```

## Quickstart

```sh
cd <your-project>       # any dir with a Forge.toml + src/
forge run               # build (debug) and run
forge run --release     # build (release) and run
forge debug             # build, then launch a debugger
forge clean             # remove the project's target/ output
forge run --manifest path/to/Forge.toml
                        # build another project without cd-ing into it
```

Try it immediately against the bundled fixture:

```sh
cd test
forge run --release     # prints "Hello world!"
```

## Features

### Build orchestration
Simple, Cargo-style commands (`forge run`, `forge run --release`, ...)
replace verbose, hand-rolled Makefiles/build scripts.

Incremental: already-compiled source files are skipped on rebuild (object
files are compared against source mtimes). Tracking header dependencies is
a future improvement, so touching an `.h` alone does not yet trigger a
recompile.

### Cross-platform compiler dispatch
Forge detects the host OS and version at build time and automatically
invokes the platform's native compiler, falling back to `clang` when no
suitable native compiler is available.

### Manifest-based config
Project configuration lives in a custom manifest file, modeled on
`Cargo.toml` — declarative, not scripted.

Supported fields:

```toml
[project]
name = "app"                 # required

[sources]
c = ["src"]                  # required: at least one of c/cpp/asm, non-empty
cpp = []                     # optional if unused
asm = []                     # optional if unused

[targets]
os = ["windows", "linux", "macos"]   # required
arch = ["x86_64", "aarch64"]         # required

[build]
compiler = "clang-17"        # optional: override compiler dispatch entirely

[profile.debug]
cflags = ["-g", "-O0"]       # required

[profile.release]
cflags = ["-O2"]             # required
```

`targets.os`/`arch` list the hosts this project may build for; Forge builds
the current host only (no cross-compilation). Assembly: GCC/Clang toolchains
accept `.s`/`.S` sources; the MSVC-specific `.asm` dialect requires the MSVC
toolchain (`ml64`).

### Clearer debugging
The `debug` subcommand shells out to the target OS's native debugger
(or a chosen third-party one) and post-processes the output into
something more readable — e.g. a lifter-style disassembly/stack view
instead of a raw debugger dump.

Debugger selection, in order: `cdb` (Windows) → `gdb` (Windows/Linux) →
`lldb` (macOS). Override with the `FORGE_DEBUGGER` environment variable
(e.g. `FORGE_DEBUGGER=gdb`, `FORGE_DEBUGGER=windbg`).

### Verbose, readable logs
All build/run/debug output is written to a `/target` directory.
Logs are verbose by default and meant to be easy for a human to read,
not just machine-parsed.

## Scope

**v1 is build orchestration only.** Forge supports C, C++, and Assembly.

## Status

v1 build orchestration is implemented and working: `forge run`,
`forge run --release`, and `forge debug` build, link, log, and execute on the
host. Compiler dispatch, manifest parsing, and per-invocation `target/logs`
are in place.

## Author note 

  I'm gonna keep it real. I don't have a very good 
understanding of C so i vibe-coded as much of a decent skeleton as i can while I'm learning C deeper.
This is very much a work in progress and I hope u guys are patient with me. :)

## License

TBD.
