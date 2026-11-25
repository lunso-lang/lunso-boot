CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
STD_DIR ?= $(CURDIR)/std
shell_quote = '$(subst ','"'"',$(1))'
SOURCES := src/main.c src/lexer.c src/ast.c src/parser.c src/sema.c src/ir.c src/ir_write.c \
           src/literal.c src/qbe.c src/native.c src/modules.c
HEADERS := src/lexer.h src/ast.h src/ast_builder.h src/parser.h src/sema.h src/ir.h \
           src/literal.h src/qbe.h src/native.h src/modules.h

.PHONY: all clean force

all: build/lunso

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
	rm -f build/lunso build/std_path.h build/std-linux-x86_64.o
