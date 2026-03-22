# Architecture

opsh is a non-interactive shell that compiles scripts to bytecode and executes them on a stack-based virtual machine. It is designed for operations automation and LLM agent consumption.

## Design Principles

1. **Non-interactive.** No line editing, no job control UI, no prompt expansion. The shell reads a script and executes it.

2. **Agent-first I/O.** A JSON-RPC 2.0 protocol provides structured execution events for machine consumption. Human-readable output is secondary.

3. **Compilable.** Scripts compile to a serializable bytecode format (`.opsb`) that can be distributed and executed without re-parsing.

4. **Minimal footprint.** The binary has zero dependencies beyond libc. Everything is modular.

5. **UTF-8 throughout.** All strings are `char *` UTF-8. No `wchar_t`, no locale-dependent behavior.


## Compilation Pipeline

```
Source (.opsh)
    |
    v
  Lexer (context-dependent tokenization)
    |
    v
  Parser (recursive descent)
    |
    v
  AST (sh_list_t -> and_or_t -> command_t)
    |
    v
  Compiler (single-pass with backpatching)
    |
    v
  Bytecode image (constant pool + instruction stream)
    |         |
    v         v
  VM        .opsb file
```


## Foundation Layer

All modules depend on a small set of utility types.

### Growable string buffer (`strbuf_t`)

Geometric-growth byte buffer. Always null-terminated. Provides append, printf, detach (transfers ownership), and UTF-8 codepoint counting.

### Pointer list (`plist_t`)

NULL-terminated growable array of `void *`. Used for argument lists, word lists, and AST node collections.

### Hash table (`hashtable_t`)

Open-addressing with FNV-1a hashing. Tombstone-aware load factor prevents infinite probe loops under heavy delete/insert cycles. Keys are borrowed pointers; the caller manages their lifetime.

### Checked arithmetic

`checked_add` and `checked_mul` abort on overflow. `xmalloc`, `xrealloc`, `xcalloc` abort on allocation failure.

### Reference counting (`refcount_t`)

Used on AST command nodes so function definitions can share parsed bodies without deep-copying.

### JSON writer

String escaping and object/array construction built on `strbuf_t`. Used by the agent event sink and the LSP server.


## Lexer

The lexer is embedded in the parser (not a separate pass). Shell tokenization is context-dependent: the same character sequence can be a keyword, an operator, or a plain word depending on grammatical position.

The lexer handles:
- Quoting: single quotes (literal), double quotes (with expansion), backslash escaping
- Parameter expansion: `$var`, `${var:-default}`, `${#var}`, `${var#pattern}`, `${var/pat/rep}`
- Command substitution: `$(cmd)` (stored as unparsed text for the compiler to re-parse)
- Arithmetic expansion: `$((expr))`
- Here-documents, here-strings
- IO numbers (digits immediately before `<` or `>`)
- All shell operators and reserved words

A `quoted` flag on each word unit tracks whether content appeared inside quotes, enabling the compiler to decide whether field splitting and globbing apply.


## Parser

Recursive descent. Produces a tree of linked-list AST nodes:

```
sh_list_t          sh-list (;-separated commands)
  and_or_t      pipeline (|-separated commands, optional ! prefix)
    command_t      command (discriminated union)
      CT_SIMPLE    simple command (words + assignments + redirections)
      CT_GROUP     brace group { ... }
      CT_SUBSHELL  subshell ( ... )
      CT_IF        if/elif/else
      CT_FOR       for loop (variable + word list + body)
      CT_WHILE     while/until loop
      CT_CASE      case statement (word + pattern/body items)
      CT_FUNCDEF   function definition (name + body)
      CT_BRACKET   [[ ]] expression (expression tree)
```

Word units form a linked list per word: `WP_LITERAL` (literal text), `WP_PARAM` (parameter expansion with operator/flags), `WP_CMDSUB` (command substitution), `WP_ARITH` (arithmetic expression).

Errors are collected into a structured array (`parse_error_t` with line number and message) for programmatic access by the LSP server.


## Compiler

Single-pass AST-to-bytecode compiler with backpatching for forward jumps.

### Word compilation

`compile_word` walks the `word_part_t` linked list, emitting `PUSH_CONST` for literals, `GET_VAR` or `EXPAND_PARAM` for parameter expansions, `GET_SPECIAL` for special parameters (`$?`, `$#`), `EXPAND_ARITH` for arithmetic, and `CMD_SUBST` for command substitution. Multi-part words are joined with `CONCAT`.

### Dynamic argument count

Each word in a simple command produces a `(values... count)` group. Unquoted words with expansions get `SPLIT_FIELDS` (which may produce multiple values). Unquoted literals with glob characters get `GLOB`. `COLLECT_WORDS` flattens all groups into `(args... argc)` for the exec handlers.

### Control flow

- **And-or lists:** `JMP_FALSE` for `&&`, `JMP_TRUE` for `||`, with backpatching.
- **If/elif/else:** condition + `JMP_FALSE` over body, `JMP` to end of chain, backpatched.
- **For loops:** word list compiled to groups, `COLLECT_WORDS`, `INIT_ITER`. Loop body between `GET_NEXT_ITER`/`DUP`/`JMP_NONE` and `JMP` back. Cleanup POPs at exit.
- **While/until:** condition at loop top, `JMP_FALSE`/`JMP_TRUE` over body, `JMP` back.
- **Case:** each item compiles the case word + pattern, `PATTERN_MATCH`, `JMP_TRUE` to body. `;;` emits `JMP` to end; `;&` falls through.
- **Functions:** body compiled inline with `JMP` over it. Offset recorded in function table. `RET` at end.

### Module compilation

When the compiler encounters `lib::import name`, it resolves the module file (`script_dir/lib/name.opsh`, then `$OPSH_LOADPATH`), parses and compiles it into the same bytecode image with shared constant pool and function table. Circular dependencies are detected via an import stack. An `OP_IMPORT` instruction is emitted for runtime initialization.

### Pipelines and command substitution

Each pipeline member or command substitution body is compiled as a sub-segment (JMP over + body + HALT). Pipeline sub-segments are compiled first, then the contiguous `PIPELINE`/`PIPELINE_CMD`/`PIPELINE_END` block is emitted so the VM can read them sequentially.


## Bytecode VM

Stack-based, fetch-decode-execute loop. No registers.

### Value type (`value_t`)

Tagged union with five variants:
- `VT_STRING` -- `char *` (UTF-8, owned)
- `VT_INTEGER` -- `int64_t`
- `VT_ARRAY` -- array of `char *`
- `VT_ITERATOR` -- iteration state for for-loops (element array + position)
- `VT_NONE` -- unset/exhausted

### Instruction encoding

Variable-width instructions: 1-byte opcode + 0 or more operands. Multi-byte operands are little-endian. Operand types: u8, u16 (constant pool index), i32 (jump offset), u32 (bytecode offset).

Jump offsets are relative to the instruction pointer after the entire jump instruction has been read.

### Operand stack

Fixed-size array (`VM_STACK_MAX = 1024`). Push/pop with bounds checking (abort on overflow/underflow).

### Variable system

Variables are stored in a scope-chain of hashtable-backed environments (`environ_t`). Each scope has a parent pointer. Variable lookup walks the chain. `environ_assign` (POSIX semantics) modifies the first scope that contains the variable; `environ_set_local` creates in the current scope only (for `local`).

Variables carry flags: `VF_EXPORT` (inherited by child processes), `VF_READONLY` (writes rejected with an error).

### Call frames

Function calls push a `call_frame_t` with return IP, saved environment, saved stack base, and saved loop depth. `RET` restores all of these and cleans up any extra values left on the stack.

### Redirections

`REDIR_SAVE` pushes a frame onto the FD save/restore stack. Each redirection op (`REDIR_OPEN`, `REDIR_DUP`, `REDIR_CLOSE`, `REDIR_HERE`) dups the original FD to a saved slot before redirecting. `REDIR_RESTORE` pops the frame and restores all FDs in reverse order.

Here-documents use `mkstemp` temporary files (immediately unlinked) to avoid pipe buffer deadlock on large content.

### Process execution

- **Builtins:** dispatched via a registry table (`builtin_entry_t` array). Resolved by name at compile time or runtime.
- **Functions:** called via `EXEC_FUNC` (compile-time resolved) or `EXEC_SIMPLE` (runtime lookup). Push call frame, new scope, bind positional parameters (`$1`..`$N`, `$#`), jump to function body.
- **Pipelines:** fork a child per member, connect with pipes, wait for all. Each child runs `vm_run` from its sub-segment offset. Stdio flushed before fork.
- **Command substitution:** fork, child redirects stdout to pipe, runs sub-segment. Parent reads pipe, strips trailing newlines, pushes result as string.

### Signal handling

Deferred model: SIGINT/SIGTERM handlers set `volatile sig_atomic_t` flags. The VM checks for pending signals at the top of each dispatch loop iteration (safe point). If a trap handler is registered, it is parsed, compiled, and executed in a fresh VM with shared environment. SIGPIPE is ignored in the parent shell. Signal defaults are restored in child processes after fork.


## Builtins

19 builtins: `echo`, `exit`, `true`, `false`, `:`, `cd`, `pwd`, `export`, `unset`, `readonly`, `local`, `shift`, `test`/`[`, `printf`, `read`, `return`, `type`, `trap`.

Builtins write to fd 1 via `write()` (not `fputs`) so redirections take effect. The `return` builtin sets a `return_requested` flag on the VM; the dispatch loop checks it after `EXEC_BUILTIN` and triggers call frame unwind.

`test`/`[` is a runtime expression parser (not compiled like `[[ ]]`). Supports file tests (`-f`, `-d`, `-e`, `-r`, `-w`, `-x`, `-s`), string comparison (`=`, `!=`, `-z`, `-n`), and integer comparison (`-eq`, `-ne`, `-lt`, `-le`, `-gt`, `-ge`).


## Module System

`lib::import` is a compile-time directive. The compiler resolves the module file, parses and compiles it into the same bytecode image. All modules share a single constant pool and function table.

Functions defined with a `namespace::` prefix in a module file are callable from the importing script by their full name. No `module::exports` directive is required in v1 -- all namespaced functions are public.

At runtime, `OP_IMPORT` executes the module's top-level initialization code in a fresh VM with shared environment. A hashtable tracks which modules have been initialized to prevent duplicate execution.

Module resolution order:
1. `script_dir/lib/name.opsh`
2. `$OPSH_LOADPATH` entries


## Agent Interface

### Transport

JSON-RPC 2.0 with Content-Length framing (same as LSP):

```
Content-Length: 73\r\n
\r\n
{"jsonrpc":"2.0","method":"commandStart","params":{"id":0,"name":"echo"}}
```

### Transport modes

| Mode | Flag | Description |
|------|------|-------------|
| None | (default) | No events. Script runs normally. |
| Stdio | `--agent-stdio` | Events written to a dedicated FD (stderr dup'd, then stderr redirected to /dev/null). |

### Events

| Method | Emitted when |
|--------|-------------|
| `scriptStart` | Script begins (file path) |
| `commandStart` | A command begins (id, name) |
| `commandEnd` | A command completes (id, status) |
| `scriptEnd` | Script completes (status) |
| `error` | A structured error occurs |

Events are emitted as JSON-RPC notifications (no `id`, no response). Command IDs are monotonically increasing integers.

The event sink is an abstraction (`event_sink_t` with `emit` function pointer). The VM holds a pointer to the active sink. Child processes clear the sink after fork to prevent duplicate event emission.


## Bytecode Serialization

### .opsb format

16-byte header followed by typed sections with length prefixes.

**Header:**
- 4 bytes: magic `"OPSH"`
- 2 bytes: format version (u16, currently 1)
- 2 bytes: flags (reserved)
- 4 bytes: section count (u32)
- 4 bytes: reserved

**Section types:**

| Type | Contents |
|------|----------|
| `SECT_CONST_POOL` (0x01) | u16 count + length-prefixed UTF-8 strings |
| `SECT_BYTECODE` (0x02) | Raw bytecode bytes |
| `SECT_FUNC_TABLE` (0x03) | u16 count + (u16 name_idx, u32 offset) per function |
| `SECT_MODULE_TABLE` (0x04) | u16 count + (u16 name_idx, u32 init_offset) per module |

Function and module names are stored as constant pool indices. Little-endian throughout. Unknown sections are skipped for forward compatibility.

Deserialization validates magic and version, enforces section ordering (const pool before func/module tables), and propagates read errors via an error-tracking context.


## LSP Server

`opsh lsp` implements the Language Server Protocol over stdin/stdout.

### Lifecycle

`initialize` (reports capabilities) -> `initialized` -> normal operation -> `shutdown` -> `exit`.

### Capabilities

- **textDocumentSync:** full document sync (mode 1). On `didOpen`/`didChange`, the server parses the document and publishes diagnostics.
- **completionProvider:** returns builtin names on `textDocument/completion`.

### Diagnostics

Parse errors are collected into a structured array on the parser (line number + message). The LSP server converts these to LSP diagnostic objects with 0-based line numbers and publishes them via `textDocument/publishDiagnostics`.


## Directory Layout

```
src/
  foundation/     strbuf, plist, hashtable, util, json
  parser/         lexer, recursive descent parser, AST types and memory management
  compiler/       AST-to-bytecode compiler, opcode definitions
  vm/             bytecode VM, value types, disassembler, .opsb serialization
  exec/           variable scope chain, signal handling
  builtins/       builtin registry and implementations
  agent/          JSON-RPC event sink
  lsp/            LSP server
  main.c          entry point (run, build, lsp subcommands)
include/opsh/     public headers (value.h, vm_fwd.h)
tests/            TAP harness and test suites
```
