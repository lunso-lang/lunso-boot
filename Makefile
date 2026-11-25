CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: build/lunso

build/lunso: src/main.c src/lexer.c src/lexer.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/lexer.c $(LDLIBS)

clean:
	rm -f build/lunso
