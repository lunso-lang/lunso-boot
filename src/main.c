#include "lexer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_source(FILE *file, size_t *length)
{
    if (fseek(file, 0, SEEK_END) != 0) {
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || (uintmax_t)size > SIZE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        return NULL;
    }

    *length = (size_t)size;
    unsigned char *source = malloc(*length ? *length : 1);
    if (source && fread(source, 1, *length, file) != *length) {
        free(source);
        return NULL;
    }
    return source;
}

static int dump_tokens(const char *path, const unsigned char *source, size_t length)
{
    static const char *const names[] = {
        [TOKEN_FUN] = "FUN",
        [TOKEN_IDENTIFIER] = "IDENTIFIER",
        [TOKEN_LEFT_PAREN] = "LEFT_PAREN",
        [TOKEN_RIGHT_PAREN] = "RIGHT_PAREN",
        [TOKEN_COLON] = "COLON",
        [TOKEN_LEFT_BRACE] = "LEFT_BRACE",
        [TOKEN_RIGHT_BRACE] = "RIGHT_BRACE",
        [TOKEN_NEWLINE] = "NEWLINE",
        [TOKEN_EOF] = "EOF",
    };

    Lexer lexer = lexer_init(source, length);
    for (;;) {
        Token token = lexer_next(&lexer);
        if (token.kind == TOKEN_INVALID) {
            fprintf(stderr, "%s:%zu:%zu: unexpected byte 0x%02X\n", path,
                    token.line, token.column, (unsigned int)source[token.offset]);
            return EXIT_FAILURE;
        }

        printf("%zu:%zu %s", token.line, token.column, names[token.kind]);
        if (token.kind == TOKEN_IDENTIFIER) {
            putchar(' ');
            fwrite(source + token.offset, 1, token.length, stdout);
        }
        putchar('\n');

        if (token.kind == TOKEN_EOF) {
            return fflush(stdout) == EOF || ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <source.lunso>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return EXIT_FAILURE;
    }

    size_t length;
    unsigned char *source = read_source(file, &length);
    fclose(file);
    if (!source) {
        fprintf(stderr, "%s: could not read source\n", path);
        return EXIT_FAILURE;
    }

    int result = dump_tokens(path, source, length);
    free(source);
    return result;
}
