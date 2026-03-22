# opsh lint checks

Shellcheck-compatible checks applicable to opsh. Checks marked "done" are
implemented. Others are categorized by implementation difficulty.

SC1xxx (parser errors) are omitted — our parser already catches syntax errors.
SC3xxx (POSIX compliance) are omitted — opsh defines its own feature set.

## Legend

- **done**: implemented
- **simple**: command name or single-node pattern match
- **pattern**: multi-node AST pattern match
- **tracking**: needs variable assignment/reference tracking pass
- **parser**: needs a parser change to preserve info
- **n/a**: feature not in opsh or not applicable

## Checks

| Code | Severity | Effort | Description |
|------|----------|--------|-------------|
| SC2002 | style | simple | Useless cat. Consider `cmd < file` instead |
| SC2005 | style | pattern | Useless echo. Instead of `echo $(cmd)`, just use `cmd` |
| SC2006 | style | parser | Use `$(...)` notation instead of legacy backticks |
| SC2008 | style | simple | echo doesn't read from stdin. Are you sure you should be piping to it? |
| SC2009 | info | simple | Consider using pgrep instead of grepping ps output |
| SC2010 | info | simple | Don't use `ls \| grep`. Use a glob or a for loop with a condition |
| SC2012 | info | simple | Use find instead of ls to better handle non-alphanumeric filenames |
| SC2015 | info | pattern | `A && B \|\| C` is not if-then-else. C may run when A is true |
| SC2016 | info | pattern | Expressions don't expand in single quotes, use double quotes for that |
| SC2028 | info | simple | echo won't expand escape sequences. Consider printf |
| SC2034 | warning | tracking | Variable appears unused. Verify it or export it |
| SC2035 | warning | pattern | Use `./*glob*` or `-- *glob*` so names with dashes won't become options |
| SC2036 | warning | pattern | If you wanted to assign the output of the pipeline, use `a=$(b \| c)` |
| SC2043 | warning | pattern | This loop will only ever run once for a constant value |
| SC2046 | warning | pattern | Quote this to prevent word splitting |
| SC2048 | warning | **done** | Use `"$@"` (with quotes) to prevent whitespace problems |
| SC2050 | warning | pattern | This expression is constant. Did you forget the `$` on a variable? |
| SC2059 | info | pattern | Don't use variables in the printf format string. Use `printf "..%s.." "$foo"` |
| SC2064 | warning | pattern | Use single quotes for trap, otherwise expansion happens at definition |
| SC2066 | warning | pattern | Since you double quoted this, it will not word split, and the loop will only run once |
| SC2068 | error | pattern | Double quote array expansions to avoid re-splitting elements |
| SC2069 | warning | pattern | To redirect stdout+stderr, `2>&1` must be last |
| SC2078 | error | pattern | This expression is constant. Did you forget a `$` somewhere? |
| SC2086 | info | **done** | Double quote to prevent globbing and word splitting |
| SC2088 | warning | pattern | Tilde does not expand in quotes. Use `$HOME` |
| SC2091 | warning | pattern | Remove surrounding `$()` to avoid executing output |
| SC2094 | info | pattern | Make sure not to read and write the same file in the same pipeline |
| SC2097 | warning | pattern | This assignment is only seen by the forked process |
| SC2098 | warning | pattern | This expansion will not see the mentioned assignment |
| SC2104 | error | pattern | In functions, use return instead of break |
| SC2105 | error | pattern | break is only valid in loops |
| SC2112 | warning | pattern | `function` keyword is non-standard. Delete it |
| SC2115 | warning | pattern | Use `"${var:?}"` to ensure this never expands to `/*` |
| SC2116 | style | pattern | Useless echo. Instead of `cmd $(echo foo)`, just use `cmd foo` |
| SC2119 | info | tracking | Use `foo "$@"` if function's `$1` should mean script's `$1` |
| SC2120 | info | tracking | Function references arguments, but none are ever passed |
| SC2123 | warning | simple | PATH is the shell search path. Use another name |
| SC2126 | style | pattern | Consider using `grep -c` instead of `grep \| wc` |
| SC2128 | warning | pattern | Expanding an array without an index only gives the first element |
| SC2129 | style | pattern | Consider using `{ cmd1; cmd2; } >> file` instead of individual redirects |
| SC2139 | warning | pattern | This expands when defined, not when used. Consider escaping |
| SC2143 | style | pattern | Use `grep -q` instead of comparing output of `grep \| wc` |
| SC2148 | error | simple | Add a shebang to specify the shell |
| SC2151 | error | pattern | Only one integer 0-255 can be returned. Use stdout for other data |
| SC2152 | error | pattern | Can only return 0-255. Other data should be written to stdout |
| SC2154 | warning | tracking | Variable is referenced but not assigned |
| SC2155 | warning | pattern | Declare and assign separately to avoid masking return values |
| SC2157 | error | pattern | Argument to implicit `-n` is always true due to literal strings |
| SC2158 | error | pattern | `[ false ]` is true. Remove the brackets |
| SC2159 | error | pattern | `[ 0 ]` is true. Use `false` instead |
| SC2160 | style | pattern | Instead of `[ true ]`, just use `true` |
| SC2161 | style | pattern | Instead of `[ 1 ]`, just use `true` |
| SC2162 | warning | simple | read without `-r` will mangle backslashes |
| SC2163 | warning | pattern | This does not export `FOO`. Remove `$/${}`  |
| SC2164 | warning | **done** | Use `cd ... \|\| exit` in case cd fails |
| SC2165 | warning | tracking | This nested loop overrides the index variable of its parent |
| SC2166 | warning | pattern | Prefer `[ p ] && [ q ]` as `[ p -a q ]` is not well defined |
| SC2168 | error | pattern | `local` is only valid in functions |
| SC2172 | warning | simple | Trapping signals by number is not well defined. Prefer signal names |
| SC2181 | style | pattern | Check exit code directly with `if mycmd;`, not indirectly with `$?` |
| SC2184 | warning | simple | Quote arguments to unset so they're not glob expanded |
| SC2188 | warning | pattern | This redirection doesn't have a command. Move to its command |
| SC2194 | warning | pattern | This word is constant. Did you forget the `$` on a variable? |
| SC2195 | warning | pattern | This pattern will never match the case statement's word |
| SC2216 | warning | simple | Piping to `rm`, a command that doesn't read stdin |
| SC2217 | warning | simple | Redirecting to `echo`, a command that doesn't read stdin |
| SC2220 | warning | pattern | Invalid flags are not handled. Add a `*)` case |
| SC2223 | warning | pattern | This default assignment may cause DoS due to globbing. Quote it |
| SC2230 | info | simple | `which` is non-standard. Use builtin `command -v` instead |
| SC2236 | style | pattern | Use `-n` instead of `! -z` |
| SC2237 | style | pattern | Use `[ -n .. ]` instead of `! [ -z .. ]` |
| SC2249 | info | pattern | Consider adding a default `*)` case |
| SC2264 | warning | tracking | This function unconditionally re-invokes itself. Missing `command`? |
| SC2269 | warning | pattern | This variable is assigned to itself, so the assignment does nothing |
| SC2291 | info | pattern | Quote repeated spaces to avoid them collapsing into one |
| SC2310 | warning | tracking | This function is invoked in an `if` condition so set -e will be disabled |
| SC2317 | warning | tracking | Command appears to be unreachable |
| SC2329 | info | tracking | This function is never invoked |

## Summary

| Effort | Count | Notes |
|--------|-------|-------|
| done | 3 | SC2048, SC2086, SC2164 |
| simple | 14 | Command name checks, single-node patterns |
| pattern | 46 | Multi-node AST patterns, most of the work |
| tracking | 10 | Need a variable/function usage tracking pass |
| parser | 1 | SC2006 needs backtick flag on cmd_subst_t |
| **total** | **74** | |
