CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
SOURCES := src/main.c src/lexer.c src/ast.c src/parser.c src/sema.c src/ir.c src/ir_write.c \
           src/literal.c src/qbe.c src/native.c src/modules.c
HEADERS := src/lexer.h src/ast.h src/ast_builder.h src/parser.h src/sema.h src/ir.h \
           src/literal.h src/qbe.h src/native.h src/modules.h

.PHONY: all clean

all: build/lunso

build/lunso: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f build/lunso
