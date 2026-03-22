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
VM_SRCS = src/vm/value.c src/vm/vm.c src/vm/disasm.c src/vm/image_io.c
EXEC_SRCS = src/exec/variable.c src/exec/signal.c
BUILTIN_SRCS = src/builtins/builtins.c
COMPILER_SRCS = src/compiler/compiler.c
AGENT_SRCS = src/agent/event.c
MAIN_SRCS = src/main.c

ALL_SRCS = $(FOUNDATION_SRCS) $(PARSER_SRCS) $(VM_SRCS) $(EXEC_SRCS) \
           $(BUILTIN_SRCS) $(COMPILER_SRCS) $(AGENT_SRCS) $(MAIN_SRCS)
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

# All non-main objects (for test linking)
LIB_OBJS = $(FOUNDATION_OBJS) $(PARSER_OBJS) $(VM_OBJS) $(EXEC_OBJS) \
           $(BUILTIN_OBJS) $(COMPILER_OBJS) $(AGENT_OBJS)

# Test sources
TEST_TAP_SRC = tests/test_tap.c
TEST_FOUNDATION_SRCS = tests/foundation/test_strbuf.c \
                       tests/foundation/test_plist.c \
                       tests/foundation/test_hashtable.c \
                       tests/foundation/test_util.c
TEST_PARSER_SRCS = tests/parser/test_lexer.c tests/parser/test_parser.c
TEST_VM_SRCS = tests/vm/test_vm.c tests/vm/test_variable.c
TEST_COMPILER_SRCS = tests/compiler/test_compiler.c

TEST_TAP_BIN = $(BUILD)/tests/test_tap
TEST_FOUNDATION_BINS = $(TEST_FOUNDATION_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_PARSER_BINS = $(TEST_PARSER_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_VM_BINS = $(TEST_VM_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_COMPILER_BINS = $(TEST_COMPILER_SRCS:tests/%.c=$(BUILD)/tests/%)

.PHONY: all clean test test-tap test-foundation test-parser test-vm test-compiler \
        format format-check

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

test: test-tap test-foundation test-parser test-vm test-compiler

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

clean:
	rm -rf $(BUILD)

format:
	find src tests include -name '*.c' -o -name '*.h' | xargs clang-format -i

format-check:
	find src tests include -name '*.c' -o -name '*.h' | xargs clang-format --dry-run --Werror

-include $(DEPS)
