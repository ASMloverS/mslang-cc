# mslang-cc

**mslang** is a general-purpose scripting language with Go-flavored syntax and
Python-like dynamic semantics, implemented in pure C11 on top of a bytecode
virtual machine. This repository hosts its design documentation and (in
progress) the reference implementation.

## Features

- **Standalone interpreter** — run `.ms` scripts directly, no entry function
  required; includes a REPL.
- **First-class embedding** — a Python-style C API (`ms` prefix) for creating
  interpreters, executing scripts, and two-way interop from C/C++.
- **Go-style concurrency** — coroutines and channels with `async`/`await`,
  scheduled M:N across threads.
- **Python-style semantics** — dynamic typing, arbitrary-precision integers,
  classes with single inheritance and magic methods,
  `try/except/finally/raise`, list/dict comprehensions.
- **Simple, readable implementation** — C11 core with zero third-party
  dependencies; mark-sweep GC, stack-based VM.

```ms
import "strings"

func fib(n) {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

print(fib(10))
```

## Status

Design draft v0.1 — the language is fully specified in
[`docs/language/`](docs/language/) (currently written in Chinese); the
implementation is being built to match.

## Repository layout

```
docs/language/   language specification and style guides
include/mslang/  public C API headers            (planned)
src/             lexer, parser, compiler, VM, GC (planned)
stdlib/, lib/    standard library (C and .ms)    (planned)
tests/           C unit tests and script tests   (planned)
```

## Build

Requires CMake ≥ 3.20 and a C11 compiler (MSVC 2019+ / GCC 10+ / Clang 12+).
All build artifacts go into `build/` only.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

## Contributing

All contributors must follow [`AGENTS.md`](AGENTS.md): UTF-8/LF files, C code
per `docs/language/10-c-style.md`, MS code per `docs/language/12-ms-style.md`,
and commit messages in the form `<gitmoji> <type>(<scope>): <message>`.
