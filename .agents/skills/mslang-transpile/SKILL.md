---
name: mslang-transpile
description: Transpile MSL (mslang) source files (.ms) to C using the mslang compiler and the C11 dialect from docs/language/10-c-style.md. Use when the user asks to translate, port, convert, compile, or transpile MSL/mslang code into C.
---

# MSL → C Transpilation

Transpile MSL (mslang) source into C. There is no mechanical compiler; you
are the compiler. Correctness of the output C code is your responsibility.

The C11 dialect and all binding rules live in `docs/language/10-c-style.md`.
It is long — read only the section relevant to the construct at hand.

## Workflow

### 1. Gather all inputs

- Read every `.ms` file to transpile.
- Identify the APIs each file uses (memory, IO, string, math, OS).
- Always read `docs/language/09-c-api.md` in full — the runtime headers are
  available for every transpilation, so you need the complete picture of what
  exists.

### 2. Read style rules on demand

Read `docs/language/10-c-style.md` per section, matched to what the current
source actually uses — never all at once:

- §2 (formatting) and §3 (naming) — ALWAYS. Read first, apply to every file.
- §1 (file organization, include guards, include order) — per output file.
- §4 (C feature rules: fixed-width integers, typedef limits, const) — when
  declaring types and functions.
- §5 (error handling, resource management) — when a function can fail or
  owns resources.
- §6 (memory and GC discipline) — when the source touches heap memory or
  holds `MsObject*` across API calls.
- §7 (comments and docs) — when writing public headers.
- §8 (assertions) — for internal invariants.
- §9 (platform abstraction) — when the source needs threads, atomics,
  clocks, sockets, or dynamic loading.

### 3. Map MSL to C

- Names: apply §3 — functions `msLowerCamelCase`, types `MsUpperCamelCase`,
  constants/macros/enum values `MS_UPPER_SNAKE`, file-local statics
  `lowerCamelCase`.
- Types: fixed-width types only (`int64_t`, `uint8_t`, ...); `size_t` for
  sizes and indices; `bool` from `<stdbool.h>`; no custom integer aliases
  (§4).
- Pointers: the star attaches to the type — `MsObject* obj`, never
  `MsObject *obj`; declare one pointer variable per statement (§2).
- Error handling: report failure via `MsResult` or `NULL` + error state;
  fail fast with early returns and release resources in reverse order of
  acquisition (§5). Never let `errno` propagate across layers.
- Memory: allocate only via `msAlloc`/`msRealloc`/`msFree`; never call
  `malloc` directly (§6).
- GC roots: any local `MsObject*` that lives across a possible allocation
  must be pushed with `msRootPush` and popped in LIFO order
  (09-c-api.md §3).
- Runtime APIs: use the `ms*` functions from `docs/language/09-c-api.md` —
  never reimplement what the runtime provides.
- Platform-dependent functionality (threads, atomics, sockets, clocks): call
  the `src/platform/` abstraction; no `#ifdef _WIN32` outside it (§9).

### 4. Emit and verify

- Output `.c`/`.h` per §1–§2: UTF-8, LF, 2-space indent, 120-column limit,
  K&R braces, include guards (no `#pragma once`), self-contained headers.
- Sweep the output against the §4 prohibition list: no VLA, no `alloca`, no
  K&R definitions, no `gets`-class functions, no mutable globals, no
  typedefs outside the allowed set.
- Every allocation needs an error path and a cleanup path (§5/§6).

## Common pitfalls

- Star-left pointer declarations (`MsObject *obj`) — always `MsObject* obj`.
- Direct `malloc`/`free` instead of `msAlloc`/`msRealloc`/`msFree`.
- Holding an `MsObject*` across an allocation without `msRootPush`.
- Reimplementing functionality the runtime already provides — check
  `docs/language/09-c-api.md` first.
- Wrong naming case — functions are `msLowerCamelCase`, types
  `MsUpperCamelCase`, only constants/macros are `MS_UPPER_SNAKE`.
- `/* */` comments — C code uses `//` only, including doc comments (§7).
- `typedef` of internal structs — the allowed set is the config structs and
  `MsCFunction`, opaque types, and enums; internal structs stay
  `struct MsFoo` (§4).
