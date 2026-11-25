CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
FUZZ_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer
FUZZ_ITERS ?= 20000
PYTHON ?= python3
STD_DIR ?= $(CURDIR)/std
shell_quote = '$(subst ','"'"',$(1))'
SOURCES := src/main.c src/lexer.c src/ast.c src/parser.c src/sema.c src/ir.c src/ir_write.c \
           src/literal.c src/qbe.c src/native.c src/modules.c
CORE_SOURCES := $(filter-out src/main.c,$(SOURCES))
HEADERS := src/lexer.h src/ast.h src/ast_builder.h src/parser.h src/sema.h src/ir.h \
           src/literal.h src/qbe.h src/native.h src/modules.h

.PHONY: all clean force test fuzz

all: build/lunso

test: all build/std-linux-x86_64.o
	$(PYTHON) tests/run.py

build/fuzz: tests/fuzz.c $(CORE_SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -Isrc -o $@ tests/fuzz.c $(CORE_SOURCES) $(LDLIBS)

fuzz: build/fuzz
	./build/fuzz --iters=$(FUZZ_ITERS) tests/pass/*.lunso tests/fail/*.lunso examples/*.lunso

build/lunso: $(SOURCES) $(HEADERS) build/std_path.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -include build/std_path.h $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

build/std-linux-x86_64.o: std/platform/linux_x86_64.s
	mkdir -p build
	$(CC) $(ASFLAGS) -c $< -o $@

build/std_path.h: force
	@mkdir -p build
	@set -e; std_dir=$(call shell_quote,$(STD_DIR)); \
	case "$$std_dir" in ""|/*) ;; *) std_dir=$(call shell_quote,$(CURDIR))/$$std_dir ;; esac; \
	tmp=$$(mktemp "$@.XXXXXX"); \
	trap 'rm -f "$$tmp"' EXIT; \
	printf '%s\n' "$$std_dir" | sed 's/[\\"]/\\&/g; s/^/#define LUNSO_STD_DIR "/; s/$$/"/' > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; fi

clean:
	rm -f build/lunso build/fuzz build/std_path.h build/std-linux-x86_64.o
