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
        if (error.has_instantiation) {
            modules_report(&program, error.instantiation_offset, "generic instantiated here");
        }
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

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "usage: %s [options] <source.lunso>\n", prog);
    fprintf(out, "       %s --build [options] <source.lunso> [-o <executable>] [--link <object>]...\n",
            prog);
}

static void print_help(const char *prog)
{
    print_usage(stdout, prog);
    fputs("\nModes:\n"
          "  (default)        Dump lexer tokens\n"
          "  --parse          Parse and report AST summary\n"
          "  --check          Run module loading and semantic checks\n"
          "  --emit-ir        Emit Lunso IR to stdout\n"
          "  --emit-qbe       Emit QBE IL to stdout\n"
          "  --build          Compile and link a native executable\n"
          "\nOptions:\n"
          "  --std=<dir>      Override standard library directory\n"
          "  --std <dir>      Override standard library directory\n"
          "  -o <file>        Output executable path (with --build; defaults to <stem>)\n"
          "  --link <obj>     Link an additional object or archive (with --build)\n"
          "  -h, --help       Show this help message and exit\n"
          "  -V, --version    Show compiler version and exit\n",
          stdout);
}

static char *default_output_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t len = strlen(base);
    static const char ext[] = ".lunso";
    size_t ext_len = sizeof(ext) - 1;
    if (len > ext_len && strcmp(base + len - ext_len, ext) == 0) {
        size_t stem_len = len - ext_len;
        char *out = malloc(stem_len + 1);
        if (out) {
            memcpy(out, base, stem_len);
            out[stem_len] = '\0';
        }
        return out;
    }
    static const char suffix[] = ".out";
    char *out = malloc(len + sizeof(suffix));
    if (out) {
        memcpy(out, base, len);
        memcpy(out + len, suffix, sizeof(suffix));
    }
    return out;
}

static bool set_std_dir(const char *path, const char **std_dir)
{
    if (!path || !*path) {
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

static bool try_parse_mode(const char *argument, Mode *out_mode)
{
    if (strcmp(argument, "--parse") == 0) {
        *out_mode = MODE_PARSE;
        return true;
    }
    if (strcmp(argument, "--check") == 0) {
        *out_mode = MODE_CHECK;
        return true;
    }
    if (strcmp(argument, "--emit-ir") == 0) {
        *out_mode = MODE_EMIT_IR;
        return true;
    }
    if (strcmp(argument, "--emit-qbe") == 0) {
        *out_mode = MODE_EMIT_QBE;
        return true;
    }
    if (strcmp(argument, "--build") == 0) {
        *out_mode = MODE_BUILD;
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    const char *std_dir = NULL;
    const char *path = NULL;
    Mode mode = MODE_TOKENS;
    bool mode_set = false;
    bool stop_options = false;
    NativeOptions native = {0};
    char *owned_output = NULL;

    for (int i = 1; i < argc;) {
        const char *arg = argv[i++];
        if (!stop_options && strcmp(arg, "--") == 0) {
            stop_options = true;
            continue;
        }
        if (!stop_options && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)) {
            print_help(argv[0]);
            free(native.objects);
            return fflush(stdout) == EOF || ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        if (!stop_options && (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0)) {
            puts("lunso-boot 0.1.0");
            free(native.objects);
            return fflush(stdout) == EOF || ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        Mode parsed_mode;
        if (!stop_options && try_parse_mode(arg, &parsed_mode)) {
            if (mode_set) {
                fprintf(stderr, "conflicting mode option: %s\n", arg);
                print_usage(stderr, argv[0]);
                free(native.objects);
                return EXIT_FAILURE;
            }
            mode = parsed_mode;
            mode_set = true;
            continue;
        }
        if (!stop_options && strncmp(arg, "--std=", sizeof("--std=") - 1) == 0) {
            if (!set_std_dir(arg + sizeof("--std=") - 1, &std_dir)) {
                free(native.objects);
                return EXIT_FAILURE;
            }
            continue;
        }
        if (!stop_options && strcmp(arg, "--std") == 0) {
            if (i >= argc || !set_std_dir(argv[i++], &std_dir)) {
                if (i > argc || !std_dir) {
                    print_usage(stderr, argv[0]);
                }
                free(native.objects);
                return EXIT_FAILURE;
            }
            continue;
        }
        if (!stop_options && strcmp(arg, "-o") == 0) {
            if (i >= argc || native.output) {
                fputs(native.output ? "-o can only be supplied once\n" : "-o requires an output path\n",
                      stderr);
                print_usage(stderr, argv[0]);
                free(native.objects);
                return EXIT_FAILURE;
            }
            native.output = argv[i++];
            continue;
        }
        if (!stop_options && strcmp(arg, "--link") == 0) {
            if (i >= argc) {
                fputs("--link requires an object path\n", stderr);
                print_usage(stderr, argv[0]);
                free(native.objects);
                return EXIT_FAILURE;
            }
            if (!native.objects) {
                native.objects = calloc((size_t)argc, sizeof(*native.objects));
                if (!native.objects) {
                    fputs("could not allocate link options\n", stderr);
                    return EXIT_FAILURE;
                }
            }
            native.objects[native.object_count++] = argv[i++];
            continue;
        }
        if (!stop_options && arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(stderr, argv[0]);
            free(native.objects);
            return EXIT_FAILURE;
        }
        if (path) {
            fprintf(stderr, "unexpected extra input file: %s\n", arg);
            print_usage(stderr, argv[0]);
            free(native.objects);
            return EXIT_FAILURE;
        }
        path = arg;
    }

    if (!path) {
        print_usage(stderr, argv[0]);
        free(native.objects);
        return EXIT_FAILURE;
    }
    if (mode != MODE_BUILD && (native.output != NULL || native.object_count > 0)) {
        fputs("-o and --link are only supported with --build\n", stderr);
        print_usage(stderr, argv[0]);
        free(native.objects);
        return EXIT_FAILURE;
    }
    if (mode == MODE_BUILD && !native.output) {
        owned_output = default_output_name(path);
        if (!owned_output) {
            fputs("could not allocate output path\n", stderr);
            free(native.objects);
            return EXIT_FAILURE;
        }
        native.output = owned_output;
    }

    int result = run_source(mode, path, &native, std_dir);
    free(owned_output);
    free(native.objects);
    return result;
}
