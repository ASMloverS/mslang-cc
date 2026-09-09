---
name: mslang-transpile
description: Transpile MSL (mslang) source files (.ms) to C using the mslang compiler and the C99 dialect from docs/language/10-c-style.md. Use when the user asks to translate, port, convert, compile, or transpile MSL/mslang code into C.
---

# MSL → C Transpilation

Transpile MSL (mslang) source into C. There is no mechanical compiler; you
are the compiler. Correctness of the output C code is your responsibility.

The C99 dialect and all binding rules live in `docs/language/10-c-style.md`.
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

- §3.2 (naming) — ALWAYS. Read first, applies to every file.
- §4.1–4.3 (header/layout, includes, file-local types) — per output file.
- §4.4 (functions: `static`, return types, parameter passing) — per function.
- §5.2 (string lifetime), §7.2 (error handling), §7.4 (allocation) — when the
  source touches strings, errors, or heap memory.
- §7.6 (container selection) — when the source needs a data structure.
- §8 (cast macros) — when the code needs type conversions. Includes the full
  integer conversion policy (narrowing via `MslCast*`, never raw C casts).
- §10 (restrictions) — check the final output against this list.

### 3. Map MSL to C

- Names: apply §3.2 — functions `MslPascalCase`, variables/parameters
  `camelCase`, macros/constants `MSL_UPPER_SNAKE`, enum values
  `MslName_Value`.
- Types: fixed-width types only (`int32_t`, `uint64_t`, ...); `size_t` for
  sizes and indices; `bool` for truth values (§3.3).
- Mutable out-results are pointer parameters; large struct inputs are
  `const`-pointer parameters (§4.4.3).
- Strings: `char *` mutable / `const char *` read-only, always NUL-terminated;
  `MslString` only when the length itself is the point (§5.2).
- Runtime APIs: use the `msl_*` headers from `docs/language/09-c-api.md` —
  never reimplement what the runtime provides.
- No suitable runtime API: add exactly one abstraction layer per §3.4, named
  `Msl<Domain><Action>`. It must be reusable; caller logic stays out.
- Data structures: pick from `msl_list`, `msl_dictionary`, `msl_bitset`,
  `msl_queue` per §7.6. Introduce a local container only when nothing fits.

### 4. Emit and verify

- Output `.c`/`.h` per §4 (UTF-8, LF, 4-space indent, 80-column limit,
  Allman braces, one declaration per line).
- Sweep the output against §10 restrictions and the cleanup rules in §7.5.
- Every allocation needs an error path and a cleanup path (§7.2/§7.4/§7.5).

## Common pitfalls

- Raw C casts instead of the §8 `MslCast*` macros.
- `MslString` used as the general string type — it is for length-aware
  processing only; plain C strings are the default.
- Reimplementing functionality the runtime already provides — check
  `docs/language/09-c-api.md` first.
- Uppercase function or type names — only macros and constants are
  `UPPER_SNAKE`; types are `MslPascalCase`, functions `MslPascalCase`.
- Pointer typedefs and `typedef` of primitives — both forbidden (§3.1/§3.3).
- `goto` for anything other than a cleanup path (§10).
