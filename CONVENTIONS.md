# Coding Conventions

## Language

C99 with `-Wall -Wextra -Werror -pedantic`. No compiler extensions except `__attribute__((format))` for printf-like functions and `__attribute__((unused))` in the TAP header.

## Formatting

clang-format with the config in `.clang-format`:
- LLVM base style
- 4-space indent
- 100-column limit
- Linux brace style (opening brace on same line for functions)
- Sorted includes with regrouping

Run `make format` before committing. `make format-check` verifies compliance.

## Naming

- Types: lowercase with `_t` suffix: `value_t`, `strbuf_t`, `vm_t`
- Functions: lowercase with underscores: `strbuf_init`, `vm_run`, `environ_get`
- Constants and enum values: UPPER_CASE: `VT_STRING`, `OP_PUSH_CONST`, `MAX_LOOP_DEPTH`
- Macros: UPPER_CASE: `FLUSH_LITERAL`, `ASM_PUSH_CONST`
- Flags: UPPER_CASE with prefix: `VF_EXPORT`, `PE_COLON`, `EXEC_FLAG_FORK`
- Struct fields: lowercase with prefix for the type: `type`, `name`, `fd`, `lineno`

## Memory

- Use `xmalloc`, `xrealloc`, `xcalloc` (from `foundation/util.h`) instead of raw `malloc`/`realloc`/`calloc`. These abort on failure instead of returning NULL.
- Use `checked_add` and `checked_mul` for overflow-safe size calculations before allocation.
- Prefer `xcalloc` over `xmalloc` for structs to get zero-initialization.
- Document ownership at transfer points. When a function takes ownership of a value, the caller must not use it after the call.
- ASan + UBSan are enabled in debug builds. All tests must pass under sanitizers.

## Strings

- All strings are UTF-8 `char *`. No `wchar_t` anywhere.
- Use `strbuf_t` for building strings incrementally.
- `strbuf_detach` transfers ownership to the caller and resets the strbuf.
- `value_to_string` always returns a new allocation; the caller must free it.

## Error Handling

- Allocation failures abort (via `xmalloc`). There is no recovery path for out-of-memory.
- Parser and lexer errors increment an error count and continue parsing where possible. The caller checks `parser_error_count()` after parsing.
- VM errors set `vm->laststatus` and `vm->halted = true`. Diagnostic messages go to stderr via `fprintf`.
- Compiler errors increment `cc->error_count` and continue. The `compile()` function returns NULL if any errors occurred.
- System call errors (fork, pipe, open) are reported to stderr and produce a non-zero exit status.

## Includes

Include order (enforced by clang-format's `SortIncludes` + `IncludeBlocks: Regroup`):

1. The file's own header (for `.c` files)
2. Project headers (`"foundation/..."`, `"parser/..."`, etc.)
3. System headers (`<stdio.h>`, `<stdlib.h>`, etc.)

Each group is separated by a blank line.

## Functions

- Keep functions short. If a function exceeds ~80 lines, consider splitting it.
- Static functions for file-internal use. Declare public functions in headers.
- Forward-declare static functions at the top of the file when mutual recursion requires it.
- Use `(void)param;` to silence unused parameter warnings.

## Comments

- Comments should explain why, not what. The code should be self-documenting.
- Block comments (`/* ... */`) for function documentation and section headers.
- Single-line comments for inline clarification of non-obvious logic.
- No decorative comment blocks or ASCII art dividers.

## Testing

- Tests use the TAP harness in `tests/tap.h`.
- Each test file is a standalone program with `main()`.
- `tap_plan(n)` at the start, `tap_done()` at the end.
- Test names should be descriptive: `"for loop: iterates over words"`.
- Test the behavior, not the implementation. Compile and run, check output.
- Every phase must pass `make test` under ASan/UBSan before merging.

## Commits

- Each commit should be a self-contained logical unit.
- Summary line: max 72 characters, English sentence, no abbreviations or markup.
- Body: explain what changed and why. Reference the phase if relevant.
- Separate refactoring from feature additions.
- Do not mix formatting changes with logic changes.
- Fix commits should be squashed into the commit they fix before the history is finalized.

## Git Hygiene

- No `Co-Authored-By` lines.
- No force pushes to main unless explicitly requested.
- Review fixes are squashed into their parent commits to keep the history clean.

## Build System

- Single non-recursive Makefile at the project root.
- All objects go to `build/` mirroring `src/` structure.
- Auto-dependency tracking via `-MMD -MP`.
- Test binaries depend on `tests/tap.h` and `tests/asm.h` explicitly.
- New source directories require adding `*_SRCS`, `*_OBJS` variables and updating `ALL_SRCS` and `LIB_OBJS`.
