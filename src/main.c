#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "ir.h"
#include "qbe.h"
#include "native.h"
#include "modules.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_TOKENS, MODE_PARSE, MODE_CHECK, MODE_EMIT_IR, MODE_EMIT_QBE, MODE_BUILD } Mode;

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
        [TOKEN_STRUCT] = "STRUCT",
        [TOKEN_ENUM] = "ENUM",
        [TOKEN_AS] = "AS",
        [TOKEN_CONST] = "CONST",
        [TOKEN_IMPORT] = "IMPORT",
        [TOKEN_PUB] = "PUB",
        [TOKEN_PRIV] = "PRIV",
        [TOKEN_RETURN] = "RETURN",
        [TOKEN_IF] = "IF",
        [TOKEN_ELSE] = "ELSE",
        [TOKEN_MATCH] = "MATCH",
        [TOKEN_FOR] = "FOR",
        [TOKEN_BREAK] = "BREAK",
        [TOKEN_CONTINUE] = "CONTINUE",
        [TOKEN_VAR] = "VAR",
        [TOKEN_IDENTIFIER] = "IDENTIFIER",
        [TOKEN_INTEGER] = "INTEGER",
        [TOKEN_FLOAT] = "FLOAT",
        [TOKEN_STRING] = "STRING",
        [TOKEN_CHAR] = "CHAR",
        [TOKEN_COMMENT] = "COMMENT",
        [TOKEN_LEFT_PAREN] = "LEFT_PAREN",
        [TOKEN_RIGHT_PAREN] = "RIGHT_PAREN",
        [TOKEN_COLON] = "COLON",
        [TOKEN_COMMA] = "COMMA",
        [TOKEN_SEMICOLON] = "SEMICOLON",
        [TOKEN_DOT] = "DOT",
        [TOKEN_DOT_DOT] = "DOT_DOT",
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
        [TOKEN_FAT_ARROW] = "FAT_ARROW",
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
        case TOKEN_CHAR:
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

static int parse_source(const char *path, const unsigned char *source, size_t length)
{
    Ast ast;
    ParseError error;
    if (!parser_parse(source, length, &ast, &error)) {
        return report_source_error(path, source, error.token, error.message);
    }
    uint32_t nodes = ast.node_count ? ast.node_count - 1 : 0;
    printf("Parsed %" PRIu32 " function(s), %" PRIu32 " AST node(s)\n",
           ast.function_count, nodes);
    ast_destroy(&ast);
    return fflush(stdout) == EOF || ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int process_source(Mode mode, const char *path, const unsigned char *source, size_t length,
                          const NativeOptions *native, const char *std_dir)
{
    if (mode == MODE_PARSE) {
        return parse_source(path, source, length);
    }

    ModuleProgram program;
    if (!modules_load(path, source, length, std_dir, &program)) {
        return EXIT_FAILURE;
    }
    Sema sema;
    SemaError error;
    if (!sema_check(&program.ast, &sema, &error)) {
        modules_report(&program, error.offset, error.message);
        modules_destroy(&program);
        return EXIT_FAILURE;
    }

    bool ok = true;
    if (mode == MODE_CHECK) {
        printf("Checked %" PRIu32 " function(s)\n", program.ast.function_count);
    } else if (mode == MODE_BUILD && modules_output_overlaps(&program, native->output)) {
        fprintf(stderr, "%s: output would overwrite the source file\n", native->output);
        ok = false;
    } else {
        IrModule ir;
        if (!ir_lower(&program.ast, &sema, &ir)) {
            fprintf(stderr, "%s: could not allocate IR storage\n", path);
            ok = false;
        } else {
            if (mode == MODE_BUILD) {
                ok = native_build(&ir, path, native);
            } else {
                ok = mode == MODE_EMIT_IR ? ir_write(stdout, &ir) : qbe_write(stdout, &ir);
            }
            ir_destroy(&ir);
        }
    }
    sema_destroy(&sema);
    modules_destroy(&program);
    return ok && fflush(stdout) != EOF && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool is_std_option(const char *argument)
{
    return strncmp(argument, "--std=", sizeof("--std=") - 1) == 0;
}

static bool set_std_option(const char *argument, const char **std_dir)
{
    const char *path = argument + sizeof("--std=") - 1;
    if (!*path) {
        fputs("--std requires a directory path\n", stderr);
        return false;
    }
    if (*std_dir) {
        fputs("--std can only be supplied once\n", stderr);
        return false;
    }
    *std_dir = path;
    return true;
}

static bool parse_source_options(Mode mode, int argc, char **argv, int first,
                                 NativeOptions *native, const char **std_dir)
{
    size_t link_capacity = (size_t)(argc - first) / 2;
    for (int i = first; i < argc;) {
        const char *option = argv[i++];
        if (is_std_option(option)) {
            if (!set_std_option(option, std_dir)) {
                return false;
            }
            continue;
        }
        if (mode != MODE_BUILD || i == argc) {
            return false;
        }
        const char *value = argv[i++];
        if (strcmp(option, "-o") == 0 && !native->output) {
            native->output = value;
            continue;
        }
        if (strcmp(option, "--link") != 0) {
            return false;
        }
        if (!native->objects) {
            native->objects = calloc(link_capacity, sizeof(*native->objects));
            if (!native->objects) {
                fputs("could not allocate link options\n", stderr);
                return false;
            }
        }
        native->objects[native->object_count++] = value;
    }
    return mode != MODE_BUILD || native->output != NULL;
}

static int run_source(Mode mode, const char *path, const NativeOptions *native, const char *std_dir)
{
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

    int result = mode == MODE_TOKENS ? dump_tokens(path, source, length)
                                     : process_source(mode, path, source, length, native, std_dir);
    free(source);
    return result;
}

static Mode parse_mode(const char *argument)
{
    if (strcmp(argument, "--parse") == 0) {
        return MODE_PARSE;
    }
    if (strcmp(argument, "--check") == 0) {
        return MODE_CHECK;
    }
    if (strcmp(argument, "--emit-ir") == 0) {
        return MODE_EMIT_IR;
    }
    if (strcmp(argument, "--emit-qbe") == 0) {
        return MODE_EMIT_QBE;
    }
    if (strcmp(argument, "--build") == 0) {
        return MODE_BUILD;
    }
    return MODE_TOKENS;
}

int main(int argc, char **argv)
{
    const char *std_dir = NULL;
    int next = 1;
    bool valid = true;
    while (valid && next < argc && is_std_option(argv[next])) {
        valid = set_std_option(argv[next++], &std_dir);
    }
    Mode mode = next < argc ? parse_mode(argv[next]) : MODE_TOKENS;
    if (mode != MODE_TOKENS) {
        next++;
        while (valid && next < argc && is_std_option(argv[next])) {
            valid = set_std_option(argv[next++], &std_dir);
        }
    }
    NativeOptions native = {0};
    const char *path = next < argc ? argv[next++] : NULL;
    valid = valid && path && parse_source_options(mode, argc, argv, next, &native, &std_dir);
    if (!valid) {
        fprintf(stderr, "usage: %s [--std=<directory>] [--parse | --check | --emit-ir | --emit-qbe] <source.lunso>\n",
                argv[0]);
        fprintf(stderr, "       %s [--std=<directory>] --build <source.lunso> -o <executable> [--link <object>]...\n",
                argv[0]);
        free(native.objects);
        return EXIT_FAILURE;
    }

    int result = run_source(mode, path, &native, std_dir);
    free(native.objects);
    return result;
}
