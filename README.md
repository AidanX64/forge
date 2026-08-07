# Forge

A package manager and automated build system for **C, C++, and Assembly**.

Forge exists to kill three problems: dependency hell, verbose hand-written
build scripts, and painful cross-platform / cross-architecture compilation.
The goal is a build UX as simple as:

```sh
forge run --release
```

regardless of target OS, architecture, or toolchain.

## Philosophy

Forge takes heavy inspiration from **Cargo** (Rust's package manager) —
same ergonomics, same "it just works" feel — applied to a language
ecosystem that has never really had it.

Implementation language rules for Forge itself:

- **C** — primary, default language for everything.
- **C++** — only when a feature is materially harder or impossible in C.
- **Assembly** — only in hot paths worth hand-optimizing, not
  speculatively, and always commented so the optimization is legible
  rather than a black box.

(Rust was considered for implementing Forge itself, but dropped in favor
of raw C.)

## Building from source

Forge is written in C11 and builds itself with `gcc` or `clang` — no other
toolchain required. The only dependency is a C compiler.

### Make (Linux / macOS / Windows with GNU make)

```sh
make                    # builds target/forge
make test               # builds and runs `forge --help`
```

### Direct (no make)

```sh
# Linux / macOS
gcc -Wall -Wextra -Werror -std=c11 -Iinclude src/*.c -o target/forge

# Windows
gcc -Wall -Wextra -Werror -std=c11 -Iinclude src/*.c -o target\forge.exe
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
make                    # builds target\forge.exe
make install            # copies it to %USERPROFILE%\bin\forge.exe
```

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

### Cross-platform compiler dispatch
Forge detects the host OS and version at build time and automatically
invokes the platform's native compiler, falling back to `clang` when no
suitable native compiler is available.

### Manifest-based config
Project configuration lives in a custom manifest file, modeled on
`Cargo.toml` — declarative, not scripted.

### Parallel multi-platform runners
Build/test runners can run in parallel across heterogeneous machines:
native or cloud, VM or bare metal, communicating over raw TCP. The
runner pool is assumed heterogeneous (mixed OS/arch/GPU), not a
uniform fleet.

Graphics programming was a core consideration from day one, not an
afterthought — runner machines with a **real GPU and a monitor** are
preferred over headless boxes, since graphics-program builds need to
actually render to be verified.

### Clearer debugging
The `debug` subcommand shells out to the target OS's native debugger
(or a chosen third-party one) and post-processes the output into
something more readable — e.g. a lifter-style disassembly/stack view
instead of a raw debugger dump.

### Verbose, readable logs
All build/run/debug output is written to a `/target` directory.
Logs are verbose by default and meant to be easy for a human to read,
not just machine-parsed.

## Scope

**v1 is build orchestration only.** Dependency fetching and package
management are explicitly **out of scope for v1** — Forge behaves as a
build system first; the "package manager" ambitions come later.

## Non-Goals (for now)

- Dependency/package fetching and resolution
- Support for languages other than C, C++, and Assembly

## Status

v1 build orchestration is implemented and working: `forge run`,
`forge run --release`, and `forge debug` build, link, log, and execute on the
host. Compiler dispatch, manifest parsing, and per-invocation `target/logs`
are in place. Dependency fetching and the TCP runner pool are planned next.

## License

TBD.