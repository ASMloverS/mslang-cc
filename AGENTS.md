# AGENTS.md

This file defines MANDATORY rules for ALL contributors, human or AI agent.
Every rule here is BINDING and MUST be obeyed WITHOUT exception — violations
MUST be rejected in review. Detailed style and design specifications live in
`docs/language/`; they are equally binding and MUST be read before writing
any code.

## File Formatting

- All text files MUST be UTF-8 (no BOM) with LF line endings.
- Trailing whitespace MUST NOT exist and MUST be stripped automatically on
  save; `.editorconfig` enforces this and MUST NOT be weakened.

## C Code

- All C code MUST strictly follow `docs/language/10-c-style.md`.

## MS Code

- All MS (mslang) code MUST strictly follow `docs/language/12-ms-style.md`.

## Python Scripts

- All Python code MUST follow PEP 8.
- Every function MUST have complete type-hint annotations (parameters and
  return value).
- Use `X | Y` union syntax; `Optional` and `Union` are FORBIDDEN.

## Build

- All build artifacts MUST be placed in `build/` only; build output anywhere
  else is FORBIDDEN.

## Git Commits

- Commit messages MUST be concise English.
- Format MUST be: `<gitmoji> <type>(<scope>): <commit message>`
  (e.g. `✨ feat(lexer): add comment skipping`).
- The gitmoji MUST be chosen from this list:
  - `✨` feat — introduce a new feature
  - `🐛` fix — fix a bug
  - `📝` docs — add or update documentation
  - `🎨` style — improve structure or formatting (no behavior change)
  - `♻️` refactor — restructure code (no feature, no fix)
  - `⚡` perf — improve performance
  - `✅` test — add or update tests
  - `🔧` chore — build, tooling, or configuration changes
  - `🔥` remove — remove code or files
  - `🚚` move — move or rename files
