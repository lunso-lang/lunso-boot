CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
SOURCES := src/main.c src/lexer.c src/ast.c src/parser.c
HEADERS := src/lexer.h src/ast.h src/ast_builder.h src/parser.h

.PHONY: all clean

all: build/lunso

build/lunso: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f build/lunso
