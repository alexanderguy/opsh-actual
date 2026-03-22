# Development Guide

## Prerequisites

- **clang** (any version supporting C99; tested with Apple clang 17 / LLVM 19)
- **clang-format** 19 (LLVM 19.x series)
- **make** (BSD or GNU)
- **POSIX system** (macOS or Linux; the VM uses `fork`, `pipe`, `waitpid`, `dup2`, `glob`)

No external libraries. Everything is built from source.

The compiler can be overridden: `make CC=gcc`.

## Building

```sh
make          # debug build: -g -O0 -fsanitize=address,undefined
make RELEASE=1  # release build: -O2 -DNDEBUG, no sanitizers
make clean    # remove build/
```

All objects go to `build/` mirroring the `src/` structure. The binary is `build/opsh`.

For IDE integration, generate `compile_commands.json` with:
```sh
bear -- make clean all
```

## Testing

```sh
make test              # all tests
make test-tap          # TAP harness self-test
make test-foundation   # strbuf, plist, hashtable, util
make test-parser       # lexer + parser
make test-vm           # VM, variables, value types
make test-compiler     # end-to-end compiler tests (the bulk)
```

Tests use a header-only TAP harness (`tests/tap.h`). Each test binary is a standalone C program that outputs TAP. The Makefile runs them and checks for failures.

All tests run under ASan and UBSan in debug builds. Any memory error or undefined behavior causes a test failure.

## Formatting

```sh
make format       # apply clang-format to all .c and .h files
make format-check # verify formatting (CI-friendly, exits non-zero on diff)
```

The `.clang-format` config is LLVM-based with 4-space indent and 100-column limit. Format before committing.

## Adding a Builtin

1. Write the implementation in `src/builtins/builtins.c`:
   ```c
   static int builtin_foo(vm_t *vm, int argc, value_t *argv)
   {
       /* argv[0] is the command name, argv[1..] are arguments */
       return 0; /* exit status */
   }
   ```

2. Add it to `builtin_table[]` at the bottom of the file:
   ```c
   {"foo", builtin_foo},
   ```

3. The compiler resolves builtins by name at compile time. No other wiring needed.

## Adding an Opcode

1. Add the opcode constant to `src/compiler/bytecode.h` in the `opcode_t` enum.
2. Add its operand size to `opcode_operand_size()` in the same file.
3. Add its name to `opcode_name()` in `src/vm/disasm.c`.
4. Add the handler in `vm_run()` in `src/vm/vm.c`.
5. If the compiler emits it, add emission logic in `src/compiler/compiler.c`.

## Adding a Module

Create a `.opsh` file in a `lib/` directory relative to the importing script:

```sh
# lib/greet.opsh
greet::hello() {
    echo "hello from greet"
}
```

Import it from a script:
```sh
lib::import greet
greet::hello
```

Module resolution order: `script_dir/lib/name.opsh`, then `$OPSH_LOADPATH` entries.

## Project Structure

| Directory | Purpose |
|-----------|---------|
| `src/foundation/` | Data structures: strbuf, plist, hashtable, util, json |
| `src/parser/` | Lexer (context-dependent), recursive descent parser, AST |
| `src/compiler/` | Bytecode compiler with backpatching, opcode definitions |
| `src/vm/` | VM dispatch loop, value types, disassembler, .opsb I/O |
| `src/exec/` | Variable scope chain, signal handling |
| `src/builtins/` | Builtin command registry and implementations |
| `src/agent/` | JSON-RPC event sink for agent consumption |
| `src/lsp/` | LSP server (diagnostics, completion) |
| `include/opsh/` | Public API headers |
| `tests/` | TAP test harness and all test suites |

## Debugging

The debug build enables ASan and UBSan by default. To debug a specific script:

```sh
make           # debug build
./build/opsh script.opsh
```

To disassemble compiled bytecode, use the disassembler (available via test programs or by adding a `--disasm` flag to main.c).

To inspect the `.opsb` format, use `xxd` or `hexdump`:
```sh
opsh build script.opsh -o script.opsb
xxd script.opsb | head -20
```
