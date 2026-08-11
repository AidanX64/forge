# Forge — Step-by-Step Build Plan

Run these in order. Let Codex finish and compile each step before
starting the next. Steps 3 and 7 are the most likely to need
back-and-forth.

---

## 1. Scaffold

Set up a C project called Forge (build orchestration system for
C/C++/assembly). Use a plain Makefile bootstrap (no CMake — Forge
should eventually build itself). Directory layout: `src/`, `include/`,
`target/` (build output + logs, gitignored). Strict C compile flags
(`-Wall -Wextra -Werror -std=c11`). Include the AGENTS.md and
README.md already in the repo root as context for conventions.

---

## 2. Manifest parser

Design and implement a parser for Forge's manifest file (working name
`Forge.toml`, modeled on Cargo.toml but custom format — don't pull in
a TOML lib, hand-roll a minimal parser in C). Fields needed for v1:
project name, C/C++/asm source dirs, target OS/arch list, compiler
override (optional), build profile settings (debug/release). Output
an in-memory struct the rest of Forge will consume.

---

## 3. Compiler dispatch

Implement OS/version detection at build time and a compiler-dispatch
layer: pick the platform's native compiler (MSVC on Windows,
gcc/clang on Linux, clang on macOS) and fall back to clang when no
native compiler is found or the OS is unrecognized — never silently
no-op. Expose this as a module other parts of Forge call to get an
invocable compile command for a given source file/target.

---

## 4. Core orchestrator + CLI

Build the `forge` CLI with `run` and `run --release` subcommands for
v1. On `forge run`, read the manifest, resolve sources, invoke the
compiler-dispatch layer per translation unit, link, and execute the
resulting binary. No dependency fetching — v1 is build orchestration
only, flag any dependency-related work as TODO instead of
implementing it.

---

## 5. Logging

Add a logging system that writes verbose, human-readable build/run
output to a `target/` directory (one log per build invocation). Must
be grep-able but readable by a human skimming a failed build — clear
stage markers (parse manifest, compile, link, run), not a single
opaque dump.

---

## 6. Debug subcommand

Add a `forge debug` subcommand that shells out to the host OS's
native debugger (or a configurable third-party one) and
post-processes its output into a clearer, lifter-style
disassembly/stack view instead of dumping raw debugger output.

This completes the initial v1 build plan.
