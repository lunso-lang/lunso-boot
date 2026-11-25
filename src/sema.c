#include "sema.h"
#include "literal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AstSpan name;
    SemaType type;
    uint32_t slot;
} Local;

typedef struct {
    const Ast *ast;
    Sema *sema;
    SemaError error;
    uint32_t *function_map;
    size_t function_capacity;
    Local *locals;
    size_t local_count;
    size_t local_capacity;
    size_t scope_start;
    uint32_t next_slot;
    SemaType result;
    unsigned depth;
} Checker;

typedef struct {
    size_t count;
    size_t start;
} Scope;

enum { MAX_ANALYSIS_DEPTH = 256 };

static const SemaType no_type = {.kind = TYPE_INVALID};
static const SemaType bool_type = {.kind = TYPE_BOOL};
static const SemaType void_type = {.kind = TYPE_VOID};

const char *sema_type_name(TypeKind type)
{
    static const char *const names[] = {
        [TYPE_INVALID] = "invalid", [TYPE_VOID] = "void", [TYPE_BOOL] = "bool",
        [TYPE_I8] = "i8", [TYPE_I16] = "i16", [TYPE_I32] = "i32", [TYPE_I64] = "i64",
        [TYPE_U8] = "u8", [TYPE_U16] = "u16", [TYPE_U32] = "u32", [TYPE_U64] = "u64",
        [TYPE_F32] = "f32", [TYPE_F64] = "f64",
        [TYPE_ARRAY] = "array", [TYPE_SLICE] = "slice",
    };
    return names[type];
}

uint32_t sema_scalar_size(TypeKind type)
{
    static const uint8_t sizes[] = {
        [TYPE_INVALID] = 0, [TYPE_VOID] = 0, [TYPE_BOOL] = 1,
        [TYPE_I8] = 1, [TYPE_I16] = 2, [TYPE_I32] = 4, [TYPE_I64] = 8,
        [TYPE_U8] = 1, [TYPE_U16] = 2, [TYPE_U32] = 4, [TYPE_U64] = 8,
        [TYPE_F32] = 4, [TYPE_F64] = 8, [TYPE_ARRAY] = 0, [TYPE_SLICE] = 0,
    };
    return sizes[type];
}

static bool failed(const Checker *checker)
{
    return checker->error.message != NULL;
}

static void fail_at(Checker *checker, uint32_t offset, const char *message)
{
    if (!failed(checker)) {
        checker->error = (SemaError){offset, message};
    }
}

static SemaType fail(Checker *checker, AstNodeId id, const char *message)
{
    fail_at(checker, id ? checker->ast->positions[id] : 0, message);
    return no_type;
}

static bool enter(Checker *checker, AstNodeId id)
{
    if (failed(checker)) {
        return false;
    }
    if (checker->depth == MAX_ANALYSIS_DEPTH) {
        fail(checker, id, "semantic nesting limit exceeded");
        return false;
    }
    checker->depth++;
    return true;
}

static bool is_integer(TypeKind type)
{
    return type >= TYPE_I8 && type <= TYPE_U64;
}

static bool is_signed(TypeKind type)
{
    return type >= TYPE_I8 && type <= TYPE_I64;
}

static bool is_float(TypeKind type)
{
    return type == TYPE_F32 || type == TYPE_F64;
}

static bool is_numeric(TypeKind type)
{
    return is_integer(type) || is_float(type);
}

static bool same_type(SemaType a, SemaType b)
{
    return a.kind == b.kind && a.length == b.length && a.element == b.element;
}

static bool is_array_like(TypeKind type)
{
    return type == TYPE_ARRAY || type == TYPE_SLICE;
}

static AstSpan node_span(const Ast *ast, AstNodeId id)
{
    return (AstSpan){ast->positions[id], ast->data[id].a};
}

static bool same_name(const Ast *ast, AstSpan a, AstSpan b)
{
    return a.length == b.length &&
           memcmp(ast->source + a.offset, ast->source + b.offset, a.length) == 0;
}

static bool name_is(const Ast *ast, AstSpan name, const char *text)
{
    return name.length == strlen(text) &&
           memcmp(ast->source + name.offset, text, name.length) == 0;
}

static bool boolean_name(const Ast *ast, AstSpan name)
{
    return name_is(ast, name, "true") || name_is(ast, name, "false");
}

static SemaType resolve_type(Checker *checker, AstNodeId id)
{
    if (id == AST_NODE_NONE) {
        return void_type;
    }
    if (checker->ast->kinds[id] == AST_ARRAY_TYPE) {
        AstNodeData data = checker->ast->data[id];
        SemaType element = resolve_type(checker, data.a);
        if (failed(checker)) {
            return no_type;
        }
        if (!data.b) {
            return (SemaType){.kind = TYPE_SLICE, .element = element.kind};
        }

        AstSpan count = node_span(checker->ast, data.b);
        uint32_t limit = (INT32_MAX - 7u) / sema_scalar_size(element.kind);
        uint32_t length = 0;
        for (uint32_t i = 0; i < count.length; i++) {
            unsigned digit = checker->ast->source[count.offset + i] - '0';
            if (length > (limit - digit) / 10) {
                return fail(checker, data.b, "array is too large for local storage");
            }
            length = length * 10 + digit;
        }
        return (SemaType){.kind = TYPE_ARRAY, .length = length, .element = element.kind};
    }
    AstSpan name = node_span(checker->ast, id);
    if (name_is(checker->ast, name, "int")) {
        return (SemaType){.kind = TYPE_I32};
    }
    for (TypeKind kind = TYPE_BOOL; kind <= TYPE_F64; kind++) {
        if (name_is(checker->ast, name, sema_type_name(kind))) {
            return (SemaType){.kind = kind};
        }
    }
    return fail(checker, id, "unknown type");
}

static uint32_t *function_entry(const Checker *checker, AstSpan name)
{
    size_t hash = 2166136261u;
    for (uint32_t i = 0; i < name.length; i++) {
        hash = (hash ^ checker->ast->source[name.offset + i]) * 16777619u;
    }
    size_t mask = checker->function_capacity - 1;
    size_t index = hash & mask;
    while (checker->function_map[index]) {
        uint32_t function = checker->function_map[index] - 1;
        if (same_name(checker->ast, name, checker->ast->functions[function].name)) {
            break;
        }
        index = (index + 1) & mask;
    }
    return &checker->function_map[index];
}

static bool register_functions(Checker *checker)
{
    const Ast *ast = checker->ast;
    if (ast->function_count == 0) {
        return true;
    }
    size_t capacity = 2;
    while (capacity / 2 < ast->function_count) {
        if (capacity > SIZE_MAX / 2) {
            return false;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*checker->function_map)) {
        return false;
    }
    checker->function_map = calloc(capacity, sizeof(*checker->function_map));
    if (!checker->function_map) {
        return false;
    }
    checker->function_capacity = capacity;
    for (uint32_t i = 0; i < ast->function_count && !failed(checker); i++) {
        AstFunction function = ast->functions[i];
        uint32_t *entry = function_entry(checker, function.name);
        if (*entry || boolean_name(ast, function.name)) {
            fail_at(checker, function.name.offset, "duplicate or reserved function name");
            break;
        }
        *entry = i + 1;
        checker->sema->functions[i].result = resolve_type(checker, function.return_type);
        if (is_array_like(checker->sema->functions[i].result.kind)) {
            fail(checker, function.return_type, "function results must be scalar");
        }
        for (uint32_t p = 0; p < function.parameters.count && !failed(checker); p++) {
            AstNodeId param = ast->children[function.parameters.first + p];
            AstNodeId annotation = ast->data[param].b;
            SemaType type = resolve_type(checker, annotation);
            if (type.kind == TYPE_ARRAY) {
                fail(checker, annotation, "pass fixed arrays through a slice parameter");
            } else if (type.kind == TYPE_SLICE && !function.body) {
                fail(checker, annotation, "external parameters must be scalar");
            }
            checker->sema->nodes[param].type = type;
        }
    }
    return true;
}

static const Local *find_local(const Checker *checker, AstSpan name, size_t first)
{
    for (size_t i = checker->local_count; i > first; i--) {
        const Local *local = &checker->locals[i - 1];
        if (same_name(checker->ast, name, local->name)) {
            return local;
        }
    }
    return NULL;
}

static Scope open_scope(Checker *checker)
{
    Scope scope = {checker->local_count, checker->scope_start};
    checker->scope_start = checker->local_count;
    return scope;
}

static void close_scope(Checker *checker, Scope scope)
{
    checker->local_count = scope.count;
    checker->scope_start = scope.start;
}

static void bind_local(Checker *checker, AstNodeId id, AstSpan name, SemaType type)
{
    if (failed(checker)) {
        return;
    }
    if (find_local(checker, name, checker->scope_start) || boolean_name(checker->ast, name)) {
        fail(checker, id, "duplicate or reserved local name");
        return;
    }
    if (checker->local_count == checker->local_capacity) {
        if (checker->local_capacity > SIZE_MAX / 2 / sizeof(*checker->locals)) {
            fail(checker, id, "could not allocate local bindings");
            return;
        }
        size_t capacity = checker->local_capacity ? checker->local_capacity * 2 : 16;
        Local *items = realloc(checker->locals, capacity * sizeof(*items));
        if (!items) {
            fail(checker, id, "could not allocate local bindings");
            return;
        }
        checker->locals = items;
        checker->local_capacity = capacity;
    }
    uint32_t slot = checker->next_slot++;
    checker->locals[checker->local_count++] = (Local){name, type, slot};
    checker->sema->nodes[id].binding = slot;
    checker->sema->nodes[id].type = type;
}

static SemaType check_expr(Checker *checker, AstNodeId id, SemaType expected);
static bool check_statement(Checker *checker, AstNodeId id);

static SemaType value(Checker *checker, AstNodeId id, SemaType expected)
{
    SemaType type = check_expr(checker, id, expected);
    if (failed(checker)) {
        return no_type;
    }
    if (type.kind == TYPE_VOID) {
        return fail(checker, id, "a value is required here");
    }
    if (type.kind == TYPE_ARRAY && expected.kind == TYPE_SLICE && type.element == expected.element) {
        checker->sema->nodes[id].borrow_array = true;
        return expected;
    }
    if (expected.kind != TYPE_INVALID && !same_type(type, expected)) {
        return fail(checker, id, "type mismatch");
    }
    return type;
}

static SemaType integer_literal(Checker *checker, AstNodeId id, SemaType expected, bool negative)
{
    const Ast *ast = checker->ast;
    SemaType type = is_integer(expected.kind) ? expected : (SemaType){.kind = TYPE_I32};
    if (negative && !is_signed(type.kind)) {
        return fail(checker, id, "unary '-' requires a signed number");
    }
    unsigned width_index = (unsigned)(type.kind - (is_signed(type.kind) ? TYPE_I8 : TYPE_U8));
    unsigned bits = 8u << width_index;
    uint64_t limit = UINT64_MAX >> (64 - bits);
    if (is_signed(type.kind)) {
        limit = (limit >> 1) + negative;
    }
    AstSpan span = node_span(ast, id);
    uint64_t number = 0;
    for (uint32_t i = 0; i < span.length; i++) {
        unsigned digit = ast->source[span.offset + i] - '0';
        if (number > (limit - digit) / 10) {
            return fail(checker, id, "integer literal is out of range");
        }
        number = number * 10 + digit;
    }
    return type;
}

static SemaType float_literal(Checker *checker, AstNodeId id, SemaType expected)
{
    SemaType type = is_float(expected.kind) ? expected : (SemaType){.kind = TYPE_F64};
    AstSpan span = node_span(checker->ast, id);
    if ((uintmax_t)span.length + 1 > SIZE_MAX) {
        return fail(checker, id, "floating-point literal is too large");
    }
    char scratch[96];
    size_t capacity = (size_t)span.length + 1;
    char *text = capacity <= sizeof(scratch) ? scratch : malloc(capacity);
    if (!text) {
        return fail(checker, id, "could not allocate literal storage");
    }
    memcpy(text, checker->ast->source + span.offset, span.length);
    text[span.length] = '\0';
    bool finite = type.kind == TYPE_F32 ? isfinite(strtof(text, NULL)) : isfinite(strtod(text, NULL));
    if (text != scratch) {
        free(text);
    }
    return finite ? type : fail(checker, id, "floating-point literal is out of range");
}

static SemaType string_literal(Checker *checker, AstNodeId id)
{
    AstSpan span = node_span(checker->ast, id);
    const unsigned char *bytes = checker->ast->source + span.offset;
    uint32_t length = 0;
    for (uint32_t i = 1; i < span.length - 1; length++) {
        if (bytes[i] == 0) {
            fail_at(checker, span.offset + i, "use an escape for a zero byte in a literal");
            return no_type;
        }
        unsigned char byte;
        if (!literal_next_byte(bytes, span.length - 1, &i, &byte)) {
            fail_at(checker, span.offset + i, "invalid byte literal escape");
            return no_type;
        }
    }
    return (SemaType){.kind = TYPE_ARRAY, .length = length, .element = TYPE_U8};
}

static SemaType check_identifier(Checker *checker, AstNodeId id)
{
    AstSpan name = node_span(checker->ast, id);
    if (boolean_name(checker->ast, name)) {
        checker->sema->nodes[id].is_boolean_literal = true;
        checker->sema->nodes[id].binding = name_is(checker->ast, name, "true");
        return bool_type;
    }
    const Local *local = find_local(checker, name, 0);
    if (!local) {
        return fail(checker, id, "unknown variable");
    }
    checker->sema->nodes[id].binding = local->slot;
    return local->type;
}

static SemaType check_call(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    AstNodeId callee = ast->children[node.a];
    if (ast->kinds[callee] != AST_IDENTIFIER) {
        return fail(checker, callee, "expected a named function");
    }
    AstSpan name = node_span(ast, callee);
    if (find_local(checker, name, 0)) {
        return fail(checker, callee, "local variable is not callable");
    }
    uint32_t entry = *function_entry(checker, name);
    if (!entry && name_is(ast, name, "len")) {
        if (node.b != 2) {
            return fail(checker, id, "len expects one array or slice argument");
        }
        SemaType operand = value(checker, ast->children[node.a + 1], no_type);
        if (!is_array_like(operand.kind)) {
            return fail(checker, id, "len expects an array or slice");
        }
        checker->sema->nodes[id].is_builtin_len = true;
        return (SemaType){.kind = TYPE_I64};
    }
    if (!entry) {
        return fail(checker, callee, "unknown function");
    }
    uint32_t index = entry - 1;
    AstNodeRange parameters = ast->functions[index].parameters;
    if (node.b - 1 != parameters.count) {
        return fail(checker, id, "wrong number of function arguments");
    }
    for (uint32_t i = 0; i < parameters.count && !failed(checker); i++) {
        AstNodeId arg = ast->children[node.a + 1 + i];
        AstNodeId param = ast->children[parameters.first + i];
        value(checker, arg, checker->sema->nodes[param].type);
    }
    checker->sema->nodes[id].binding = index;
    return checker->sema->functions[index].result;
}

static bool is_assignment(AstKind kind)
{
    return kind >= AST_ASSIGN && kind <= AST_ASSIGN_SHIFT_RIGHT;
}

static bool is_comparison(AstKind kind)
{
    return kind >= AST_EQUAL && kind <= AST_GREATER_EQUAL;
}

static bool integer_operator(AstKind kind)
{
    return kind == AST_REMAINDER || (kind >= AST_BIT_AND && kind <= AST_SHIFT_RIGHT) ||
           (kind >= AST_ASSIGN_REMAINDER && kind <= AST_ASSIGN_SHIFT_RIGHT);
}

static bool literal_like(const Ast *ast, AstNodeId id)
{
    while (ast->kinds[id] == AST_POSITIVE || ast->kinds[id] == AST_NEGATE) {
        id = ast->data[id].a;
    }
    return ast->kinds[id] == AST_INTEGER || ast->kinds[id] == AST_FLOAT;
}

static SemaType check_binary(Checker *checker, AstNodeId id, SemaType expected)
{
    const Ast *ast = checker->ast;
    AstKind kind = ast->kinds[id];
    AstNodeData node = ast->data[id];
    if (kind == AST_LOGICAL_AND || kind == AST_LOGICAL_OR) {
        value(checker, node.a, bool_type);
        value(checker, node.b, bool_type);
        return bool_type;
    }
    if (is_assignment(kind)) {
        if (ast->kinds[node.a] == AST_INDEX) {
            AstNodeId base = ast->data[node.a].a;
            if (ast->kinds[base] != AST_IDENTIFIER) {
                return fail(checker, node.a, "index assignment requires a local fixed array");
            }
        } else if (ast->kinds[node.a] != AST_IDENTIFIER ||
                   boolean_name(ast, node_span(ast, node.a))) {
            return fail(checker, node.a, "assignment requires a local variable or array element");
        }
    }
    SemaType context = !is_comparison(kind) && is_numeric(expected.kind) ? expected : no_type;
    SemaType operand;
    if (!is_assignment(kind) && context.kind == TYPE_INVALID && literal_like(ast, node.a) &&
        !literal_like(ast, node.b)) {
        operand = value(checker, node.b, no_type);
        value(checker, node.a, operand);
    } else {
        operand = value(checker, node.a, context);
        value(checker, node.b, operand);
    }
    if (failed(checker)) {
        return no_type;
    }
    if (is_assignment(kind) && ast->kinds[node.a] == AST_INDEX &&
        checker->sema->nodes[ast->data[node.a].a].type.kind != TYPE_ARRAY) {
        return fail(checker, node.a, "index assignment requires a local fixed array");
    }
    if (kind == AST_ASSIGN) {
        return operand;
    }
    if ((kind == AST_EQUAL || kind == AST_NOT_EQUAL) && operand.kind == TYPE_BOOL) {
        return bool_type;
    }
    if (integer_operator(kind) ? !is_integer(operand.kind) : !is_numeric(operand.kind)) {
        return fail(checker, id, "operator requires numeric operands of a supported type");
    }
    return is_comparison(kind) ? bool_type : operand;
}

static SemaType check_unary(Checker *checker, AstNodeId id, SemaType expected)
{
    const Ast *ast = checker->ast;
    AstKind kind = ast->kinds[id];
    AstNodeId child = ast->data[id].a;
    if (kind == AST_LOGICAL_NOT) {
        return value(checker, child, bool_type);
    }
    if (kind == AST_NEGATE && ast->kinds[child] == AST_INTEGER) {
        SemaType type = integer_literal(checker, child, expected, true);
        checker->sema->nodes[child].type = type;
        return type;
    }
    SemaType type = value(checker, child, is_numeric(expected.kind) ? expected : no_type);
    if (failed(checker)) {
        return no_type;
    }
    if (kind == AST_BIT_NOT ? !is_integer(type.kind) : !is_numeric(type.kind)) {
        return fail(checker, id, "invalid unary operand type");
    }
    if (kind == AST_NEGATE && !is_signed(type.kind) && !is_float(type.kind)) {
        return fail(checker, id, "unary '-' requires a signed number");
    }
    return type;
}

static SemaType expression_type(Checker *checker, AstNodeId id, SemaType expected)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    switch (ast->kinds[id]) {
    case AST_IDENTIFIER:
        return check_identifier(checker, id);
    case AST_INTEGER:
        return integer_literal(checker, id, expected, false);
    case AST_FLOAT:
        return float_literal(checker, id, expected);
    case AST_STRING:
        return string_literal(checker, id);
    case AST_CALL:
        return check_call(checker, id);
    case AST_INDEX: {
        SemaType base = value(checker, node.a, no_type);
        SemaType index = value(checker, node.b, no_type);
        if (!is_array_like(base.kind) || !is_integer(index.kind)) {
            return fail(checker, id, "indexing requires an array or slice and an integer index");
        }
        return (SemaType){.kind = base.element};
    }
    case AST_SLICE: {
        SemaType base = value(checker, node.a, no_type);
        AstNodeData bounds = ast->data[node.b];
        SemaType start = value(checker, bounds.a, no_type);
        SemaType end = value(checker, bounds.b, no_type);
        if (!is_array_like(base.kind) || !is_integer(start.kind) || !is_integer(end.kind)) {
            return fail(checker, id, "slicing requires an array or slice and integer bounds");
        }
        return (SemaType){.kind = TYPE_SLICE, .element = base.element};
    }
    case AST_MEMBER:
        value(checker, node.a, no_type);
        return fail(checker, id, "member access requires an aggregate with declared fields");
    case AST_POSITIVE:
    case AST_NEGATE:
    case AST_LOGICAL_NOT:
    case AST_BIT_NOT:
        return check_unary(checker, id, expected);
    default:
        return check_binary(checker, id, expected);
    }
}

static SemaType check_expr(Checker *checker, AstNodeId id, SemaType expected)
{
    if (!enter(checker, id)) {
        return no_type;
    }
    SemaType type = expression_type(checker, id, expected);
    checker->depth--;
    checker->sema->nodes[id].type = type;
    return type;
}

static void check_variable(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstVariable variable = ast->variables[ast->data[id].a];
    SemaType type = resolve_type(checker, variable.type);
    if (variable.initializer) {
        value(checker, variable.initializer, type);
    }
    bind_local(checker, id, (AstSpan){ast->positions[id], variable.name_length}, type);
}

static bool check_block(Checker *checker, AstNodeId id)
{
    AstNodeData data = checker->ast->data[id];
    bool falls_through = true;
    for (uint32_t i = 0; i < data.b && !failed(checker); i++) {
        bool child_falls = check_statement(checker, checker->ast->children[data.a + i]);
        falls_through = falls_through && child_falls;
    }
    checker->sema->nodes[id].falls_through = falls_through;
    return falls_through;
}

static bool check_loop(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    AstForHeader header = {.condition = node.a};
    if (ast->kinds[id] == AST_FOR_C_STYLE) {
        header = ast->for_headers[node.a];
    }
    Scope scope = open_scope(checker);
    if (header.initializer) {
        check_statement(checker, header.initializer);
    }
    if (header.condition) {
        value(checker, header.condition, bool_type);
    }
    if (header.step) {
        check_expr(checker, header.step, no_type);
    }
    check_statement(checker, node.b);
    close_scope(checker, scope);
    return header.condition != AST_NODE_NONE;
}

static bool statement_falls_through(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    switch (ast->kinds[id]) {
    case AST_BLOCK: {
        Scope scope = open_scope(checker);
        bool falls = check_block(checker, id);
        close_scope(checker, scope);
        return falls;
    }
    case AST_RETURN:
        if (node.a && checker->result.kind == TYPE_VOID) {
            fail(checker, id, "a function without a result cannot return a value");
        } else if (node.a) {
            value(checker, node.a, checker->result);
        } else if (checker->result.kind != TYPE_VOID) {
            fail(checker, id, "return value is required");
        }
        return false;
    case AST_IF: {
        value(checker, node.a, bool_type);
        AstIfBranches branches = ast->if_branches[node.b];
        bool then_falls = check_statement(checker, branches.then_branch);
        bool else_falls = !branches.else_branch || check_statement(checker, branches.else_branch);
        return then_falls || else_falls;
    }
    case AST_FOR:
    case AST_FOR_C_STYLE:
        return check_loop(checker, id);
    case AST_VAR:
        check_variable(checker, id);
        return true;
    default:
        check_expr(checker, id, no_type);
        return true;
    }
}

static bool check_statement(Checker *checker, AstNodeId id)
{
    if (!enter(checker, id)) {
        return false;
    }
    bool falls = statement_falls_through(checker, id);
    checker->depth--;
    checker->sema->nodes[id].falls_through = falls;
    return falls;
}

static void check_function(Checker *checker, uint32_t index)
{
    const Ast *ast = checker->ast;
    AstFunction function = ast->functions[index];
    SemaFunction *info = &checker->sema->functions[index];
    checker->result = info->result;
    checker->local_count = 0;
    checker->scope_start = 0;
    checker->next_slot = 0;
    for (uint32_t i = 0; i < function.parameters.count && !failed(checker); i++) {
        AstNodeId param = ast->children[function.parameters.first + i];
        bind_local(checker, param, node_span(ast, param), checker->sema->nodes[param].type);
    }
    if (function.body) {
        bool falls = check_block(checker, function.body);
        if (falls && info->result.kind != TYPE_VOID) {
            fail_at(checker, function.name.offset, "function can finish without returning a value");
        }
        info->slot_count = checker->next_slot;
    }
}

void sema_destroy(Sema *sema)
{
    free(sema->nodes);
    free(sema->functions);
    *sema = (Sema){0};
}

bool sema_check(const Ast *ast, Sema *out, SemaError *error)
{
    *out = (Sema){0};
    *error = (SemaError){0};
    if (ast->function_count == 0) {
        return true;
    }
    out->nodes = calloc(ast->node_count, sizeof(*out->nodes));
    out->functions = calloc(ast->function_count, sizeof(*out->functions));
    Checker checker = {.ast = ast, .sema = out};
    if (!out->nodes || !out->functions || !register_functions(&checker)) {
        fail_at(&checker, 0, "could not allocate semantic analysis storage");
    }
    for (uint32_t i = 0; i < ast->function_count && !failed(&checker); i++) {
        check_function(&checker, i);
    }
    free(checker.locals);
    free(checker.function_map);
    if (failed(&checker)) {
        *error = checker.error;
        sema_destroy(out);
        return false;
    }
    return true;
}
