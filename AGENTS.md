# AGENTS.md — Forge

## Project Overview

Forge is a package manager and automated build system for **C, C++, and
assembly**. It exists to kill three problems: dependency hell, verbose
build scripts, and painful cross-platform / cross-architecture compilation.

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

- **Resolver/orchestrator (v1 scope)**: build orchestration only.
  Dependency fetching / package management is explicitly out of scope
  for v1 — don't add it prematurely.
- **Manifest**: custom manifest format, modeled on `Cargo.toml`. Keep
  parsing strict and errors human-readable.
- **Compiler dispatch**: detect host OS + version at build time and
  invoke the platform's native compiler, falling back to `clang` when
  no native compiler is available/appropriate.
- **Runners**: parallel multi-platform build/test runners, both native
  and cloud, both VM and bare metal, communicating over raw TCP.
  Graphics programming is a first-class use case — runner machines
  with a real GPU and a monitor are preferred over headless boxes,
  since GPU/graphics-program builds need to actually render to verify.
- **Debugger integration**: the `debug` subcommand shells out to the
  target OS's native debugger (or a chosen third-party one) and
  post-processes its output into something clearer — e.g. a
  lifter-style disassembly/stack view instead of raw debugger dump.
- **Logging**: all build/run/debug output goes to a `/target`
  directory. Logs must be verbose by default and easy to read — no
  cryptic single-line failures.

## Conventions for Agents Working on This Repo

- Don't add dependency-resolution/package-fetching logic — that's
  post-v1. Flag it as a TODO instead of implementing it.
- Any new subcommand should compose with `forge run --release` style
  ergonomics — short, predictable, Cargo-like verbs.
- When touching compiler-dispatch code, handle the "unknown/unsupported
  OS" case explicitly rather than silently defaulting to clang.
- When touching the runner/TCP layer, assume multiple heterogeneous
  machines (different OS, arch, GPU presence) are in the pool
  simultaneously — don't assume a homogeneous fleet.
- Keep `/target` log output structured enough to grep, but readable
  enough for a human skimming a failed build.

## Non-Goals (v1)

- Dependency/package fetching and resolution
- Non-C/C++/assembly language support