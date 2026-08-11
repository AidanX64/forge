# AGENTS.md — Forge

## Project Overview

Forge is a build orchestration system for **C, C++, and assembly**. It exists
to reduce verbose build scripts and painful cross-platform /
cross-architecture compilation.

Design north star: Cargo. The build UX should be as close to

```
forge run --release
```

as possible, regardless of target OS, architecture, or toolchain.

## Language & Implementation Rules

- **C** is the primary implementation language. Default to it.
- **C++** only when a feature is materially harder or impossible in C.
  Justify the exception in the PR/commit description.
- **Assembly** only in hot paths identified as worth hand-optimizing —
  not speculatively. Comment *why* the asm exists, not just what it does.
- No Rust, despite Cargo being the UX inspiration — Rust was considered
  and dropped in favor of raw C.
- Prefer legible, well-commented code over clever code. Optimizations
  should be understandable, not black boxes.

## Core Subsystems

- **Build orchestration (v1 scope)**: build, link, run, clean, and debug.
- **Manifest**: custom manifest format, modeled on `Cargo.toml`. Keep
  parsing strict and errors human-readable.
- **Compiler dispatch**: detect host OS + version at build time and
  invoke the platform's native compiler, falling back to `clang` when
  no native compiler is available/appropriate.
- **Debugger integration**: the `debug` subcommand shells out to the
  target OS's native debugger (or a chosen third-party one) and
  post-processes its output into something clearer — e.g. a
  lifter-style disassembly/stack view instead of raw debugger dump.
- **Logging**: all build/run/debug output goes to a `/target`
  directory. Logs must be verbose by default and easy to read — no
  cryptic single-line failures.

## Conventions for Agents Working on This Repo

- Any new subcommand should compose with `forge run --release` style
  ergonomics — short, predictable, Cargo-like verbs.
- When touching compiler-dispatch code, handle the "unknown/unsupported
  OS" case explicitly rather than silently defaulting to clang.
- Keep `/target` log output structured enough to grep, but readable
  enough for a human skimming a failed build.

## Scope (v1)

- C/C++/assembly language support
