#include "parser.h"

#include "ast_builder.h"

#include <stdlib.h>

typedef struct {
    Lexer lexer;
    Token current;
    ParseError error;
    AstBuilder builder;
    unsigned group_depth;
    unsigned recursion_depth;
} Parser;

enum {
    PREC_ASSIGNMENT = 1,
    PREC_LOGICAL_OR,
    PREC_LOGICAL_AND,
    PREC_BIT_OR,
    PREC_BIT_XOR,
    PREC_BIT_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_SHIFT,
    PREC_SUM,
    PREC_PRODUCT,
    PREC_PREFIX,
    MAX_RECURSION_DEPTH = 256,
};

typedef struct {
    AstKind kind;
    unsigned precedence;
} Infix;

static const Infix infix_operators[] = {
    [TOKEN_EQUAL] = {AST_ASSIGN, PREC_ASSIGNMENT},
    [TOKEN_PLUS_EQUAL] = {AST_ASSIGN_ADD, PREC_ASSIGNMENT},
    [TOKEN_MINUS_EQUAL] = {AST_ASSIGN_SUBTRACT, PREC_ASSIGNMENT},
    [TOKEN_STAR_EQUAL] = {AST_ASSIGN_MULTIPLY, PREC_ASSIGNMENT},
    [TOKEN_SLASH_EQUAL] = {AST_ASSIGN_DIVIDE, PREC_ASSIGNMENT},
    [TOKEN_PERCENT_EQUAL] = {AST_ASSIGN_REMAINDER, PREC_ASSIGNMENT},
    [TOKEN_AMPERSAND_EQUAL] = {AST_ASSIGN_BIT_AND, PREC_ASSIGNMENT},
    [TOKEN_PIPE_EQUAL] = {AST_ASSIGN_BIT_OR, PREC_ASSIGNMENT},
    [TOKEN_CARET_EQUAL] = {AST_ASSIGN_BIT_XOR, PREC_ASSIGNMENT},
    [TOKEN_SHIFT_LEFT_EQUAL] = {AST_ASSIGN_SHIFT_LEFT, PREC_ASSIGNMENT},
    [TOKEN_SHIFT_RIGHT_EQUAL] = {AST_ASSIGN_SHIFT_RIGHT, PREC_ASSIGNMENT},
    [TOKEN_PIPE_PIPE] = {AST_LOGICAL_OR, PREC_LOGICAL_OR},
    [TOKEN_AMPERSAND_AMPERSAND] = {AST_LOGICAL_AND, PREC_LOGICAL_AND},
    [TOKEN_PIPE] = {AST_BIT_OR, PREC_BIT_OR},
    [TOKEN_CARET] = {AST_BIT_XOR, PREC_BIT_XOR},
    [TOKEN_AMPERSAND] = {AST_BIT_AND, PREC_BIT_AND},
    [TOKEN_EQUAL_EQUAL] = {AST_EQUAL, PREC_EQUALITY},
    [TOKEN_BANG_EQUAL] = {AST_NOT_EQUAL, PREC_EQUALITY},
    [TOKEN_LESS] = {AST_LESS, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {AST_LESS_EQUAL, PREC_COMPARISON},
    [TOKEN_GREATER] = {AST_GREATER, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {AST_GREATER_EQUAL, PREC_COMPARISON},
    [TOKEN_SHIFT_LEFT] = {AST_SHIFT_LEFT, PREC_SHIFT},
    [TOKEN_SHIFT_RIGHT] = {AST_SHIFT_RIGHT, PREC_SHIFT},
    [TOKEN_PLUS] = {AST_ADD, PREC_SUM},
    [TOKEN_MINUS] = {AST_SUBTRACT, PREC_SUM},
    [TOKEN_STAR] = {AST_MULTIPLY, PREC_PRODUCT},
    [TOKEN_SLASH] = {AST_DIVIDE, PREC_PRODUCT},
    [TOKEN_PERCENT] = {AST_REMAINDER, PREC_PRODUCT},
};

static AstNodeId parse_expression(Parser *parser, unsigned minimum);
static AstNodeId parse_block(Parser *parser);

static bool failed(const Parser *parser)
{
    return parser->error.message != NULL;
}

static void fail(Parser *parser, const char *message)
{
    if (!failed(parser)) {
        const char *lex_error = lexer_error_message(parser->current.kind);
        parser->error = (ParseError){parser->current, lex_error ? lex_error : message};
    }
}

static Token read_token(Lexer *lexer, bool in_group)
{
    for (;;) {
        Token token = lexer_next(lexer);
        if (token.kind == TOKEN_COMMENT) {
            if (!in_group && lexer->line != token.line) {
                // Preserve a statement boundary when a block comment crosses a line.
                while (lexer->source[token.offset] != '\r' && lexer->source[token.offset] != '\n') {
                    token.offset++;
                    token.column++;
                }
                token.kind = TOKEN_NEWLINE;
                token.length = lexer->source[token.offset] == '\r' &&
                               lexer->source[token.offset + 1] == '\n' ? 2 : 1;
                return token;
            }
            continue;
        }
        if (token.kind != TOKEN_NEWLINE || !in_group) {
            return token;
        }
    }
}

static void advance(Parser *parser)
{
    parser->current = read_token(&parser->lexer, parser->group_depth != 0);
}

static bool match(Parser *parser, TokenKind kind)
{
    if (failed(parser) || parser->current.kind != kind) {
        return false;
    }
    advance(parser);
    return true;
}

static Token expect(Parser *parser, TokenKind kind, const char *message)
{
    Token token = parser->current;
    if (!match(parser, kind)) {
        fail(parser, message);
    }
    return token;
}

static bool enter(Parser *parser)
{
    if (failed(parser)) {
        return false;
    }
    if (parser->recursion_depth == MAX_RECURSION_DEPTH) {
        fail(parser, "syntax nesting limit exceeded");
        return false;
    }
    parser->recursion_depth++;
    return true;
}

static AstNodeId add_node(Parser *parser, AstKind kind, Token token, uint32_t a, uint32_t b)
{
    if (failed(parser)) {
        return AST_NODE_NONE;
    }
    AstNodeId id = ast_add_node(&parser->builder, kind, (uint32_t)token.offset, (AstNodeData){a, b});
    if (id == AST_NODE_NONE) {
        fail(parser, "could not allocate AST storage");
    }
    return id;
}

static void push_child(Parser *parser, AstNodeId child)
{
    if (!failed(parser) && !ast_push_child(&parser->builder, child)) {
        fail(parser, "could not allocate AST storage");
    }
}

static AstNodeRange finish_children(Parser *parser, uint32_t mark)
{
    AstNodeRange range = {0};
    if (!failed(parser) && !ast_finish_children(&parser->builder, mark, &range)) {
        fail(parser, "could not allocate AST storage");
    }
    return range;
}

static bool is_separator(TokenKind kind)
{
    return kind == TOKEN_NEWLINE || kind == TOKEN_SEMICOLON;
}

static void finish_item(Parser *parser, TokenKind end, const char *message)
{
    if (!failed(parser) && parser->current.kind != end &&
        !is_separator(parser->current.kind)) {
        fail(parser, message);
    }
    while (!failed(parser) && is_separator(parser->current.kind)) {
        advance(parser);
    }
}

static void open_group(Parser *parser)
{
    parser->group_depth++;
    advance(parser);
}

static void close_group(Parser *parser, TokenKind kind, const char *message)
{
    parser->group_depth--;
    expect(parser, kind, message);
}

static AstNodeId parse_call(Parser *parser, AstNodeId callee)
{
    Token opening = parser->current;
    uint32_t mark = parser->builder.pending_count;
    push_child(parser, callee);
    open_group(parser);
    if (parser->current.kind != TOKEN_RIGHT_PAREN) {
        do {
            push_child(parser, parse_expression(parser, PREC_ASSIGNMENT));
        } while (match(parser, TOKEN_COMMA) && parser->current.kind != TOKEN_RIGHT_PAREN);
    }
    close_group(parser, TOKEN_RIGHT_PAREN, "expected ')' after arguments");
    AstNodeRange items = finish_children(parser, mark);
    return add_node(parser, AST_CALL, opening, items.first, items.count);
}

static AstNodeId parse_postfix(Parser *parser, AstNodeId base)
{
    while (!failed(parser)) {
        Token token = parser->current;
        switch (token.kind) {
        case TOKEN_LEFT_PAREN:
            base = parse_call(parser, base);
            break;
        case TOKEN_LEFT_BRACKET: {
            open_group(parser);
            AstNodeId index = parse_expression(parser, PREC_ASSIGNMENT);
            close_group(parser, TOKEN_RIGHT_BRACKET, "expected ']' after index");
            base = add_node(parser, AST_INDEX, token, base, index);
            break;
        }
        case TOKEN_DOT: {
            advance(parser);
            Token member = expect(parser, TOKEN_IDENTIFIER, "expected member name after '.'");
            base = add_node(parser, AST_MEMBER, member, base, (uint32_t)member.length);
            break;
        }
        default:
            return base;
        }
    }
    return AST_NODE_NONE;
}

static AstNodeId parse_operand(Parser *parser)
{
    Token token = parser->current;
    AstKind kind;
    switch (token.kind) {
    case TOKEN_PLUS:
        kind = AST_POSITIVE;
        break;
    case TOKEN_MINUS:
        kind = AST_NEGATE;
        break;
    case TOKEN_BANG:
        kind = AST_LOGICAL_NOT;
        break;
    case TOKEN_TILDE:
        kind = AST_BIT_NOT;
        break;
    case TOKEN_LEFT_PAREN: {
        open_group(parser);
        AstNodeId value = parse_expression(parser, PREC_ASSIGNMENT);
        close_group(parser, TOKEN_RIGHT_PAREN, "expected ')' after expression");
        return parse_postfix(parser, value);
    }
    case TOKEN_IDENTIFIER:
    case TOKEN_INTEGER:
    case TOKEN_FLOAT:
    case TOKEN_STRING: {
        static const AstKind literal_kinds[] = {
            [TOKEN_IDENTIFIER] = AST_IDENTIFIER,
            [TOKEN_INTEGER] = AST_INTEGER,
            [TOKEN_FLOAT] = AST_FLOAT,
            [TOKEN_STRING] = AST_STRING,
        };
        advance(parser);
        AstNodeId node = add_node(parser, literal_kinds[token.kind], token,
                                  (uint32_t)token.length, 0);
        return parse_postfix(parser, node);
    }
    default:
        fail(parser, "expected an expression");
        return AST_NODE_NONE;
    }

    advance(parser);
    AstNodeId operand = parse_expression(parser, PREC_PREFIX);
    return add_node(parser, kind, token, operand, 0);
}

static AstNodeId parse_expression(Parser *parser, unsigned minimum)
{
    if (!enter(parser)) {
        return AST_NODE_NONE;
    }

    AstNodeId left = parse_operand(parser);
    while (!failed(parser) &&
           (size_t)parser->current.kind < sizeof(infix_operators) / sizeof(infix_operators[0])) {
        Token token = parser->current;
        Infix infix = infix_operators[token.kind];
        if (infix.precedence < minimum) {
            break;
        }
        advance(parser);
        // Assignment is right-associative. Other infix operators are left-associative.
        unsigned next = infix.precedence + (infix.precedence != PREC_ASSIGNMENT);
        AstNodeId right = parse_expression(parser, next);
        left = add_node(parser, infix.kind, token, left, right);
    }

    parser->recursion_depth--;
    return left;
}

static bool match_else(Parser *parser)
{
    if (match(parser, TOKEN_ELSE)) {
        return true;
    }
    if (failed(parser) || parser->current.kind != TOKEN_NEWLINE) {
        return false;
    }

    Lexer lookahead = parser->lexer;
    Token token;
    do {
        token = read_token(&lookahead, false);
    } while (token.kind == TOKEN_NEWLINE);
    if (token.kind != TOKEN_ELSE) {
        return false;
    }
    parser->lexer = lookahead;
    advance(parser);
    return true;
}

static AstNodeId parse_if(Parser *parser)
{
    if (!enter(parser)) {
        return AST_NODE_NONE;
    }
    Token keyword = expect(parser, TOKEN_IF, "expected 'if'");
    AstNodeId condition = parse_expression(parser, PREC_ASSIGNMENT);
    AstIfBranches branches = {.then_branch = parse_block(parser)};
    if (match_else(parser)) {
        branches.else_branch = parser->current.kind == TOKEN_IF
                                   ? parse_if(parser) : parse_block(parser);
    }
    uint32_t index = 0;
    if (!failed(parser) && !ast_add_if_branches(&parser->builder, branches, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    parser->recursion_depth--;
    return add_node(parser, AST_IF, keyword, condition, index);
}

static AstNodeId parse_statement(Parser *parser)
{
    Token token = parser->current;
    switch (token.kind) {
    case TOKEN_IF:
        return parse_if(parser);
    case TOKEN_FOR: {
        advance(parser);
        AstNodeId condition = parse_expression(parser, PREC_ASSIGNMENT);
        AstNodeId body = parse_block(parser);
        return add_node(parser, AST_FOR, token, condition, body);
    }
    case TOKEN_RETURN: {
        advance(parser);
        TokenKind next = parser->current.kind;
        AstNodeId value = AST_NODE_NONE;
        if (!is_separator(next) && next != TOKEN_RIGHT_BRACE && next != TOKEN_EOF) {
            value = parse_expression(parser, PREC_ASSIGNMENT);
        }
        return add_node(parser, AST_RETURN, token, value, 0);
    }
    default:
        return parse_expression(parser, PREC_ASSIGNMENT);
    }
}

static AstNodeId parse_block(Parser *parser)
{
    if (!enter(parser)) {
        return AST_NODE_NONE;
    }
    Token opening = expect(parser, TOKEN_LEFT_BRACE, "expected '{' to start block");
    uint32_t mark = parser->builder.pending_count;
    while (!failed(parser) && is_separator(parser->current.kind)) {
        advance(parser);
    }
    while (!failed(parser) && parser->current.kind != TOKEN_RIGHT_BRACE &&
           parser->current.kind != TOKEN_EOF) {
        push_child(parser, parse_statement(parser));
        if (parser->current.kind == TOKEN_EOF) {
            break;
        }
        finish_item(parser, TOKEN_RIGHT_BRACE, "expected a newline, ';', or '}' after statement");
    }
    expect(parser, TOKEN_RIGHT_BRACE, "expected '}' to end block");
    AstNodeRange statements = finish_children(parser, mark);
    parser->recursion_depth--;
    return add_node(parser, AST_BLOCK, opening, statements.first, statements.count);
}

static AstNodeId parse_type(Parser *parser)
{
    Token type = expect(parser, TOKEN_IDENTIFIER, "expected a type name");
    return add_node(parser, AST_TYPE_NAME, type, (uint32_t)type.length, 0);
}

static AstNodeRange parse_parameters(Parser *parser)
{
    uint32_t mark = parser->builder.pending_count;
    if (failed(parser)) {
        return (AstNodeRange){0};
    }
    if (parser->current.kind != TOKEN_LEFT_PAREN) {
        fail(parser, "expected '(' before parameters");
        return (AstNodeRange){0};
    }
    open_group(parser);
    if (parser->current.kind != TOKEN_RIGHT_PAREN) {
        do {
            Token name = expect(parser, TOKEN_IDENTIFIER, "expected a parameter name");
            expect(parser, TOKEN_COLON, "expected ':' after parameter name");
            AstNodeId type = parse_type(parser);
            push_child(parser, add_node(parser, AST_PARAMETER, name, (uint32_t)name.length, type));
        } while (match(parser, TOKEN_COMMA) && parser->current.kind != TOKEN_RIGHT_PAREN);
    }
    close_group(parser, TOKEN_RIGHT_PAREN, "expected ')' after parameters");
    return finish_children(parser, mark);
}

static AstNodeId parse_function(Parser *parser)
{
    Token keyword = expect(parser, TOKEN_FUN, "expected a function declaration");
    Token name = expect(parser, TOKEN_IDENTIFIER, "expected a function name");
    AstFunction function = {
        .name = {(uint32_t)name.offset, (uint32_t)name.length},
        .parameters = parse_parameters(parser),
    };
    if (match(parser, TOKEN_COLON)) {
        function.return_type = parse_type(parser);
    }
    function.body = parse_block(parser);

    uint32_t index = 0;
    if (!failed(parser) && !ast_add_function(&parser->builder, function, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_FUNCTION, keyword, index, 0);
}

bool parser_parse(const unsigned char *source, size_t length, Ast *out, ParseError *error)
{
    *out = (Ast){0};
    *error = (ParseError){0};
    if (length > UINT32_MAX) {
        *error = (ParseError){
            .token = {.line = 1, .column = 1},
            .message = "source is too large for 32-bit AST offsets",
        };
        return false;
    }

    Parser parser = {
        .lexer = lexer_init(source, length),
        .builder.ast = {.source = source, .source_length = (uint32_t)length},
    };
    advance(&parser);
    while (is_separator(parser.current.kind)) {
        advance(&parser);
    }
    while (!failed(&parser) && parser.current.kind != TOKEN_EOF) {
        push_child(&parser, parse_function(&parser));
        finish_item(&parser, TOKEN_EOF, "expected a newline or ';' after function");
    }
    parser.builder.ast.declarations = finish_children(&parser, 0);
    free(parser.builder.pending);

    if (failed(&parser)) {
        *error = parser.error;
        ast_destroy(&parser.builder.ast);
        return false;
    }
    *out = parser.builder.ast;
    return true;
}
