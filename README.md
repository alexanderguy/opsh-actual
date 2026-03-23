# opsh

A non-interactive POSIX shell with a bytecode VM, designed for ops automation and LLM agent consumption.

## Status

opsh compiles `.opsh` scripts to bytecode and executes them on a stack-based virtual machine. The stripped release binary is ~170KB with zero dependencies beyond libc.

**What works:**

- External command execution via fork/execvp with proper signal handling
- Full word expansion: `$var`, `${var:-default}`, `${var#pat}`, `${var%pat}`, `${var/pat/rep}`, `${var/#pat/rep}`, `${var/%pat/rep}`, `${#var}`, `$(cmd)`, tilde expansion (`~`, `~/path`, `~user`)
- Arithmetic: `$((expr))` with 14-level operator precedence, variables, assignment, ternary, short-circuit, pre/post increment
- Control flow: `if`/`elif`/`else`, `for` (with and without `in` list), `while`/`until`, `case`, brace groups, subshells
- Array variables: `arr=(a b c)`, `${arr[N]}`, `${arr[$i]}`, `arr[N]=val`, `${#arr[@]}`
- `[[ ]]` expressions with file tests, string/numeric comparisons, glob matching, `=~` regex with `BASH_REMATCH` capture groups, `&&`, `||`, `!`, `!=`
- Functions with positional parameters, `"$@"` with word boundary preservation, `shift`
- Pipelines (`cmd1 | cmd2 | cmd3`) and and-or lists (`&&`, `||`, `!`)
- I/O redirections (`<`, `>`, `>>`, `>|`, `<>`, `<&`, `>&`, `<<`, `<<-`, `<<<`)
- Here-documents with delimiter matching, tab stripping, parameter expansion, and quoted delimiters
- `break`/`continue` with multi-level depth and proper iterator cleanup
- Temporary assignment scoping: `VAR=value cmd` reverts after command completes
- Shell options: `set -e` (errexit), `set -u` (nounset), `set -x` (xtrace)
- 27 builtins: `echo`, `exit`, `true`, `false`, `:`, `cd`, `pwd`, `export`, `unset`, `readonly`, `local`, `shift`, `test`/`[`, `printf`, `read`, `return`, `type`, `trap`, `eval`, `.`/`source`, `command`, `exec`, `wait`, `kill`, `umask`, `set`, `getopts`
- Script arguments (`$1`-`$N`, `$#`, `$@`, `$*`, `$$`, `$!`, `$-`, `$0`) and `opsh -c 'string'`
- Signal handling with deferred dispatch, `trap` for INT/TERM/HUP/QUIT/USR1/USR2/PIPE/ALRM/EXIT; `kill` supports KILL and other signals
- Module system: `lib::import` with namespaced functions
- Functions defined in `eval` and `source` are callable by the parent
- JSON-RPC 2.0 agent event stream (`--agent-stdio`)
- Bytecode serialization (`.opsb` format) and standalone binary compilation
- Formatter (`opsh format`) and linter (`opsh lint`) with shellcheck-compatible output
- LSP server (`opsh lsp`) with diagnostics and completion
- Comprehensive test suite under ASan/UBSan with libfuzzer harnesses

**Known limitations:**

- Capability system and native module API not yet implemented

## Usage

```sh
# Run a script
opsh script.opsh

# Run a script with arguments
opsh script.opsh arg1 arg2

# Inline command
opsh -c 'echo hello $1' -- world

# Compile to standalone binary
opsh build script.opsh -o myapp

# Compile to bytecode
opsh build script.opsh -o script.opsb

# Format scripts
opsh format script.opsh

# Lint scripts
opsh lint script.opsh

# Run with agent event stream (JSON-RPC 2.0 on stderr)
opsh --agent-stdio script.opsh

# Start the LSP server
opsh lsp
```

## Building

```sh
make            # debug build (ASan + UBSan enabled)
make RELEASE=1  # release build (-O2, no sanitizers)
make test       # run all tests
make format     # format all source with clang-format
make fuzz-build FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang  # build fuzz targets
```

Requires clang (C99) and a POSIX system (macOS or Linux). No external dependencies.

## Architecture

```
Source (.opsh)
    |
    v
  Parser (recursive descent, context-dependent lexer)
    |
    v
  AST (sh_list_t -> and_or_t -> command_t tree)
    |
    v
  Compiler (single-pass, backpatching)
    |
    v
  Bytecode image (constant pool + instruction stream)
    |
    v
  VM (stack-based, fetch-decode-execute loop)
```

The VM uses a tagged union value type (`value_t`) with string, integer, array, iterator, and none variants. Strings are reference-counted via a hidden-header scheme (`rcstr`) so that `value_clone` and `OP_PUSH_CONST` avoid heap allocation on the hot path. AST nodes are bump-allocated from a per-parse arena and freed in bulk after compilation. Variables are stored in a scope-chain of hashtable-backed environments; the compiler pre-scans function bodies for `local` declarations and emits `OP_GET_LOCAL`/`OP_SET_LOCAL` to skip the scope chain walk for known locals. Functions are compiled inline with jump-over-body and called via `EXEC_FUNC` with call frame save/restore. Functions defined in `eval`/`source` are supported by keeping sub-images alive and swapping the active image on call/return.

## Project Layout

```
src/
  foundation/   strbuf, plist, hashtable, util, json, rcstr, arena
  parser/       lexer, recursive descent parser, AST types
  compiler/     AST-to-bytecode compiler with backpatching
  vm/           bytecode VM, value types, arithmetic evaluator, disassembler, image I/O
  exec/         variable system, signal handling
  builtins/     builtin command registry and implementations
  format/       AST pretty-printer (opsh format)
  lint/         static analysis checks (opsh lint)
  agent/        JSON-RPC event sink
  lsp/          Language Server Protocol server
  main.c        entry point
include/opsh/   public headers (value.h)
tests/          TAP test harness and test suites
fuzz/           libfuzzer harnesses and seed corpora
```

## License

See project root for license information.
