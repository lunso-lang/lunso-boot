#include "parser.h"

#include "ast_builder.h"

#include <stdlib.h>

typedef struct {
    Lexer lexer;
    Token current;
    ParseError error;
    AstBuilder *builder;
    uint32_t source_base;
    uint32_t module;
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
    PREC_CAST,
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
static AstNodeId parse_type(Parser *parser);

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
    AstNodeId id = ast_add_node(parser->builder, kind, parser->source_base + (uint32_t)token.offset,
                               (AstNodeData){a, b});
    if (id == AST_NODE_NONE) {
        fail(parser, "could not allocate AST storage");
    }
    return id;
}

static void push_child(Parser *parser, AstNodeId child)
{
    if (!failed(parser) && !ast_push_child(parser->builder, child)) {
        fail(parser, "could not allocate AST storage");
    }
}

static AstNodeRange finish_children(Parser *parser, uint32_t mark)
{
    AstNodeRange range = {0};
    if (!failed(parser) && !ast_finish_children(parser->builder, mark, &range)) {
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
    if (message && !failed(parser) && parser->current.kind != end &&
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
    uint32_t mark = parser->builder->pending_count;
    push_child(parser, callee);
    open_group(parser);
    while (!failed(parser) && parser->current.kind != TOKEN_RIGHT_PAREN &&
           parser->current.kind != TOKEN_EOF) {
        push_child(parser, parse_expression(parser, PREC_ASSIGNMENT));
        if (parser->current.kind == TOKEN_RIGHT_PAREN || parser->current.kind == TOKEN_EOF) {
            break;
        }
        expect(parser, TOKEN_COMMA, "expected ',' or ')' after argument");
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
            if (parser->current.kind == TOKEN_DOT_DOT && !failed(parser)) {
                Token range = parser->current;
                advance(parser);
                AstNodeId end = parse_expression(parser, PREC_ASSIGNMENT);
                close_group(parser, TOKEN_RIGHT_BRACKET, "expected ']' after slice range");
                AstNodeId bounds = add_node(parser, AST_SLICE_RANGE, range, index, end);
                base = add_node(parser, AST_SLICE, token, base, bounds);
            } else {
                close_group(parser, TOKEN_RIGHT_BRACKET, "expected ']' after index");
                base = add_node(parser, AST_INDEX, token, base, index);
            }
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
    case TOKEN_AMPERSAND:
        kind = AST_ADDRESS_OF;
        break;
    case TOKEN_STAR:
        kind = AST_DEREFERENCE;
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
    while (!failed(parser)) {
        Token token = parser->current;
        if (token.kind == TOKEN_AS && minimum <= PREC_CAST) {
            advance(parser);
            AstNodeId type = parse_type(parser);
            left = add_node(parser, AST_CAST, token, left, type);
            continue;
        }
        if ((size_t)token.kind >= sizeof(infix_operators) / sizeof(infix_operators[0])) {
            break;
        }
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
    if (!failed(parser) && !ast_add_if_branches(parser->builder, branches, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    parser->recursion_depth--;
    return add_node(parser, AST_IF, keyword, condition, index);
}

static AstNodeId parse_type_name(Parser *parser)
{
    Token type = expect(parser, TOKEN_IDENTIFIER, "expected a type name");
    if (!match(parser, TOKEN_DOT)) {
        return add_node(parser, AST_TYPE_NAME, type, (uint32_t)type.length, 0);
    }
    uint32_t mark = parser->builder->pending_count;
    push_child(parser, add_node(parser, AST_IDENTIFIER, type, (uint32_t)type.length, 0));
    do {
        Token part = expect(parser, TOKEN_IDENTIFIER, "expected a type path component");
        push_child(parser, add_node(parser, AST_IDENTIFIER, part, (uint32_t)part.length, 0));
    } while (match(parser, TOKEN_DOT));
    AstNodeRange path = finish_children(parser, mark);
    return add_node(parser, AST_QUALIFIED_TYPE, type, path.first, path.count);
}

static AstNodeId parse_type_inner(Parser *parser)
{
    Token opening = parser->current;
    if (match(parser, TOKEN_STAR)) {
        bool read_only = match(parser, TOKEN_CONST);
        AstNodeId pointee = parse_type(parser);
        return add_node(parser, AST_POINTER_TYPE, opening, pointee, read_only);
    }
    if (opening.kind != TOKEN_LEFT_BRACKET) {
        return parse_type_name(parser);
    }
    open_group(parser);
    AstNodeId count = AST_NODE_NONE;
    if (parser->current.kind != TOKEN_RIGHT_BRACKET) {
        Token literal = expect(parser, TOKEN_INTEGER, "expected an integer array length or ']'");
        count = add_node(parser, AST_INTEGER, literal, (uint32_t)literal.length, 0);
    }
    close_group(parser, TOKEN_RIGHT_BRACKET, "expected ']' after array length");
    AstNodeId element = parse_type(parser);
    return add_node(parser, AST_ARRAY_TYPE, opening, element, count);
}

static AstNodeId parse_type(Parser *parser)
{
    if (!enter(parser)) {
        return AST_NODE_NONE;
    }
    AstNodeId type = parse_type_inner(parser);
    parser->recursion_depth--;
    return type;
}

static AstNodeId parse_variable(Parser *parser)
{
    advance(parser);
    Token name = expect(parser, TOKEN_IDENTIFIER, "expected a variable name after 'var'");
    AstVariable variable = {.name_length = (uint32_t)name.length};
    if (match(parser, TOKEN_COLON)) {
        variable.type = parse_type(parser);
    }
    if (match(parser, TOKEN_EQUAL)) {
        variable.initializer = parse_expression(parser, PREC_ASSIGNMENT);
    } else if (!variable.type) {
        fail(parser, "expected ':' or '=' after variable name");
    }

    uint32_t index = 0;
    if (!failed(parser) && !ast_add_variable(parser->builder, variable, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_VAR, name, index, 0);
}

static AstNodeId parse_for(Parser *parser)
{
    Token keyword = parser->current;
    advance(parser);
    if (parser->current.kind == TOKEN_LEFT_BRACE) {
        AstNodeId body = parse_block(parser);
        return add_node(parser, AST_FOR, keyword, AST_NODE_NONE, body);
    }

    bool declares_variable = parser->current.kind == TOKEN_VAR;
    AstNodeId first = AST_NODE_NONE;
    if (declares_variable) {
        first = parse_variable(parser);
    } else if (parser->current.kind != TOKEN_SEMICOLON) {
        first = parse_expression(parser, PREC_ASSIGNMENT);
    }

    if (!match(parser, TOKEN_SEMICOLON)) {
        if (declares_variable) {
            fail(parser, "expected ';' after for initializer");
        }
        AstNodeId body = parse_block(parser);
        return add_node(parser, AST_FOR, keyword, first, body);
    }

    AstForHeader header = {.initializer = first};
    if (parser->current.kind != TOKEN_SEMICOLON) {
        header.condition = parse_expression(parser, PREC_ASSIGNMENT);
    }
    expect(parser, TOKEN_SEMICOLON, "expected ';' after for condition");
    if (parser->current.kind != TOKEN_LEFT_BRACE) {
        header.step = parse_expression(parser, PREC_ASSIGNMENT);
    }
    AstNodeId body = parse_block(parser);

    uint32_t index = 0;
    if (!failed(parser) && !ast_add_for_header(parser->builder, header, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_FOR_C_STYLE, keyword, index, body);
}

static AstNodeId parse_statement(Parser *parser)
{
    Token token = parser->current;
    switch (token.kind) {
    case TOKEN_IF:
        return parse_if(parser);
    case TOKEN_FOR:
        return parse_for(parser);
    case TOKEN_VAR:
        return parse_variable(parser);
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
    uint32_t mark = parser->builder->pending_count;
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

static AstNodeRange parse_parameters(Parser *parser)
{
    uint32_t mark = parser->builder->pending_count;
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

static AstSpan source_span(const Parser *parser, Token token)
{
    return (AstSpan){parser->source_base + (uint32_t)token.offset, (uint32_t)token.length};
}

static AstNodeId parse_function(Parser *parser, bool public)
{
    Token start = parser->current;
    bool external = match(parser, TOKEN_EXT);
    expect(parser, TOKEN_FUN, external ? "expected 'fun' after 'ext'"
                                      : "expected a function declaration");
    Token name = expect(parser, TOKEN_IDENTIFIER, "expected a function name");
    AstFunction function = {
        .name = source_span(parser, name),
        .parameters = parse_parameters(parser),
        .module = parser->module,
        .public = public,
    };
    if (match(parser, TOKEN_COLON)) {
        function.return_type = parse_type(parser);
    }
    if (!external) {
        function.body = parse_block(parser);
    } else if (parser->current.kind == TOKEN_LEFT_BRACE) {
        fail(parser, "an external function declaration cannot have a body");
    }

    uint32_t index = 0;
    if (!failed(parser) && !ast_add_function(parser->builder, function, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_FUNCTION, start, index, 0);
}

static AstNodeId parse_struct(Parser *parser, bool public)
{
    Token keyword = expect(parser, TOKEN_STRUCT, "expected 'struct'");
    Token name = expect(parser, TOKEN_IDENTIFIER, "expected a struct name");
    expect(parser, TOKEN_LEFT_BRACE, "expected '{' after struct name");
    uint32_t mark = parser->builder->pending_count;
    while (!failed(parser) && is_separator(parser->current.kind)) {
        advance(parser);
    }
    while (!failed(parser) && parser->current.kind != TOKEN_RIGHT_BRACE &&
           parser->current.kind != TOKEN_EOF) {
        Token field = expect(parser, TOKEN_IDENTIFIER, "expected a field name");
        expect(parser, TOKEN_COLON, "expected ':' after field name");
        AstNodeId type = parse_type(parser);
        push_child(parser, add_node(parser, AST_FIELD, field, (uint32_t)field.length, type));
        if (parser->current.kind == TOKEN_EOF) {
            break;
        }
        finish_item(parser, TOKEN_RIGHT_BRACE, "expected a newline, ';', or '}' after field");
    }
    expect(parser, TOKEN_RIGHT_BRACE, "expected '}' to end struct");
    AstStruct record = {
        .name = source_span(parser, name),
        .fields = finish_children(parser, mark),
        .module = parser->module,
        .public = public,
    };
    uint32_t index = 0;
    if (!failed(parser) && !ast_add_struct(parser->builder, record, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_STRUCT, keyword, index, 0);
}

static AstNodeId parse_enum(Parser *parser, bool public)
{
    Token keyword = expect(parser, TOKEN_ENUM, "expected 'enum'");
    Token name = expect(parser, TOKEN_IDENTIFIER, "expected an enum name");
    expect(parser, TOKEN_COLON, "expected ':' before enum backing type");
    Token type = expect(parser, TOKEN_IDENTIFIER, "expected an integer backing type");
    AstNodeId backing = add_node(parser, AST_TYPE_NAME, type, (uint32_t)type.length, 0);
    expect(parser, TOKEN_LEFT_BRACE, "expected '{' after enum backing type");

    uint32_t mark = parser->builder->pending_count;
    while (!failed(parser) && is_separator(parser->current.kind)) {
        advance(parser);
    }
    while (!failed(parser) && parser->current.kind != TOKEN_RIGHT_BRACE &&
           parser->current.kind != TOKEN_EOF) {
        Token member = expect(parser, TOKEN_IDENTIFIER, "expected an enum member name");
        AstNodeId value = AST_NODE_NONE;
        if (match(parser, TOKEN_EQUAL)) {
            Token sign = parser->current;
            bool negative = match(parser, TOKEN_MINUS);
            Token literal = expect(parser, TOKEN_INTEGER, "expected an integer enum value");
            value = add_node(parser, AST_INTEGER, literal, (uint32_t)literal.length, 0);
            if (negative) {
                value = add_node(parser, AST_NEGATE, sign, value, 0);
            }
        }
        push_child(parser, add_node(parser, AST_ENUM_CASE, member, (uint32_t)member.length, value));
        const char *separator = match(parser, TOKEN_COMMA)
                                ? NULL : "expected ',', a newline, ';', or '}' after enum member";
        finish_item(parser, TOKEN_RIGHT_BRACE, separator);
    }
    expect(parser, TOKEN_RIGHT_BRACE, "expected '}' after enum");
    AstEnum enumeration = {
        .name = source_span(parser, name),
        .cases = finish_children(parser, mark),
        .backing = backing,
        .module = parser->module,
        .public = public,
    };
    uint32_t index = 0;
    if (!failed(parser) && !ast_add_enum(parser->builder, enumeration, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_ENUM, keyword, index, 0);
}

static AstNodeId parse_import(Parser *parser)
{
    Token keyword = expect(parser, TOKEN_IMPORT, "expected 'import'");
    AstImport item = {0};
    uint32_t mark = parser->builder->pending_count;
    do {
        if (parser->builder->pending_count != mark && match(parser, TOKEN_STAR)) {
            item.kind = IMPORT_ALL;
            break;
        }
        Token part = expect(parser, TOKEN_IDENTIFIER, "expected a module path component");
        push_child(parser, add_node(parser, AST_IDENTIFIER, part, (uint32_t)part.length, 0));
    } while (match(parser, TOKEN_DOT));
    item.path = finish_children(parser, mark);

    if (item.kind == IMPORT_MODULE && match(parser, TOKEN_AS)) {
        item.alias = source_span(parser, expect(parser, TOKEN_IDENTIFIER, "expected an import alias"));
    } else if (item.kind == IMPORT_MODULE && parser->current.kind == TOKEN_LEFT_BRACE) {
        item.kind = IMPORT_SELECTED;
        open_group(parser);
        do {
            Token name = expect(parser, TOKEN_IDENTIFIER, "expected an imported name");
            AstNodeId alias = AST_NODE_NONE;
            if (match(parser, TOKEN_AS)) {
                Token token = expect(parser, TOKEN_IDENTIFIER, "expected an import alias");
                alias = add_node(parser, AST_IDENTIFIER, token, (uint32_t)token.length, 0);
            }
            push_child(parser, add_node(parser, AST_IMPORT_ITEM, name, (uint32_t)name.length, alias));
        } while (match(parser, TOKEN_COMMA) && parser->current.kind != TOKEN_RIGHT_BRACE);
        close_group(parser, TOKEN_RIGHT_BRACE, "expected '}' after imported names");
        item.items = finish_children(parser, mark);
    }

    uint32_t index = 0;
    if (!failed(parser) && !ast_add_import(parser->builder, item, &index)) {
        fail(parser, "could not allocate AST storage");
    }
    return add_node(parser, AST_IMPORT, keyword, index, 0);
}

bool parser_append(AstBuilder *builder, const unsigned char *source, uint32_t length,
                   uint32_t source_base, uint32_t module, AstNodeRange *out, ParseError *error)
{
    *error = (ParseError){0};
    Parser parser = {.lexer = lexer_init(source, length), .builder = builder,
                     .source_base = source_base, .module = module};
    uint32_t mark = builder->pending_count;
    advance(&parser);
    while (is_separator(parser.current.kind)) {
        advance(&parser);
    }
    while (!failed(&parser) && parser.current.kind != TOKEN_EOF) {
        const char *message;
        AstNodeId declaration;
        if (parser.current.kind == TOKEN_IMPORT) {
            declaration = parse_import(&parser);
            message = "expected a newline or ';' after import";
        } else {
            bool public = match(&parser, TOKEN_PUB);
            if (!public) {
                match(&parser, TOKEN_PRIV);
            }
            switch (parser.current.kind) {
            case TOKEN_STRUCT:
                declaration = parse_struct(&parser, public);
                message = NULL;
                break;
            case TOKEN_ENUM:
                declaration = parse_enum(&parser, public);
                message = NULL;
                break;
            default:
                declaration = parse_function(&parser, public);
                message = "expected a newline or ';' after function";
                break;
            }
        }
        push_child(&parser, declaration);
        finish_item(&parser, TOKEN_EOF, message);
    }
    *out = finish_children(&parser, mark);
    *error = parser.error;
    return !failed(&parser);
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

    AstBuilder builder = {.ast = {.source = source, .source_length = (uint32_t)length}};
    bool parsed = parser_append(&builder, source, (uint32_t)length, 0, 0,
                                &builder.ast.declarations, error);
    free(builder.pending);
    if (!parsed) {
        ast_destroy(&builder.ast);
        return false;
    }
    *out = builder.ast;
    return true;
}
