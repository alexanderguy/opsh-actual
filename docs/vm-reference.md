# opsh Virtual Machine Reference

The opsh VM is a stack-based bytecode interpreter. Instructions are
variable-width: a 1-byte opcode followed by zero or more operands.
Multi-byte operands are little-endian.

## Operand types

| Type | Size | Description |
|------|------|-------------|
| u8   | 1    | Unsigned byte |
| u16  | 2    | Unsigned 16-bit (constant pool index, name index) |
| i32  | 4    | Signed 32-bit (jump offsets, relative to IP after operand) |
| u32  | 4    | Unsigned 32-bit (bytecode segment offsets) |

## Value types

The VM uses tagged union values (`value_t`) on the operand stack and in
variable storage.

| Tag | Name | Description |
|-----|------|-------------|
| 0   | VT_NONE | No value (unset variable, exhausted iterator) |
| 1   | VT_STRING | UTF-8 string (heap-allocated, owned) |
| 2   | VT_INTEGER | 64-bit signed integer |
| 3   | VT_ARRAY | Array of strings (heap-allocated elements) |
| 4   | VT_ITERATOR | For-loop iteration state |

## Opcodes

### Stack operations

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| PUSH_CONST | 0x01 | u16 pool_idx | → value | Push string from constant pool |
| PUSH_INT | 0x02 | i32 value | → value | Push integer literal |
| POP | 0x03 | — | value → | Discard top of stack |
| DUP | 0x04 | — | a → a a | Duplicate top of stack |

### Variable operations

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| GET_VAR | 0x10 | u16 name_idx | → value | Look up variable by name, push value (empty string if unset) |
| SET_VAR | 0x11 | u16 name_idx | value → | Assign value to variable via scope chain walk |
| GET_ARRAY | 0x12 | u16 name_idx | index → value | Get array element by index |
| SET_ARRAY | 0x13 | u16 name_idx | value index → | Set array element, auto-extends |
| SET_ARRAY_BULK | 0x14 | u16 name_idx, u16 count | val₁..valₙ → | Create array from N stack values |
| EXPORT | 0x15 | u16 name_idx | — | Mark variable for export to child processes |
| PUSH_SCOPE | 0x16 | u8 flags | — | Push new variable scope (flags: 0=local, 1=temporary) |
| POP_SCOPE | 0x17 | — | — | Pop and destroy innermost scope |
| GET_SPECIAL | 0x18 | u8 which | → value | Push special parameter ($?, $#, $@, $*, $$, $!, $-, $0) |

### String and expansion operations

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| CONCAT | 0x20 | u16 count | val₁..valₙ → result | Concatenate N strings into one |
| EXPAND_PARAM | 0x21 | u16 name_idx, u8 op, u8 flags | varies → value | Parameter expansion (${var:-default}, ${var#pat}, etc.) |
| EXPAND_ARITH | 0x22 | — | expr → value | Evaluate arithmetic expression string |
| SPLIT_FIELDS | 0x23 | — | value → field₁..fieldₙ count | Split string by IFS |
| GLOB | 0x24 | — | pattern → match₁..matchₙ count | Pathname expansion; no match returns pattern literally |
| QUOTE_REMOVE | 0x25 | — | — | No-op (reserved) |
| EXPAND_TILDE | 0x26 | — | value → value | Replace leading ~ with $HOME, ~ after : in assignments |
| COLLECT_WORDS | 0x27 | — | (val count)... ngroups → arg₁..argₙ argc | Flatten word groups into argv |
| EXPAND_ARGS | 0x28 | — | → val₁..valₙ count | Push positional params as separate words (for "$@") |

### Control flow

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| JMP | 0x30 | i32 offset | — | Unconditional jump (relative to IP after operand) |
| JMP_TRUE | 0x31 | i32 offset | — | Jump if laststatus == 0 |
| JMP_FALSE | 0x32 | i32 offset | — | Jump if laststatus != 0 |
| JMP_NONE | 0x33 | i32 offset | value → | Pop; jump if VT_NONE |
| RET | 0x34 | — | — | Return from function: restore IP, scope, image |
| LOOP_ENTER | 0x35 | — | — | Mark loop entry (saves stack depth) |
| LOOP_EXIT | 0x36 | — | — | Mark loop exit (decrements loop depth) |
| BREAK | 0x37 | u8 depth | — | Break from loop (not emitted; compiler uses JMP) |
| CONTINUE | 0x38 | u8 depth | — | Continue loop (not emitted; compiler uses JMP) |
| GET_NEXT_ITER | 0x39 | — | → value | Advance iterator on stack; VT_NONE when exhausted |
| INIT_ITER | 0x3A | u16 group_count | (val count)... → iterator | Create iterator from word groups |

### Command execution

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| EXEC_SIMPLE | 0x40 | u8 flags | arg₁..argₙ argc → | Execute: try functions, builtins, then fork/execvp |
| EXEC_BUILTIN | 0x41 | u16 builtin_idx | arg₁..argₙ argc → | Call builtin by compile-time index |
| EXEC_FUNC | 0x42 | u16 func_idx | arg₁..argₙ argc → | Call function by compile-time index |
| PIPELINE | 0x43 | u16 cmd_count | — | Fork N children in shared process group with pipes; followed by PIPELINE_CMD/END |
| PIPELINE_CMD | 0x44 | u32 offset | — | Pipeline member (consumed by PIPELINE) |
| PIPELINE_END | 0x45 | u8 flags | — | Pipeline terminator (flags: bit 0 = negate status) |
| CMD_SUBST | 0x46 | u32 offset | → value | Fork, capture stdout, push as string |
| SUBSHELL | 0x47 | u32 offset | — | Fork, run sub-segment, wait, collect status |
| BACKGROUND | 0x48 | u32 offset | — | Fork in new process group, run sub-segment, register as job, don't wait |

### Redirection

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| REDIR_SAVE | 0x50 | — | — | Push redirection frame (saves FD state) |
| REDIR_RESTORE | 0x51 | — | — | Pop frame, restore all saved FDs |
| REDIR_OPEN | 0x52 | u8 fd, u8 type | filename → | Open file, dup2 onto fd |
| REDIR_DUP | 0x53 | u8 fd, u8 flags | target → | dup2(target, fd) |
| REDIR_CLOSE | 0x54 | u8 fd | — | Close fd (after saving) |
| REDIR_HERE | 0x55 | u8 fd, u8 flags | content → | Write content to temp file, redirect fd |

### Test operations (for `[[ ]]`)

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| TEST_UNARY | 0x60 | u8 op | value → | File/string test; sets laststatus |
| TEST_BINARY | 0x61 | u8 op | left right → | Comparison; sets laststatus |
| NEGATE_STATUS | 0x62 | — | — | laststatus = !laststatus |
| PATTERN_MATCH | 0x63 | — | pattern subject → | Glob match via fnmatch; sets laststatus |

### Module operations

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| IMPORT | 0x70 | u16 name_idx | — | Load and run module init code (once) |
| CAP_CHECK | 0x71 | u8 capability | — | Reserved for capability system |

### Shell option support

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| ERREXIT_PUSH | 0x80 | — | — | Suppress errexit (entering if/while/&&/|| context) |
| ERREXIT_POP | 0x81 | — | — | Restore errexit |

### Sentinel

| Opcode | Hex  | Operands | Stack effect | Description |
|--------|------|----------|--------------|-------------|
| HALT | 0xFF | — | — | Run EXIT trap and stop execution |

## Special variable IDs (for GET_SPECIAL)

| ID | Parameter | Description |
|----|-----------|-------------|
| 0  | $? | Exit status of last command |
| 1  | $# | Number of positional parameters |
| 2  | $@ | Positional params joined with space (naive; use EXPAND_ARGS for "$@") |
| 3  | $* | Positional params joined with first char of IFS |
| 4  | $$ | Process ID of the shell |
| 5  | $! | PID of last background command |
| 6  | $- | Current option flags |
| 7  | $0 | Script name |

## Test operation IDs (for TEST_UNARY / TEST_BINARY)

### Unary file tests

| ID | Operator | Description |
|----|----------|-------------|
| 0  | -f | Regular file |
| 1  | -d | Directory |
| 2  | -e | Exists |
| 3  | -s | Non-empty (size > 0) |
| 4  | -r | Readable |
| 5  | -w | Writable |
| 6  | -x | Executable |
| 7  | -L / -h | Symbolic link (uses lstat) |
| 8  | -p | Named pipe (FIFO) |
| 9  | -b | Block device |
| 10 | -c | Character device |
| 11 | -S | Socket |
| 12 | -g | Setgid bit |
| 13 | -u | Setuid bit |
| 14 | -k | Sticky bit |
| 15 | -O | Owned by effective UID |
| 16 | -G | Owned by effective GID |
| 17 | -t | FD is a terminal |
| 18 | -N | Modified since last read |

### Unary string tests

| ID | Operator | Description |
|----|----------|-------------|
| 20 | -n | Non-empty string |
| 21 | -z | Empty string |

### Binary string tests

| ID | Operator | Description |
|----|----------|-------------|
| 30 | == / = | String equality (glob match in [[ ]]) |
| 31 | != | String inequality (glob match in [[ ]]) |

### Binary numeric tests

| ID | Operator | Description |
|----|----------|-------------|
| 40 | -eq | Equal |
| 41 | -ne | Not equal |
| 42 | -lt | Less than |
| 43 | -le | Less or equal |
| 44 | -gt | Greater than |
| 45 | -ge | Greater or equal |

### Binary file tests

| ID | Operator | Description |
|----|----------|-------------|
| 50 | -nt | Newer than (mtime) |
| 51 | -ot | Older than (mtime) |
| 52 | -ef | Same file (dev + inode) |

### Regex

| ID | Operator | Description |
|----|----------|-------------|
| 60 | =~ | Extended regex match; populates BASH_REMATCH |

## Job control

The VM maintains a job table (`job_table_t`) that tracks background processes
and pipelines. Each job has a process group ID (PGID), an array of member PIDs,
and a state (running, stopped, or done).

### Process groups

- **OP_BACKGROUND**: The child creates its own process group via `setpgid(0, 0)`.
  Both parent and child call `setpgid` to avoid races (loser gets EACCES, ignored).
  The job is registered in the table.
- **OP_PIPELINE**: All pipeline members share a process group. The first child
  creates the group; subsequent children join via `setpgid(0, pgid)`. The parent
  calls `setpgid(child, pgid)` for each child. Foreground pipelines are waited
  via `job_wait_fg()`.
- **OP_EXEC_SIMPLE**, **OP_CMD_SUBST**, **OP_SUBSHELL**: Foreground operations.
  Not registered in the job table. Wait directly by PID.

### waitpid ownership

`job_update()` (non-blocking, `WNOHANG|WUNTRACED|WCONTINUED`) and `job_wait_fg()`
(blocking, `WUNTRACED`) are the primary callers of `waitpid` for job-table processes.
The `wait` builtin routes through the job table to avoid double-reap races.

### Signals

- **SIGCHLD**: Handler sets a pending flag. Drained at well-defined points (before
  `jobs` output, before `wait` examination).
- **SIGTSTP**: Shell ignores it (`SIG_IGN`). Children get `SIG_DFL` after fork.
- **SIGCONT**: Sent by `fg`/`bg` builtins via `kill(-pgid, SIGCONT)`.
- **SIGTTIN/SIGTTOU**: Shell ignores them to avoid stopping on background terminal I/O.

### Builtins

- **jobs** [`-l`]: List active jobs with state and command. `-l` includes PIDs.
- **fg** [`%spec`]: Resume stopped job in foreground, or bring running job to foreground.
- **bg** [`%spec`]: Resume stopped job in background.
- **wait** [`%spec` | `pid`]: Wait for job or PID. No args waits for all.
- **kill** [`-signal`] [`%spec` | `pid`...]: Send signal to job's process group or PID.

### Job specs

| Spec | Meaning |
|------|---------|
| `%N` | Job number N |
| `%%` or `%+` | Current (most recent) job |
| `%-` | Previous job |
| `%?string` | Job whose command contains string |
| `%string` | Job whose command starts with string |

## Bytecode image format

A bytecode image contains:

- **Constant pool**: Array of deduplicated strings, referenced by u16 index
- **Code segment**: Variable-width instruction stream
- **Function table**: Name + bytecode offset pairs (compiled inline with JMP-over-body)
- **Module table**: Name + init offset pairs (for `lib::import`)

The `.opsb` serialization format uses a chunked binary encoding with an
8-byte trailer (4-byte payload offset LE + "OPSB" magic) for standalone
binary embedding.
