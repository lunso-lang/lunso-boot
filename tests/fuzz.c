#include "ast.h"
#include "ir.h"
#include "lexer.h"
#include "parser.h"
#include "qbe.h"
#include "sema.h"

#include <inttypes.h>
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

static const char *const TOKENS[] = {
    "fun ", "ext ", "struct ", "enum ", "as ", "const ", "import ",
    "pub ", "priv ", "return ", "if ", "else ", "for ", "var ",
    "i32", "i64", "u8", "u64", "f64", "bool", "len(", "data(", "size_of(",
    "{", "}", "(", ")", "[", "]", ";", ":", ",", ".", "..",
    "=", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%",
    "&", "&&", "|", "||", "^", "~", "<<", ">>", "+=", "\n", "0", "1", "\"a\"",
};

static void fuzz_one(const unsigned char *data, size_t len, FILE *sink)
{
    Lexer lexer = lexer_init(data, len);
    for (;;) {
        Token token = lexer_next(&lexer);
        if (lexer_error_message(token.kind) || token.kind == TOKEN_EOF) {
            break;
        }
    }

    Ast ast;
    ParseError parse_err;
    if (!parser_parse(data, len, &ast, &parse_err)) {
        return;
    }

    Sema sema;
    SemaError sema_err;
    if (sema_check(&ast, &sema, &sema_err)) {
        IrModule ir;
        if (ir_lower(&ast, &sema, &ir)) {
            (void)ir_write(sink, &ir);
            (void)qbe_write(sink, &ir);
            ir_destroy(&ir);
        }
        sema_destroy(&sema);
    }
    ast_destroy(&ast);
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

    printf("fuzz: seed=%" PRIu64 " iters=%" PRIu64 " seeds=%zu max_len=%zu\n",
           seed, iters, seed_count, max_len);
    fflush(stdout);

    for (size_t i = 0; i < seed_count; i++) {
        fuzz_one(seeds[i].data, seeds[i].len, sink);
    }

    size_t len = 0;
    for (uint64_t i = 0; i < iters; i++) {
        len = mutate_buffer(buf, len, max_len, seeds, seed_count);
        fuzz_one(buf, len, sink);
    }

    for (size_t i = 0; i < seed_count; i++) {
        free(seeds[i].data);
    }
    free(seeds);
    free(buf);
    fclose(sink);
    printf("fuzz: completed %" PRIu64 " iterations\n", iters);
    return EXIT_SUCCESS;
}
