CC ?= clang
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc
LDFLAGS =

ifdef RELEASE
CFLAGS += -O2 -DNDEBUG
else
CFLAGS += -fsanitize=address,undefined -g -O0
LDFLAGS += -fsanitize=address,undefined
endif

BUILD = build
BINARY = $(BUILD)/opsh

# Source files
FOUNDATION_SRCS = src/foundation/util.c src/foundation/strbuf.c \
                  src/foundation/plist.c src/foundation/hashtable.c \
                  src/foundation/arena.c src/foundation/rcstr.c \
                  src/foundation/json.c
PARSER_SRCS = src/parser/ast.c src/parser/lexer.c src/parser/parser.c
VM_SRCS = src/vm/value.c src/vm/vm.c src/vm/disasm.c src/vm/image_io.c src/vm/arith.c
EXEC_SRCS = src/exec/variable.c src/exec/signal.c
BUILTIN_SRCS = src/builtins/builtins.c
COMPILER_SRCS = src/compiler/compiler.c
AGENT_SRCS = src/agent/event.c
FORMAT_SRCS = src/format/format.c
LINT_SRCS = src/lint/lint.c src/lint/checks.c
LSP_SRCS = src/lsp/lsp.c
MAIN_SRCS = src/main.c

ALL_SRCS = $(FOUNDATION_SRCS) $(PARSER_SRCS) $(VM_SRCS) $(EXEC_SRCS) \
           $(BUILTIN_SRCS) $(COMPILER_SRCS) $(AGENT_SRCS) $(FORMAT_SRCS) $(LINT_SRCS) $(LSP_SRCS) $(MAIN_SRCS)
ALL_OBJS = $(ALL_SRCS:src/%.c=$(BUILD)/%.o)
DEPS = $(ALL_OBJS:.o=.d)

# Object groups for test linking
FOUNDATION_OBJS = $(FOUNDATION_SRCS:src/%.c=$(BUILD)/%.o)
PARSER_OBJS = $(PARSER_SRCS:src/%.c=$(BUILD)/%.o)
VM_OBJS = $(VM_SRCS:src/%.c=$(BUILD)/%.o)
EXEC_OBJS = $(EXEC_SRCS:src/%.c=$(BUILD)/%.o)
BUILTIN_OBJS = $(BUILTIN_SRCS:src/%.c=$(BUILD)/%.o)
COMPILER_OBJS = $(COMPILER_SRCS:src/%.c=$(BUILD)/%.o)
AGENT_OBJS = $(AGENT_SRCS:src/%.c=$(BUILD)/%.o)
FORMAT_OBJS = $(FORMAT_SRCS:src/%.c=$(BUILD)/%.o)
LINT_OBJS = $(LINT_SRCS:src/%.c=$(BUILD)/%.o)
LSP_OBJS = $(LSP_SRCS:src/%.c=$(BUILD)/%.o)

# All non-main objects (for test linking)
LIB_OBJS = $(FOUNDATION_OBJS) $(PARSER_OBJS) $(VM_OBJS) $(EXEC_OBJS) \
           $(BUILTIN_OBJS) $(COMPILER_OBJS) $(AGENT_OBJS) $(FORMAT_OBJS) $(LINT_OBJS) $(LSP_OBJS)

# Test sources
TEST_TAP_SRC = tests/test_tap.c
TEST_FOUNDATION_SRCS = tests/foundation/test_strbuf.c \
                       tests/foundation/test_plist.c \
                       tests/foundation/test_hashtable.c \
                       tests/foundation/test_util.c
TEST_PARSER_SRCS = tests/parser/test_lexer.c tests/parser/test_parser.c
TEST_VM_SRCS = tests/vm/test_vm.c tests/vm/test_variable.c tests/vm/test_arith.c tests/vm/test_image_io.c
TEST_COMPILER_SRCS = tests/compiler/test_compiler.c tests/compiler/test_features.c
TEST_FORMAT_SRCS = tests/format/test_format.c
TEST_LINT_SRCS = tests/lint/test_lint.c
TEST_LSP_SRCS = tests/lsp/test_lsp.c
TEST_CLI_SRCS = tests/cli/test_cli.c

TEST_TAP_BIN = $(BUILD)/tests/test_tap
TEST_FOUNDATION_BINS = $(TEST_FOUNDATION_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_PARSER_BINS = $(TEST_PARSER_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_VM_BINS = $(TEST_VM_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_COMPILER_BINS = $(TEST_COMPILER_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_FORMAT_BINS = $(TEST_FORMAT_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_LINT_BINS = $(TEST_LINT_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_LSP_BINS = $(TEST_LSP_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_CLI_BINS = $(TEST_CLI_SRCS:tests/%.c=$(BUILD)/tests/%)

# Fuzz targets (require LLVM clang with libfuzzer; Apple clang does not include it)
# Usage: make fuzz-build FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang
FUZZ_CC ?= clang
FUZZ_PARSER_BIN = $(BUILD)/fuzz/fuzz_parser
FUZZ_COMPILE_BIN = $(BUILD)/fuzz/fuzz_compile
FUZZ_IMAGE_BIN = $(BUILD)/fuzz/fuzz_image
FUZZ_FORMAT_BIN = $(BUILD)/fuzz/fuzz_format
FUZZ_LINT_BIN = $(BUILD)/fuzz/fuzz_lint
FUZZ_ARITH_BIN = $(BUILD)/fuzz/fuzz_arith
FUZZ_LSP_BIN = $(BUILD)/fuzz/fuzz_lsp
FUZZ_FOUNDATION_BIN = $(BUILD)/fuzz/fuzz_foundation
FUZZ_VM_BIN = $(BUILD)/fuzz/fuzz_vm
FUZZ_BINS = $(FUZZ_PARSER_BIN) $(FUZZ_COMPILE_BIN) $(FUZZ_IMAGE_BIN) $(FUZZ_FORMAT_BIN) $(FUZZ_LINT_BIN) $(FUZZ_ARITH_BIN) $(FUZZ_LSP_BIN) $(FUZZ_FOUNDATION_BIN) $(FUZZ_VM_BIN)

FUZZ_CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc \
              -fsanitize=fuzzer,address,undefined -g -O1
FUZZ_LDFLAGS = -fsanitize=fuzzer,address,undefined

# Separate object directory for fuzz-instrumented objects (different CFLAGS)
FUZZ_LIB_OBJS = $(LIB_OBJS:$(BUILD)/%=$(BUILD)/fuzz-objs/%)

.PHONY: all clean test test-tap test-foundation test-parser test-vm test-compiler \
        test-format test-lint test-lsp test-cli format format-check fuzz-build

all: $(BINARY)

$(BINARY): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Test binaries
$(TEST_TAP_BIN): $(TEST_TAP_SRC) tests/tap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD)/tests/foundation/%: tests/foundation/%.c tests/tap.h $(FOUNDATION_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(FOUNDATION_OBJS)

$(BUILD)/tests/parser/%: tests/parser/%.c tests/tap.h $(PARSER_OBJS) $(FOUNDATION_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(PARSER_OBJS) $(FOUNDATION_OBJS)

$(BUILD)/tests/vm/%: tests/vm/%.c tests/tap.h tests/asm.h $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILD)/tests/compiler/%: tests/compiler/%.c tests/tap.h $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

$(BUILD)/tests/format/%: tests/format/%.c tests/tap.h $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

test: test-tap test-foundation test-parser test-vm test-compiler test-format test-lint test-lsp test-cli

test-tap: $(TEST_TAP_BIN)
	$(TEST_TAP_BIN)

test-foundation: $(TEST_FOUNDATION_BINS)
	@for t in $(TEST_FOUNDATION_BINS); do echo "# Running $$t"; $$t || exit 1; done

test-parser: $(TEST_PARSER_BINS)
	@for t in $(TEST_PARSER_BINS); do echo "# Running $$t"; $$t || exit 1; done

test-vm: $(TEST_VM_BINS)
	@for t in $(TEST_VM_BINS); do echo "# Running $$t"; $$t || exit 1; done

test-compiler: $(TEST_COMPILER_BINS)
	@for t in $(TEST_COMPILER_BINS); do echo "# Running $$t"; $$t || exit 1; done

test-format: $(TEST_FORMAT_BINS)
	@for t in $(TEST_FORMAT_BINS); do echo "# Running $$t"; $$t || exit 1; done

$(BUILD)/tests/lint/%: tests/lint/%.c tests/tap.h $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS)

test-lint: $(TEST_LINT_BINS)
	@for t in $(TEST_LINT_BINS); do echo "# Running $$t"; $$t || exit 1; done

$(BUILD)/tests/lsp/%: tests/lsp/%.c tests/tap.h $(BINARY)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test-lsp: $(TEST_LSP_BINS)
	@for t in $(TEST_LSP_BINS); do echo "# Running $$t"; $$t || exit 1; done

$(BUILD)/tests/cli/%: tests/cli/%.c tests/tap.h $(BINARY)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test-cli: $(TEST_CLI_BINS)
	@for t in $(TEST_CLI_BINS); do echo "# Running $$t"; $$t || exit 1; done

# Fuzz-instrumented library objects (compiled with fuzzer sanitizer flags)
$(BUILD)/fuzz-objs/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -MMD -MP -c -o $@ $<

# Fuzz binaries
$(FUZZ_PARSER_BIN): fuzz/fuzz_parser.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_COMPILE_BIN): fuzz/fuzz_compile.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_IMAGE_BIN): fuzz/fuzz_image.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_FORMAT_BIN): fuzz/fuzz_format.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_LINT_BIN): fuzz/fuzz_lint.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_ARITH_BIN): fuzz/fuzz_arith.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_LSP_BIN): fuzz/fuzz_lsp.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_FOUNDATION_BIN): fuzz/fuzz_foundation.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

$(FUZZ_VM_BIN): fuzz/fuzz_vm.c $(FUZZ_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) -o $@ $< $(FUZZ_LIB_OBJS)

fuzz-build: $(FUZZ_BINS)

clean:
	rm -rf $(BUILD)

format:
	find src tests include fuzz -name '*.c' -o -name '*.h' | xargs clang-format -i

format-check:
	find src tests include fuzz -name '*.c' -o -name '*.h' | xargs clang-format --dry-run --Werror

-include $(DEPS)
