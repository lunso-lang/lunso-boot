#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "ir.h"
#include "qbe.h"
#include "native.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_TOKENS, MODE_PARSE, MODE_CHECK, MODE_EMIT_IR, MODE_EMIT_QBE, MODE_BUILD } Mode;

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

static int report_source_error(const char *path, const unsigned char *source,
                               Token token, const char *message)
{
    fprintf(stderr, "%s:%zu:%zu: %s", path, token.line, token.column, message);
    if (token.kind == TOKEN_INVALID) {
        fprintf(stderr, " 0x%02X", (unsigned int)source[token.offset]);
    }
    fputc('\n', stderr);
    return EXIT_FAILURE;
}

static int dump_tokens(const char *path, const unsigned char *source, size_t length)
{
    static const char *const names[] = {
        [TOKEN_FUN] = "FUN",
        [TOKEN_EXT] = "EXT",
        [TOKEN_RETURN] = "RETURN",
        [TOKEN_IF] = "IF",
        [TOKEN_ELSE] = "ELSE",
        [TOKEN_FOR] = "FOR",
        [TOKEN_VAR] = "VAR",
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
    };

    Lexer lexer = lexer_init(source, length);
    for (;;) {
        Token token = lexer_next(&lexer);
        const char *error = lexer_error_message(token.kind);
        if (error) {
            return report_source_error(path, source, token, error);
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

static int report_offset_error(const char *path, const unsigned char *source, SemaError error)
{
    Token token = {.line = 1, .column = 1};
    for (uint32_t i = 0; i < error.offset; i++) {
        if (source[i] == '\r' || source[i] == '\n') {
            if (source[i] == '\r' && i + 1 < error.offset && source[i + 1] == '\n') {
                i++;
            }
            token.line++;
            token.column = 1;
        } else {
            token.column++;
        }
    }
    return report_source_error(path, source, token, error.message);
}

static int process_source(Mode mode, const char *path, const unsigned char *source, size_t length,
                          const char *output)
{
    Ast ast;
    ParseError error;
    if (!parser_parse(source, length, &ast, &error)) {
        return report_source_error(path, source, error.token, error.message);
    }

    int result = EXIT_SUCCESS;
    if (mode != MODE_PARSE) {
        Sema sema;
        SemaError semantic_error;
        if (!sema_check(&ast, &sema, &semantic_error)) {
            ast_destroy(&ast);
            return report_offset_error(path, source, semantic_error);
        }
        if (mode != MODE_CHECK) {
            IrModule ir;
            if (!ir_lower(&ast, &sema, &ir)) {
                fprintf(stderr, "%s: could not allocate IR storage\n", path);
                result = EXIT_FAILURE;
            } else {
                bool written;
                if (mode == MODE_BUILD) {
                    written = native_build(&ir, path, output);
                } else {
                    written = mode == MODE_EMIT_IR ? ir_write(stdout, &ir) : qbe_write(stdout, &ir);
                }
                if (!written) {
                    result = EXIT_FAILURE;
                }
                ir_destroy(&ir);
            }
        } else {
            printf("Checked %" PRIu32 " function(s)\n", ast.function_count);
        }
        sema_destroy(&sema);
    } else {
        uint32_t nodes = ast.node_count ? ast.node_count - 1 : 0;
        printf("Parsed %" PRIu32 " function(s), %" PRIu32 " AST node(s)\n",
               ast.function_count, nodes);
    }
    ast_destroy(&ast);
    return fflush(stdout) == EOF || ferror(stdout) ? EXIT_FAILURE : result;
}

int main(int argc, char **argv)
{
    Mode mode = MODE_TOKENS;
    if (argc > 1 && strcmp(argv[1], "--parse") == 0) {
        mode = MODE_PARSE;
    } else if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        mode = MODE_CHECK;
    } else if (argc > 1 && strcmp(argv[1], "--emit-ir") == 0) {
        mode = MODE_EMIT_IR;
    } else if (argc > 1 && strcmp(argv[1], "--emit-qbe") == 0) {
        mode = MODE_EMIT_QBE;
    } else if (argc > 1 && strcmp(argv[1], "--build") == 0) {
        mode = MODE_BUILD;
    }
    int expected = mode == MODE_TOKENS ? 2 : (mode == MODE_BUILD ? 5 : 3);
    if (argc != expected || (mode == MODE_BUILD && strcmp(argv[3], "-o") != 0)) {
        fprintf(stderr, "usage: %s [--parse | --check | --emit-ir | --emit-qbe] <source.lunso>\n",
                argv[0]);
        fprintf(stderr, "       %s --build <source.lunso> -o <executable>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[mode == MODE_TOKENS ? 1 : 2];
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

    const char *output = mode == MODE_BUILD ? argv[4] : NULL;
    int result = mode == MODE_TOKENS ? dump_tokens(path, source, length)
                                     : process_source(mode, path, source, length, output);
    free(source);
    return result;
}
