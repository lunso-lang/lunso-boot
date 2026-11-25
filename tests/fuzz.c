#include "ast.h"
#include "ir.h"
#include "lexer.h"
#include "parser.h"
#include "qbe.h"
#include "sema.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    unsigned char *data;
    size_t len;
} Seed;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static uint64_t rng_state = 1;

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

static size_t rng_range(size_t bound)
{
    return bound ? (size_t)(rng_next() % bound) : 0;
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    va_end(args);
    if (needed < 0) {
        return;
    }
    if (sb->len + (size_t)needed >= sb->cap) {
        size_t new_cap = sb->cap ? sb->cap * 2 : 512;
        while (new_cap <= sb->len + (size_t)needed) {
            new_cap *= 2;
        }
        char *grown = realloc(sb->data, new_cap);
        if (!grown) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        sb->data = grown;
        sb->cap = new_cap;
        va_start(args, fmt);
        vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
        va_end(args);
    }
    sb->len += (size_t)needed;
}

static const char *const TOKENS[] = {
    "fun ", "ext ", "struct ", "enum ", "as ", "const ", "import ",
    "pub ", "priv ", "return ", "if ", "else ", "for ", "break ", "continue ", "var ",
    "i32", "i64", "u8", "u64", "f64", "bool", "len(", "data(", "size_of(",
    "{", "}", "(", ")", "[", "]", ";", ":", ",", ".", "..",
    "=", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%",
    "&", "&&", "|", "||", "^", "~", "<<", ">>", "+=", "\n", "0", "1", "0x2a", "0b101", "'a'", "\"a\"",
};

static void fuzz_one(const unsigned char *data, size_t len, FILE *sink,
                     uint64_t seed, const char *label)
{
    Lexer lexer = lexer_init(data, len);
    for (;;) {
        Token token = lexer_next(&lexer);
        if (lexer_error_message(token.kind) || token.kind == TOKEN_EOF) {
            break;
        }
    }

    Ast ast;
    ParseError parse_err = {0};
    if (!parser_parse(data, len, &ast, &parse_err)) {
        if (!parse_err.message || !parse_err.message[0]) {
            fprintf(stderr, "fuzz invariant failed (%s, seed=%" PRIu64 "): "
                            "parser_parse returned false with empty error message\n",
                    label, seed);
            exit(EXIT_FAILURE);
        }
        return;
    }

    Sema sema;
    SemaError sema_err = {0};
    if (sema_check(&ast, &sema, &sema_err)) {
        IrModule ir;
        if (ir_lower(&ast, &sema, &ir)) {
            (void)ir_write(sink, &ir);
            (void)qbe_write(sink, &ir);
            ir_destroy(&ir);
        }
        sema_destroy(&sema);
    } else if (!sema_err.message || !sema_err.message[0]) {
        fprintf(stderr, "fuzz invariant failed (%s, seed=%" PRIu64 "): "
                        "sema_check returned false with empty error message\n",
                label, seed);
        ast_destroy(&ast);
        exit(EXIT_FAILURE);
    }
    ast_destroy(&ast);
}

static void emit_ws(StrBuf *sb)
{
    switch (rng_range(4)) {
    case 0:
        sb_append(sb, " ");
        break;
    case 1:
        sb_append(sb, "  ");
        break;
    case 2:
        sb_append(sb, "\n");
        break;
    case 3:
        sb_append(sb, " // comment\n");
        break;
    }
}

static void emit_type(StrBuf *sb)
{
    static const char *const types[] = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "*u8", "*const u8", "[]u8", "[4]i32",
    };
    sb_append(sb, "%s", types[rng_range(sizeof(types) / sizeof(types[0]))]);
}

static void emit_expr(StrBuf *sb, int depth)
{
    static const char *const binops[] = {
        "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=",
        "&", "|", "^", "<<", ">>", "&&", "||",
    };
    if (depth <= 0) {
        switch (rng_range(8)) {
        case 0:
            sb_append(sb, "%zu", rng_range(100));
            break;
        case 1:
            sb_append(sb, "%zu.%zu", rng_range(10), rng_range(10));
            break;
        case 2:
            sb_append(sb, "\"str\"");
            break;
        case 3:
            sb_append(sb, "x");
            break;
        case 4:
            sb_append(sb, "true");
            break;
        case 5:
            sb_append(sb, "0x%zx", rng_range(256));
            break;
        case 6:
            sb_append(sb, "0b101");
            break;
        case 7:
            sb_append(sb, "'a'");
            break;
        }
        return;
    }

    switch (rng_range(11)) {
    case 0:
    case 1:
        emit_expr(sb, 0);
        break;
    case 2:
        emit_expr(sb, depth - 1);
        sb_append(sb, " %s ", binops[rng_range(sizeof(binops) / sizeof(binops[0]))]);
        emit_expr(sb, depth - 1);
        break;
    case 3: {
        static const char *const unops[] = {"- ", "! ", "~ ", "& ", "* "};
        sb_append(sb, "%s", unops[rng_range(5)]);
        emit_expr(sb, depth - 1);
        break;
    }
    case 4:
        sb_append(sb, "(");
        emit_expr(sb, depth - 1);
        sb_append(sb, " as ");
        emit_type(sb);
        sb_append(sb, ")");
        break;
    case 5:
        sb_append(sb, "f(");
        emit_expr(sb, depth - 1);
        sb_append(sb, ", ");
        emit_expr(sb, depth - 1);
        sb_append(sb, ")");
        break;
    case 6:
        sb_append(sb, "arr[");
        emit_expr(sb, depth - 1);
        sb_append(sb, "]");
        break;
    case 7:
        sb_append(sb, "ptr[");
        emit_expr(sb, depth - 1);
        sb_append(sb, "..");
        emit_expr(sb, depth - 1);
        sb_append(sb, "]");
        break;
    case 8: {
        static const char *const builtins[] = {"len", "data", "size_of"};
        sb_append(sb, "%s(", builtins[rng_range(3)]);
        emit_expr(sb, depth - 1);
        sb_append(sb, ")");
        break;
    }
    case 9:
        sb_append(sb, "[");
        emit_expr(sb, depth - 1);
        sb_append(sb, ", ");
        emit_expr(sb, depth - 1);
        sb_append(sb, "]");
        break;
    case 10:
        sb_append(sb, "(S { f0: ");
        emit_expr(sb, depth - 1);
        sb_append(sb, " })");
        break;
    }
}

static void emit_stmt(StrBuf *sb, int depth)
{
    switch (rng_range(depth > 0 ? 9 : 3)) {
    case 0: {
        int form = (int)rng_range(3);
        sb_append(sb, "var v%zu", rng_range(5));
        if (form == 0 || form == 2) {
            sb_append(sb, ": ");
            emit_type(sb);
        }
        if (form == 1 || form == 2) {
            sb_append(sb, " = ");
            emit_expr(sb, 2);
        }
        sb_append(sb, "\n");
        break;
    }
    case 1:
        sb_append(sb, "x = ");
        emit_expr(sb, 2);
        sb_append(sb, "\n");
        break;
    case 2:
        sb_append(sb, "return ");
        emit_expr(sb, 2);
        sb_append(sb, "\n");
        break;
    case 3:
        sb_append(sb, "if ");
        emit_expr(sb, 2);
        sb_append(sb, " {\n");
        emit_stmt(sb, depth - 1);
        sb_append(sb, "}");
        if (rng_range(2) == 0) {
            sb_append(sb, " else {\n");
            emit_stmt(sb, depth - 1);
            sb_append(sb, "}");
        }
        sb_append(sb, "\n");
        break;
    case 4:
        sb_append(sb, "for var i: i32 = 0; i < 3; i += 1 {\n");
        emit_stmt(sb, depth - 1);
        sb_append(sb, "}\n");
        break;
    case 5:
        sb_append(sb, "for x < 10 {\n");
        emit_stmt(sb, depth - 1);
        sb_append(sb, "}\n");
        break;
    case 6:
        sb_append(sb, "break\n");
        break;
    case 7:
        sb_append(sb, "continue\n");
        break;
    case 8:
        sb_append(sb, "lbl%d: for x < 3 {\n", depth);
        if (rng_range(2) == 0) {
            sb_append(sb, "break lbl%d\n", depth);
        } else {
            sb_append(sb, "continue lbl%d\n", depth);
        }
        sb_append(sb, "}\n");
        break;
    }
}

static void gen_valid_program(StrBuf *sb)
{
    static const char *const seps[] = {", ", "; ", "\n"};
    sb->len = 0;

    if (rng_range(2) == 0) {
        static const char *const struct_seps[] = {"; ", "\n"};
        sb_append(sb, "struct S {");
        emit_ws(sb);
        size_t fields = 1 + rng_range(3);
        for (size_t i = 0; i < fields; i++) {
            sb_append(sb, "f%zu: ", i);
            emit_type(sb);
            sb_append(sb, "%s", struct_seps[rng_range(2)]);
        }
        sb_append(sb, "}\n");
    }

    if (rng_range(2) == 0) {
        sb_append(sb, "enum E : u8 {");
        emit_ws(sb);
        size_t members = 1 + rng_range(3);
        for (size_t i = 0; i < members; i++) {
            sb_append(sb, "M%zu", i);
            if (rng_range(2) == 0) {
                sb_append(sb, " = %zu", i * 3);
            }
            sb_append(sb, "%s", seps[rng_range(3)]);
        }
        sb_append(sb, "}\n");
    }

    if (rng_range(2) == 0) {
        sb_append(sb, "ext fun ext_f(a: i32): i32\n");
    }

    if (rng_range(2) == 0) {
        sb_append(sb, "const K");
        if (rng_range(2) == 0) {
            sb_append(sb, ": i32");
        }
        sb_append(sb, " = %zu\n", rng_range(100));
    }

    sb_append(sb, "fun main(");
    if (rng_range(2) == 0) {
        sb_append(sb, "a: ");
        emit_type(sb);
        if (rng_range(2) == 0) {
            sb_append(sb, ", b: ");
            emit_type(sb);
        }
    }
    sb_append(sb, ")");
    if (rng_range(2) == 0) {
        sb_append(sb, ": ");
        emit_type(sb);
    }
    sb_append(sb, " {\n");
    size_t stmts = 1 + rng_range(4);
    for (size_t i = 0; i < stmts; i++) {
        emit_stmt(sb, 2);
    }
    sb_append(sb, "}\n");
}

static void gen_invalid_program(StrBuf *sb)
{
    sb->len = 0;
    switch (rng_range(11)) {
    case 0:
        sb_append(sb, "fun main( { }\n");
        break;
    case 1:
        sb_append(sb, "fun main(): i32 {\n    return (1 + 2\n}\n");
        break;
    case 2:
        sb_append(sb, "fun main(): i32 {\n    return 1 +\n}\n");
        break;
    case 3:
        sb_append(sb, "fun main(): i32 {\n    return 1 * / 2\n}\n");
        break;
    case 4:
        sb_append(sb, "fun main(): i32 {\n    var = 1\n}\n");
        break;
    case 5:
        sb_append(sb, "struct { a: i32 }\n");
        break;
    case 6:
        sb_append(sb, "enum E : u8 { A\n");
        break;
    case 7:
        sb_append(sb, "fun main(): i32 {\n    return arr[\n}\n");
        break;
    case 8:
        sb_append(sb, "return 0\n");
        break;
    case 9:
        sb_append(sb, "fun main(): i32 {\n    return x.\n}\n");
        break;
    case 10:
        sb_append(sb, "fun main(): i32 {\n    var a = []\n    return 0\n}\n");
        break;
    }
}

static Seed *load_seeds(int argc, char **argv, int first, size_t *out_count)
{
    int max_files = argc - first;
    if (max_files <= 0) {
        *out_count = 0;
        return NULL;
    }

    Seed *seeds = calloc((size_t)max_files, sizeof(*seeds));
    if (!seeds) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    size_t count = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        long size = ftell(f);
        if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            continue;
        }
        unsigned char *buf = malloc((size_t)size + 1);
        if (!buf) {
            fclose(f);
            exit(EXIT_FAILURE);
        }
        size_t n = fread(buf, 1, (size_t)size, f);
        fclose(f);
        buf[n] = '\0';
        seeds[count++] = (Seed){.data = buf, .len = n};
    }

    *out_count = count;
    return seeds;
}

static size_t mutate_buffer(unsigned char *buf, size_t len, size_t max_len,
                            const Seed *seeds, size_t seed_count)
{
    if (seed_count > 0 && rng_range(4) != 0) {
        const Seed *s = &seeds[rng_range(seed_count)];
        len = s->len < max_len ? s->len : max_len;
        memcpy(buf, s->data, len);
    }

    size_t ops = 1 + rng_range(4);
    for (size_t i = 0; i < ops; i++) {
        switch (rng_range(5)) {
        case 0:
            if (len > 0) {
                buf[rng_range(len)] ^= (unsigned char)(1u << rng_range(8));
            }
            break;
        case 1:
            if (len > 0) {
                buf[rng_range(len)] = (unsigned char)rng_next();
            }
            break;
        case 2:
            if (len < max_len) {
                size_t pos = rng_range(len + 1);
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = (unsigned char)rng_next();
                len++;
            }
            break;
        case 3:
            if (len > 1) {
                size_t pos = rng_range(len);
                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                len--;
            }
            break;
        case 4: {
            const char *tok = TOKENS[rng_range(sizeof(TOKENS) / sizeof(TOKENS[0]))];
            size_t tlen = strlen(tok);
            if (len + tlen <= max_len) {
                size_t pos = rng_range(len + 1);
                memmove(buf + pos + tlen, buf + pos, len - pos);
                memcpy(buf + pos, tok, tlen);
                len += tlen;
            }
            break;
        }
        }
    }
    return len;
}

int main(int argc, char **argv)
{
    uint64_t iters = 20000;
    uint64_t seed = (uint64_t)time(NULL) ^ (uint64_t)clock();
    size_t max_len = 1024;

    int first_seed = 1;
    while (first_seed < argc && strncmp(argv[first_seed], "--", 2) == 0) {
        const char *arg = argv[first_seed++];
        if (strncmp(arg, "--iters=", 8) == 0) {
            iters = strtoull(arg + 8, NULL, 10);
        } else if (strncmp(arg, "--seed=", 7) == 0) {
            seed = strtoull(arg + 7, NULL, 10);
        } else if (strncmp(arg, "--max-len=", 10) == 0) {
            max_len = (size_t)strtoull(arg + 10, NULL, 10);
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return EXIT_FAILURE;
        }
    }

    rng_state = seed ? seed : 1;

    FILE *sink = fopen("/dev/null", "wb");
    if (!sink) {
        perror("/dev/null");
        return EXIT_FAILURE;
    }

    size_t seed_count = 0;
    Seed *seeds = load_seeds(argc, argv, first_seed, &seed_count);

    unsigned char *buf = malloc(max_len + 1);
    if (!buf) {
        perror("malloc");
        fclose(sink);
        return EXIT_FAILURE;
    }

    StrBuf gen = {0};
    gen.cap = 1024;
    gen.data = malloc(gen.cap);
    if (!gen.data) {
        perror("malloc");
        free(buf);
        fclose(sink);
        return EXIT_FAILURE;
    }

    printf("fuzz: seed=%" PRIu64 " iters=%" PRIu64 " seeds=%zu max_len=%zu\n",
           seed, iters, seed_count, max_len);
    fflush(stdout);

    for (size_t i = 0; i < seed_count; i++) {
        fuzz_one(seeds[i].data, seeds[i].len, sink, seed, "seed");
    }

    // 1. Mutation fuzzing: crashes, ASan/UBSan, and non-empty error messages.
    size_t len = 0;
    for (uint64_t i = 0; i < iters; i++) {
        len = mutate_buffer(buf, len, max_len, seeds, seed_count);
        fuzz_one(buf, len, sink, seed, "mutate");
    }

    int status = EXIT_SUCCESS;

    // 2. Grammar-based valid programs: must always parse.
    uint64_t grammar_iters = iters / 4;
    for (uint64_t i = 0; i < grammar_iters; i++) {
        gen_valid_program(&gen);
        Ast ast;
        ParseError err = {0};
        if (!parser_parse((const unsigned char *)gen.data, gen.len, &ast, &err)) {
            fprintf(stderr,
                    "fuzz valid-grammar failure (seed=%" PRIu64 " iter=%" PRIu64 "): "
                    "%zu:%zu: %s\n--- source ---\n%s\n",
                    seed, i, err.token.line, err.token.column,
                    err.message ? err.message : "(null)", gen.data);
            status = EXIT_FAILURE;
            goto cleanup;
        }
        ast_destroy(&ast);
        fuzz_one((const unsigned char *)gen.data, gen.len, sink, seed, "valid-grammar");
    }

    // 3. Invalid-syntax generator: must never parse.
    for (uint64_t i = 0; i < grammar_iters; i++) {
        gen_invalid_program(&gen);
        Ast ast;
        ParseError err = {0};
        if (parser_parse((const unsigned char *)gen.data, gen.len, &ast, &err)) {
            fprintf(stderr,
                    "fuzz invalid-grammar failure (seed=%" PRIu64 " iter=%" PRIu64 "): "
                    "unexpectedly parsed!\n--- source ---\n%s\n",
                    seed, i, gen.data);
            ast_destroy(&ast);
            status = EXIT_FAILURE;
            goto cleanup;
        }
        if (!err.message || !err.message[0]) {
            fprintf(stderr, "fuzz invalid-grammar failure: empty error message\n");
            status = EXIT_FAILURE;
            goto cleanup;
        }
    }

cleanup:
    for (size_t i = 0; i < seed_count; i++) {
        free(seeds[i].data);
    }
    free(seeds);
    free(gen.data);
    free(buf);
    fclose(sink);
    if (status == EXIT_SUCCESS) {
        printf("fuzz: completed %" PRIu64 " mutation + %" PRIu64 " valid + %" PRIu64 " invalid iterations\n",
               iters, grammar_iters, grammar_iters);
    }
    return status;
}
