# Agent Guidelines for opsh

## Project Overview

opsh is a non-interactive shell for ops automation with a bytecode VM. It parses
a POSIX shell subset (plus extensions like `[[ ]]`, `local`, arrays), compiles
to bytecode, and executes in a custom VM. Zero external dependencies beyond libc.

## Code Conventions

- C99, compiled with `clang -Wall -Wextra -Werror -pedantic`
- Type names use lowercase `_t` suffix: `parser_t`, `lint_ctx_t`, `word_part_t`
- Use `xmalloc`, `xcalloc`, `xstrdup` from `foundation/util.h` — never raw malloc
- Use `read_file()` and `read_stdin()` from `foundation/util.h` — don't reimplement
- Use `json_write_string`, `json_key_string`, etc. from `foundation/json.h` for JSON output — don't hand-roll JSON escaping
- ASan + UBSan enabled in debug builds; all code must be clean under both
- `make format` before committing (clang-format, LLVM style, 4-space indent, 100-col)

## Build and Test

```sh
make              # debug build
make RELEASE=1    # release build
make test         # all tests (TAP harness)
make format-check # CI formatting check
make fuzz-build FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang  # fuzz targets
```

## Architecture

| Directory | Purpose |
|-----------|---------|
| `src/foundation/` | Data structures: strbuf, plist, hashtable, util, json |
| `src/parser/` | Lexer, recursive descent parser, AST types |
| `src/compiler/` | Single-pass bytecode compiler with backpatching |
| `src/vm/` | Fetch-decode-execute loop, value types, disassembler, .opsb I/O |
| `src/exec/` | Variable scope chain, signal handling |
| `src/builtins/` | Builtin command registry and implementations |
| `src/format/` | AST pretty-printer (`opsh format`) |
| `src/lint/` | Static analysis checks (`opsh lint`) |
| `src/agent/` | JSON-RPC event sink |
| `src/lsp/` | LSP server (diagnostics + completion) |

## Subcommands

- `opsh <script> [args...]` — run a script
- `opsh -c 'string' [args...]` — run an inline command
- `opsh build <script> [-o out]` — compile to bytecode or standalone binary
- `opsh format [-w] [-d] [-i N] [file...]` — format scripts
- `opsh lint [-f gcc|tty|json1|quiet] [-S level] [file...]` — lint scripts
- `opsh lsp` — LSP server mode

## Lint System

Checks live in `src/lint/checks.c`. The walker lives in `src/lint/lint.c`.
Adding a new check: write the detection in `checks.c`, add a test in
`tests/lint/test_lint.c`. The walker calls checks at specific AST positions
(per-word, per-command, per-compound, per-and-or, post-walk variable tracking).

Shellcheck-compatible codes (SC2xxx). Use the same code when the check matches
shellcheck's semantics. See `docs/lint-checks.md` for the full roadmap.

The LSP reuses `lint_check()` to publish diagnostics — lint and LSP share the
same analysis code.

## Formatter

The formatter in `src/format/format.c` walks the AST and pretty-prints it.
Comments are preserved via a flat list collected during lexing (stored on
`lexer_t`, extracted via `parser_take_comments()`), interleaved with output by
source line number. The AST itself does not carry comments.

## Standalone Binaries

`opsh build script.opsh -o myapp` copies the opsh binary, appends the .opsb
bytecode, and writes an 8-byte trailer (4-byte payload offset LE + "OPSB"
magic). The resulting binary detects the appended payload on startup. Imported
modules are compiled into the bytecode image — no external files needed.

## Fuzz Targets

Fuzz harnesses in `fuzz/`. Built with LLVM clang (not Apple clang, which lacks
libfuzzer). Seed corpora in `fuzz/corpus/`.

## Next: Plugin Architecture

Decided but not yet implemented. Native modules split into:

- **Built-in** (zero deps, in libopsh.a): `net` (TCP/UDP sockets), `json`
  (JSONPath querying), `yaml` (emitter)
- **Plugins** (external deps, linked on demand): `tls` (OpenSSL/BearSSL),
  `ssh` (libssh2)

Each plugin exports `const opsh_plugin_t *opsh_plugin_info(void)` with name,
version, init, destroy. `opsh build` analyzes imports and links only needed
plugin .o files. For local dev, plugins load via dlopen.
