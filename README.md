# opsh

A non-interactive POSIX shell subset with a bytecode VM, designed for ops automation and LLM agent consumption.

## Status

opsh is a working shell. It compiles `.opsh` scripts to bytecode and executes them on a register-free stack-based virtual machine. The binary is under 1MB with zero dependencies beyond libc.

**What works:**

- Full word expansion: `$var`, `${var:-default}`, `${#var}`, `$((1+2))`, `$(cmd)`, field splitting by `$IFS`, pathname globbing
- Control flow: `if`/`elif`/`else`, `for`, `while`/`until`, `case`, brace groups, subshells (via fork)
- Functions with positional parameters (`$1`, `$2`, `$#`, `shift`)
- Pipelines (`cmd1 | cmd2 | cmd3`) and and-or lists (`&&`, `||`, `!`)
- I/O redirections (`<`, `>`, `>>`, `>|`, `<>`, `<&`, `>&`, `<<`, `<<<`)
- 19 builtins: `echo`, `exit`, `true`, `false`, `:`, `cd`, `pwd`, `export`, `unset`, `readonly`, `local`, `shift`, `test`/`[`, `printf`, `read`, `return`, `type`, `trap`
- Signal handling with deferred model, `trap` for INT/TERM/HUP/QUIT/EXIT
- Module system: `lib::import` with namespaced functions (`ssh::exec`, `greet::hello`)
- JSON-RPC 2.0 agent event stream (`--agent-stdio`)
- Bytecode serialization (`.opsb` format)
- LSP server (`opsh lsp`) with syntax diagnostics and completion
- 388 tests under ASan/UBSan

**What is not yet implemented:**

- External command execution via `execve` (commands resolve to builtins or functions; unresolved commands report "command not found")
- `eval`, `source`/`.`
- Here-document expansion of parameters (content is literal)
- `break`/`continue` as shell keywords (infrastructure exists but not wired)
- Full arithmetic operator precedence (left-to-right only)
- `[[ ]]` compilation (parsed but not compiled)
- Capability system (opcodes defined, not enforced)
- Native module stubs (C extension API)
- Cross-compilation, standalone executables

## Usage

```sh
# Run a script
opsh script.opsh

# Compile to bytecode
opsh build script.opsh -o script.opsb

# Run pre-compiled bytecode
opsh script.opsb

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

The VM uses a tagged union value type (`value_t`) with string, integer, array, iterator, and none variants. Strings are reference-counted via a hidden-header scheme (`rcstr`) so that `value_clone` and `OP_PUSH_CONST` avoid heap allocation on the hot path. AST nodes are bump-allocated from a per-parse arena and freed in bulk after compilation. Variables are stored in a scope-chain of hashtable-backed environments; the compiler pre-scans function bodies for `local` declarations and emits `OP_GET_LOCAL`/`OP_SET_LOCAL` to skip the scope chain walk for known locals. Functions are compiled inline with jump-over-body and called via `EXEC_FUNC` with call frame save/restore.

## Project Layout

```
src/
  foundation/   strbuf, plist, hashtable, util, json, rcstr, arena
  parser/       lexer, recursive descent parser, AST types
  compiler/     AST-to-bytecode compiler with backpatching
  vm/           bytecode VM, value types, disassembler, image I/O
  exec/         variable system, signal handling
  builtins/     builtin command registry and implementations
  agent/        JSON-RPC event sink
  lsp/          Language Server Protocol server
  main.c        entry point
include/opsh/   public headers (value.h, vm_fwd.h)
tests/          TAP test harness and test suites
```

## License

See project root for license information.
