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

static void print_lexeme(const unsigned char *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = bytes[index];
        switch (byte) {
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        default:
            if (byte < ' ' || byte == 0x7f) {
                printf("\\x%02X", (unsigned int)byte);
            } else {
                putchar(byte);
            }
            break;
        }
    }
}

static int dump_tokens(const char *path, const unsigned char *source, size_t length)
{
    static const char *const names[] = {
        [TOKEN_FUN] = "FUN",
        [TOKEN_RETURN] = "RETURN",
        [TOKEN_IF] = "IF",
        [TOKEN_ELSE] = "ELSE",
        [TOKEN_FOR] = "FOR",
        [TOKEN_IDENTIFIER] = "IDENTIFIER",
        [TOKEN_INTEGER] = "INTEGER",
        [TOKEN_FLOAT] = "FLOAT",
        [TOKEN_STRING] = "STRING",
        [TOKEN_COMMENT] = "COMMENT",
        [TOKEN_LEFT_PAREN] = "LEFT_PAREN",
        [TOKEN_RIGHT_PAREN] = "RIGHT_PAREN",
        [TOKEN_COLON] = "COLON",
        [TOKEN_COMMA] = "COMMA",
        [TOKEN_SEMICOLON] = "SEMICOLON",
        [TOKEN_DOT] = "DOT",
        [TOKEN_LEFT_BRACE] = "LEFT_BRACE",
        [TOKEN_RIGHT_BRACE] = "RIGHT_BRACE",
        [TOKEN_LEFT_BRACKET] = "LEFT_BRACKET",
        [TOKEN_RIGHT_BRACKET] = "RIGHT_BRACKET",
        [TOKEN_PLUS] = "PLUS",
        [TOKEN_MINUS] = "MINUS",
        [TOKEN_STAR] = "STAR",
        [TOKEN_SLASH] = "SLASH",
        [TOKEN_PERCENT] = "PERCENT",
        [TOKEN_EQUAL] = "EQUAL",
        [TOKEN_EQUAL_EQUAL] = "EQUAL_EQUAL",
        [TOKEN_BANG] = "BANG",
        [TOKEN_BANG_EQUAL] = "BANG_EQUAL",
        [TOKEN_LESS] = "LESS",
        [TOKEN_LESS_EQUAL] = "LESS_EQUAL",
        [TOKEN_GREATER] = "GREATER",
        [TOKEN_GREATER_EQUAL] = "GREATER_EQUAL",
        [TOKEN_AMPERSAND] = "AMPERSAND",
        [TOKEN_AMPERSAND_AMPERSAND] = "AMPERSAND_AMPERSAND",
        [TOKEN_PIPE] = "PIPE",
        [TOKEN_PIPE_PIPE] = "PIPE_PIPE",
        [TOKEN_CARET] = "CARET",
        [TOKEN_TILDE] = "TILDE",
        [TOKEN_SHIFT_LEFT] = "SHIFT_LEFT",
        [TOKEN_SHIFT_RIGHT] = "SHIFT_RIGHT",
        [TOKEN_PLUS_EQUAL] = "PLUS_EQUAL",
        [TOKEN_MINUS_EQUAL] = "MINUS_EQUAL",
        [TOKEN_STAR_EQUAL] = "STAR_EQUAL",
        [TOKEN_SLASH_EQUAL] = "SLASH_EQUAL",
        [TOKEN_PERCENT_EQUAL] = "PERCENT_EQUAL",
        [TOKEN_AMPERSAND_EQUAL] = "AMPERSAND_EQUAL",
        [TOKEN_PIPE_EQUAL] = "PIPE_EQUAL",
        [TOKEN_CARET_EQUAL] = "CARET_EQUAL",
        [TOKEN_SHIFT_LEFT_EQUAL] = "SHIFT_LEFT_EQUAL",
        [TOKEN_SHIFT_RIGHT_EQUAL] = "SHIFT_RIGHT_EQUAL",
        [TOKEN_NEWLINE] = "NEWLINE",
        [TOKEN_EOF] = "EOF",
        [TOKEN_INVALID_NUMBER] = "invalid exponent in number",
        [TOKEN_UNTERMINATED_STRING] = "unterminated string",
        [TOKEN_UNTERMINATED_COMMENT] = "unterminated block comment",
    };

    Lexer lexer = lexer_init(source, length);
    for (;;) {
        Token token = lexer_next(&lexer);
        switch (token.kind) {
        case TOKEN_INVALID:
            fprintf(stderr, "%s:%zu:%zu: unexpected byte 0x%02X\n", path,
                    token.line, token.column, (unsigned int)source[token.offset]);
            return EXIT_FAILURE;
        case TOKEN_INVALID_NUMBER:
        case TOKEN_UNTERMINATED_STRING:
        case TOKEN_UNTERMINATED_COMMENT:
            fprintf(stderr, "%s:%zu:%zu: %s\n", path, token.line, token.column,
                    names[token.kind]);
            return EXIT_FAILURE;
        default:
            break;
        }

        printf("%zu:%zu %s", token.line, token.column, names[token.kind]);
        switch (token.kind) {
        case TOKEN_IDENTIFIER:
        case TOKEN_INTEGER:
        case TOKEN_FLOAT:
        case TOKEN_STRING:
        case TOKEN_COMMENT:
            putchar(' ');
            print_lexeme(source + token.offset, token.length);
            break;
        default:
            break;
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
