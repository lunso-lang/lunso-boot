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
    uint32_t *struct_sizes;
    uint32_t pointee_capacity;
    Local *locals;
    size_t local_count;
    size_t local_capacity;
    size_t scope_start;
    uint32_t next_slot;
    SemaType result;
    uint32_t module;
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
        [TYPE_STRUCT] = "struct", [TYPE_ARRAY] = "array", [TYPE_SLICE] = "slice",
        [TYPE_POINTER] = "pointer",
    };
    return names[type];
}

uint32_t sema_scalar_size(TypeKind type)
{
    static const uint8_t sizes[] = {
        [TYPE_INVALID] = 0, [TYPE_VOID] = 0, [TYPE_BOOL] = 1,
        [TYPE_I8] = 1, [TYPE_I16] = 2, [TYPE_I32] = 4, [TYPE_I64] = 8,
        [TYPE_U8] = 1, [TYPE_U16] = 2, [TYPE_U32] = 4, [TYPE_U64] = 8,
        [TYPE_F32] = 4, [TYPE_F64] = 8, [TYPE_STRUCT] = 0, [TYPE_ARRAY] = 0, [TYPE_SLICE] = 0,
        [TYPE_POINTER] = 8,
    };
    return sizes[type];
}

uint32_t sema_element_size(SemaType type)
{
    return type.element == TYPE_STRUCT ? type.record_size : sema_scalar_size(type.element);
}

uint32_t sema_storage_size(SemaType type)
{
    return type.kind == TYPE_STRUCT ? type.record_size : type.length * sema_element_size(type);
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

static bool can_promote(TypeKind source, TypeKind target)
{
    if (source == TYPE_F32 && target == TYPE_F64) {
        return true;
    }
    if (!is_integer(source) || !is_integer(target)) {
        return false;
    }
    uint32_t from = sema_scalar_size(source);
    uint32_t to = sema_scalar_size(target);
    return from < to && (!is_signed(source) || is_signed(target));
}

static TypeKind common_numeric_type(TypeKind left, TypeKind right)
{
    if (left == right || can_promote(right, left)) {
        return left;
    }
    if (can_promote(left, right)) {
        return right;
    }
    if (is_integer(left) && is_integer(right)) {
        for (TypeKind kind = TYPE_I8; kind <= TYPE_I64; kind++) {
            if ((left == kind || can_promote(left, kind)) &&
                (right == kind || can_promote(right, kind))) {
                return kind;
            }
        }
    }
    return TYPE_INVALID;
}

static bool same_type(SemaType a, SemaType b)
{
    return a.kind == b.kind && a.length == b.length && a.element == b.element &&
           a.record == b.record && a.pointee == b.pointee && a.read_only == b.read_only;
}

static bool is_array_like(TypeKind type)
{
    return type == TYPE_ARRAY || type == TYPE_SLICE;
}

static SemaType element_type(SemaType collection)
{
    return (SemaType){.kind = collection.element, .record = collection.record,
                      .record_size = collection.record_size, .pointee = collection.pointee,
                      .read_only = collection.read_only};
}

static SemaType pointer_to(Checker *checker, AstNodeId id, SemaType pointee, bool read_only)
{
    if (failed(checker)) {
        return no_type;
    }
    if (is_array_like(pointee.kind)) {
        return fail(checker, id, "take the address of an array element instead");
    }
    SemaType result = {.kind = TYPE_POINTER, .read_only = read_only};
    Sema *sema = checker->sema;
    for (uint32_t i = 0; i < sema->pointee_count; i++) {
        if (same_type(sema->pointees[i], pointee)) {
            result.pointee = i;
            return result;
        }
    }
    if (sema->pointee_count == checker->pointee_capacity) {
        uint32_t capacity = checker->pointee_capacity;
        if (capacity > UINT32_MAX / 2) {
            return fail(checker, id, "could not allocate pointer type storage");
        }
        capacity = capacity ? capacity * 2 : 16;
        size_t element_size = sizeof(*sema->pointees);
        if (capacity > SIZE_MAX / element_size) {
            return fail(checker, id, "could not allocate pointer type storage");
        }
        SemaType *pointees = realloc(sema->pointees, capacity * element_size);
        if (!pointees) {
            return fail(checker, id, "could not allocate pointer type storage");
        }
        sema->pointees = pointees;
        checker->pointee_capacity = capacity;
    }
    uint32_t index = sema->pointee_count++;
    sema->pointees[index] = pointee;
    result.pointee = index;
    return result;
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

static TypeKind scalar_type(const Ast *ast, AstSpan name)
{
    if (name_is(ast, name, "int")) {
        return TYPE_I32;
    }
    for (TypeKind kind = TYPE_BOOL; kind <= TYPE_F64; kind++) {
        if (name_is(ast, name, sema_type_name(kind))) {
            return kind;
        }
    }
    return TYPE_INVALID;
}

static uint32_t find_struct(const Ast *ast, uint32_t module, AstSpan name)
{
    for (uint32_t i = 0; i < ast->struct_count; i++) {
        if (ast->structs[i].module == module && same_name(ast, name, ast->structs[i].name)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint64_t align_bytes(uint64_t size, uint32_t alignment)
{
    return (size + alignment - 1) & ~((uint64_t)alignment - 1);
}

static void layout_struct(Checker *checker, uint32_t index)
{
    const Ast *ast = checker->ast;
    AstStruct record = ast->structs[index];
    if (scalar_type(ast, record.name) != TYPE_INVALID ||
        find_struct(ast, record.module, record.name) != index) {
        fail_at(checker, record.name.offset, "duplicate or reserved struct name");
        return;
    }
    if (!record.fields.count) {
        fail_at(checker, record.name.offset, "struct must have at least one field");
        return;
    }

    const AstNodeId *fields = ast->children + record.fields.first;
    uint64_t size = 0;
    uint32_t alignment = 1;
    for (uint32_t i = 0; i < record.fields.count; i++) {
        AstNodeId field = fields[i];
        AstSpan name = node_span(ast, field);
        for (uint32_t j = 0; j < i; j++) {
            if (same_name(ast, name, node_span(ast, fields[j]))) {
                fail(checker, field, "duplicate field name");
                return;
            }
        }
        AstNodeId annotation = ast->data[field].b;
        AstKind annotation_kind = ast->kinds[annotation];
        if (annotation_kind != AST_TYPE_NAME && annotation_kind != AST_POINTER_TYPE) {
            fail(checker, annotation, "struct fields must be scalar");
            return;
        }
        AstSpan field_type = node_span(ast, annotation);
        TypeKind kind = annotation_kind == AST_POINTER_TYPE ? TYPE_POINTER
                                                            : scalar_type(ast, field_type);
        if (kind == TYPE_INVALID) {
            fail(checker, annotation, find_struct(ast, record.module, field_type) == UINT32_MAX
                                      ? "unknown type" : "struct fields must be scalar");
            return;
        }
        uint32_t width = sema_scalar_size(kind);
        size = align_bytes(size, width);
        if (size + width > INT32_MAX - 7u) {
            fail(checker, field, "struct is too large for local storage");
            return;
        }
        checker->sema->nodes[field].type = (SemaType){.kind = kind};
        checker->sema->nodes[field].binding = (uint32_t)size;
        size += width;
        if (width > alignment) {
            alignment = width;
        }
    }
    checker->struct_sizes[index] = (uint32_t)align_bytes(size, alignment);
}

static SemaType resolve_type(Checker *checker, AstNodeId id)
{
    if (id == AST_NODE_NONE) {
        return void_type;
    }
    AstKind ast_kind = checker->ast->kinds[id];
    if (ast_kind == AST_QUALIFIED_TYPE) {
        const AstQualified *qualified = ast_qualified(checker->ast, id);
        if (!qualified || qualified->kind != BIND_STRUCT) {
            return fail(checker, id, "unknown qualified type");
        }
        return (SemaType){.kind = TYPE_STRUCT, .record = qualified->index,
                          .record_size = checker->struct_sizes[qualified->index]};
    }
    if (ast_kind == AST_POINTER_TYPE) {
        SemaType pointee = resolve_type(checker, checker->ast->data[id].a);
        return pointer_to(checker, id, pointee, checker->ast->data[id].b != 0);
    }
    if (ast_kind == AST_ARRAY_TYPE) {
        AstNodeData data = checker->ast->data[id];
        SemaType element = resolve_type(checker, data.a);
        if (failed(checker)) {
            return no_type;
        }
        if (is_array_like(element.kind)) {
            return fail(checker, data.a, "collection elements must have a scalar or struct type");
        }
        SemaType collection = {.kind = TYPE_SLICE, .element = element.kind,
                               .record = element.record, .record_size = element.record_size,
                               .pointee = element.pointee, .read_only = element.read_only};
        if (!data.b) {
            return collection;
        }

        AstSpan count = node_span(checker->ast, data.b);
        uint32_t limit = (INT32_MAX - 7u) / sema_element_size(collection);
        uint32_t length = 0;
        for (uint32_t i = 0; i < count.length; i++) {
            unsigned digit = checker->ast->source[count.offset + i] - '0';
            if (digit > limit || length > (limit - digit) / 10) {
                return fail(checker, data.b, "array is too large for local storage");
            }
            length = length * 10 + digit;
        }
        collection.kind = TYPE_ARRAY;
        collection.length = length;
        return collection;
    }
    AstSpan name = node_span(checker->ast, id);
    const AstBinding *import = ast_imported(checker->ast, checker->module, name);
    if (import && import->kind == BIND_STRUCT) {
        return (SemaType){.kind = TYPE_STRUCT, .record = import->index,
                          .record_size = checker->struct_sizes[import->index]};
    }
    TypeKind kind = scalar_type(checker->ast, name);
    if (kind != TYPE_INVALID) {
        return (SemaType){.kind = kind};
    }
    uint32_t record = find_struct(checker->ast, checker->module, name);
    if (record != UINT32_MAX) {
        return (SemaType){.kind = TYPE_STRUCT, .record = record,
                          .record_size = checker->struct_sizes[record]};
    }
    return fail(checker, id, "unknown type");
}

static void resolve_pointer_fields(Checker *checker, AstStruct record)
{
    const Ast *ast = checker->ast;
    checker->module = record.module;
    for (uint32_t i = 0; i < record.fields.count && !failed(checker); i++) {
        AstNodeId field = ast->children[record.fields.first + i];
        AstNodeId annotation = ast->data[field].b;
        if (ast->kinds[annotation] == AST_POINTER_TYPE) {
            checker->sema->nodes[field].type = resolve_type(checker, annotation);
        }
    }
}

static uint32_t *function_entry(const Checker *checker, AstSpan name)
{
    size_t hash = (2166136261u ^ checker->module) * 16777619u;
    for (uint32_t i = 0; i < name.length; i++) {
        hash = (hash ^ checker->ast->source[name.offset + i]) * 16777619u;
    }
    size_t mask = checker->function_capacity - 1;
    size_t index = hash & mask;
    while (checker->function_map[index]) {
        uint32_t function = checker->function_map[index] - 1;
        if (checker->ast->functions[function].module == checker->module &&
            same_name(checker->ast, name, checker->ast->functions[function].name)) {
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
        checker->module = function.module;
        uint32_t *entry = function_entry(checker, function.name);
        if (*entry || boolean_name(ast, function.name)) {
            fail_at(checker, function.name.offset, "duplicate or reserved function name");
            break;
        }
        *entry = i + 1;
        checker->sema->functions[i].result = resolve_type(checker, function.return_type);
        TypeKind result = checker->sema->functions[i].result.kind;
        if (is_array_like(result) || result == TYPE_STRUCT) {
            fail(checker, function.return_type, "function results must be scalar");
        }
        for (uint32_t p = 0; p < function.parameters.count && !failed(checker); p++) {
            AstNodeId param = ast->children[function.parameters.first + p];
            AstNodeId annotation = ast->data[param].b;
            SemaType type = resolve_type(checker, annotation);
            if (!function.body && (is_array_like(type.kind) || type.kind == TYPE_STRUCT)) {
                fail(checker, annotation, "external parameters must be scalar");
            } else if (type.kind == TYPE_ARRAY) {
                fail(checker, annotation, "pass fixed arrays through a slice parameter");
            } else if (type.kind == TYPE_STRUCT) {
                fail(checker, annotation, "pass structs through a slice parameter");
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

static SemaType check_value(Checker *checker, AstNodeId id, SemaType hint)
{
    SemaType type = check_expr(checker, id, hint);
    if (failed(checker)) {
        return no_type;
    }
    if (type.kind == TYPE_VOID) {
        return fail(checker, id, "a value is required here");
    }
    return type;
}

static void promote(Checker *checker, AstNodeId id, TypeKind target)
{
    SemaNode *node = &checker->sema->nodes[id];
    if (node->type.kind != target) {
        node->promotion = target;
    }
}

static SemaType value(Checker *checker, AstNodeId id, SemaType expected)
{
    SemaType type = check_value(checker, id, expected);
    if (failed(checker)) {
        return no_type;
    }
    if (type.kind == TYPE_ARRAY && expected.kind == TYPE_SLICE &&
        type.element == expected.element && type.record == expected.record &&
        type.pointee == expected.pointee && type.read_only == expected.read_only) {
        checker->sema->nodes[id].borrow_array = true;
        return expected;
    }
    if (type.kind == TYPE_POINTER && expected.kind == TYPE_POINTER &&
        type.pointee == expected.pointee && expected.read_only) {
        return expected;
    }
    if (can_promote(type.kind, expected.kind)) {
        promote(checker, id, expected.kind);
        return expected;
    }
    if (expected.kind != TYPE_INVALID && !same_type(type, expected)) {
        return fail(checker, id, "type mismatch");
    }
    return type;
}

static bool integer_magnitude(const Ast *ast, AstNodeId id, uint64_t *number)
{
    AstSpan span = node_span(ast, id);
    *number = 0;
    for (uint32_t i = 0; i < span.length; i++) {
        unsigned digit = ast->source[span.offset + i] - '0';
        if (*number > (UINT64_MAX - digit) / 10) {
            return false;
        }
        *number = *number * 10 + digit;
    }
    return true;
}

static bool integer_fits(TypeKind type, uint64_t number, bool negative)
{
    if (negative && !is_signed(type)) {
        return false;
    }
    uint32_t bits = 8 * sema_scalar_size(type);
    uint64_t limit = UINT64_MAX >> (64 - bits);
    if (is_signed(type)) {
        limit = (limit >> 1) + negative;
    }
    return number <= limit;
}

static SemaType integer_literal(Checker *checker, AstNodeId id, SemaType expected, bool negative)
{
    if (negative && is_integer(expected.kind) && !is_signed(expected.kind)) {
        return fail(checker, id, "unary '-' requires a signed number");
    }
    uint64_t number;
    if (!integer_magnitude(checker->ast, id, &number)) {
        return fail(checker, id, "integer literal is out of range");
    }
    TypeKind kind = expected.kind;
    if (!is_integer(kind)) {
        kind = integer_fits(TYPE_I32, number, negative) ? TYPE_I32 : TYPE_I64;
    }
    if (!integer_fits(kind, number, negative)) {
        return fail(checker, id, "integer literal is out of range");
    }
    return (SemaType){.kind = kind};
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

static SemaType check_collection_builtin(Checker *checker, AstNodeId id, SemaBuiltin builtin)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    bool length = builtin == BUILTIN_LEN;
    if (node.b != 2) {
        return fail(checker, id, length ? "len expects one array or slice argument"
                                        : "data expects one array or slice argument");
    }
    SemaType operand = value(checker, ast->children[node.a + 1], no_type);
    if (!is_array_like(operand.kind)) {
        return fail(checker, id, length ? "len expects an array or slice"
                                        : "data expects an array or slice");
    }
    checker->sema->nodes[id].builtin = builtin;
    return length ? (SemaType){.kind = TYPE_I64}
                  : pointer_to(checker, id, element_type(operand), true);
}

static SemaType check_size_of(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    if (node.b != 2) {
        return fail(checker, id, "size_of expects one argument");
    }
    SemaType type = value(checker, ast->children[node.a + 1], no_type);
    uint32_t size = sema_scalar_size(type.kind);
    if (type.kind == TYPE_STRUCT || type.kind == TYPE_ARRAY) {
        size = sema_storage_size(type);
    } else if (type.kind == TYPE_SLICE) {
        size = 2 * sema_scalar_size(TYPE_POINTER);
    }
    checker->sema->nodes[id].builtin = BUILTIN_SIZE_OF;
    checker->sema->nodes[id].binding = size;
    return (SemaType){.kind = TYPE_U64};
}

static SemaType check_call(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    AstNodeId callee = ast->children[node.a];
    AstNodeId root = callee;
    while (ast->kinds[root] == AST_MEMBER) {
        root = ast->data[root].a;
    }
    if (ast->kinds[root] != AST_IDENTIFIER) {
        return fail(checker, callee, "expected a named function");
    }
    AstSpan name = node_span(ast, root);
    if (find_local(checker, name, 0)) {
        return fail(checker, root, "local variable is not callable");
    }
    uint32_t entry = 0;
    if (root != callee) {
        const AstQualified *qualified = ast_qualified(ast, id);
        if (!qualified || qualified->kind != BIND_FUNCTION) {
            return fail(checker, root, "expected an imported module");
        }
        entry = qualified->index + 1;
    } else {
        entry = *function_entry(checker, name);
        if (!entry) {
            const AstBinding *import = ast_imported(ast, checker->module, name);
            if (import && import->kind == BIND_FUNCTION) {
                entry = import->index + 1;
            }
        }
    }
    if (!entry && name_is(ast, name, "len")) {
        return check_collection_builtin(checker, id, BUILTIN_LEN);
    }
    if (!entry && name_is(ast, name, "data")) {
        return check_collection_builtin(checker, id, BUILTIN_DATA);
    }
    if (!entry && name_is(ast, name, "size_of")) {
        return check_size_of(checker, id);
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

static bool is_shift(AstKind kind)
{
    return kind == AST_SHIFT_LEFT || kind == AST_SHIFT_RIGHT ||
           kind == AST_ASSIGN_SHIFT_LEFT || kind == AST_ASSIGN_SHIFT_RIGHT;
}

// A numeric expression built from literals can take its type from its context.
static bool literal_expression(const Ast *ast, AstNodeId id, TypeKind hint,
                               unsigned depth, bool negative)
{
    if (depth == MAX_ANALYSIS_DEPTH) {
        return false;
    }
    AstKind kind = ast->kinds[id];
    AstNodeData node = ast->data[id];
    if (kind == AST_INTEGER) {
        uint64_t number;
        return hint == TYPE_INVALID ||
               (is_integer(hint) && integer_magnitude(ast, id, &number) &&
                integer_fits(hint, number, negative));
    }
    if (kind == AST_FLOAT) {
        return hint == TYPE_INVALID || is_float(hint);
    }
    if (kind == AST_POSITIVE || kind == AST_NEGATE || kind == AST_BIT_NOT) {
        if (kind == AST_NEGATE && is_integer(hint) && !is_signed(hint)) {
            return false;
        }
        return literal_expression(ast, node.a, hint, depth + 1, kind == AST_NEGATE);
    }
    if ((kind >= AST_ADD && kind <= AST_REMAINDER) ||
        (kind >= AST_BIT_AND && kind <= AST_SHIFT_RIGHT)) {
        return literal_expression(ast, node.a, hint, depth + 1, false) &&
               literal_expression(ast, node.b, hint, depth + 1, false);
    }
    return false;
}

static SemaType literal_hint(const Ast *ast, AstNodeId id, SemaType peer)
{
    return is_numeric(peer.kind) && literal_expression(ast, id, peer.kind, 0, false)
           ? peer : no_type;
}

static SemaType binary_literal_hint(const Ast *ast, AstNodeId id, SemaType peer, SemaType context)
{
    SemaType hint = literal_hint(ast, id, peer);
    if (hint.kind == TYPE_INVALID && can_promote(peer.kind, context.kind)) {
        return literal_hint(ast, id, context);
    }
    return hint;
}

typedef enum { NO_ADDRESS, READ_ONLY_ADDRESS, WRITABLE_ADDRESS } AddressAccess;

static AddressAccess address_access(const Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    while (ast->kinds[id] == AST_MEMBER || ast->kinds[id] == AST_INDEX) {
        AstNodeId base = ast->data[id].a;
        if (ast->kinds[id] == AST_INDEX) {
            SemaType type = checker->sema->nodes[base].type;
            if (type.kind == TYPE_POINTER) {
                return type.read_only ? READ_ONLY_ADDRESS : WRITABLE_ADDRESS;
            }
            if (type.kind == TYPE_SLICE) {
                return READ_ONLY_ADDRESS;
            }
        }
        id = base;
    }
    if (ast->kinds[id] == AST_DEREFERENCE) {
        SemaType pointer = checker->sema->nodes[ast->data[id].a].type;
        return pointer.read_only ? READ_ONLY_ADDRESS : WRITABLE_ADDRESS;
    }
    if (ast->kinds[id] == AST_STRING) {
        return READ_ONLY_ADDRESS;
    }
    if (ast->kinds[id] == AST_IDENTIFIER && !checker->sema->nodes[id].is_boolean_literal) {
        return WRITABLE_ADDRESS;
    }
    return NO_ADDRESS;
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
    AstKind target = ast->kinds[node.a];
    bool assignment = is_assignment(kind);
    bool shift = is_shift(kind);
    if (assignment && target != AST_INDEX && target != AST_MEMBER && target != AST_DEREFERENCE &&
        (target != AST_IDENTIFIER || boolean_name(ast, node_span(ast, node.a)))) {
        return fail(checker, node.a, "assignment requires a local variable, array element, or field");
    }
    SemaType context = !is_comparison(kind) && is_numeric(expected.kind) ? expected : no_type;
    SemaType left;
    SemaType right;
    if (assignment || shift) {
        left = check_value(checker, node.a, assignment ? no_type : context);
        if (assignment && !failed(checker) && address_access(checker, node.a) != WRITABLE_ADDRESS) {
            if (target == AST_DEREFERENCE ||
                (target == AST_INDEX && checker->sema->nodes[ast->data[node.a].a].type.kind == TYPE_POINTER)) {
                return fail(checker, node.a, "cannot write through a read-only pointer");
            }
            return fail(checker, node.a, target == AST_INDEX
                        ? "index assignment requires a local fixed array"
                        : "field assignment requires a local struct or fixed array element");
        }
        if (shift) {
            SemaType hint = literal_hint(ast, node.b, left);
            right = check_value(checker, node.b, hint);
        } else {
            right = value(checker, node.b, left);
        }
    } else {
        bool left_literal = literal_expression(ast, node.a, TYPE_INVALID, 0, false);
        bool right_literal = literal_expression(ast, node.b, TYPE_INVALID, 0, false);
        if (left_literal && !right_literal) {
            right = check_value(checker, node.b, context);
            left = check_value(checker, node.a, binary_literal_hint(ast, node.a, right, context));
        } else {
            left = check_value(checker, node.a, context);
            right = check_value(checker, node.b, right_literal && !left_literal
                                ? binary_literal_hint(ast, node.b, left, context) : context);
        }
    }
    if (failed(checker)) {
        return no_type;
    }
    if (kind == AST_ASSIGN) {
        return left;
    }
    if (kind == AST_EQUAL || kind == AST_NOT_EQUAL) {
        if ((left.kind == TYPE_BOOL && right.kind == TYPE_BOOL) ||
            (left.kind == TYPE_POINTER && right.kind == TYPE_POINTER && left.pointee == right.pointee)) {
            return bool_type;
        }
    }
    if (shift && !is_integer(right.kind)) {
        return fail(checker, node.b, "shift count must be an integer");
    }
    if (!assignment && !shift && is_numeric(left.kind) && is_numeric(right.kind)) {
        TypeKind common = common_numeric_type(left.kind, right.kind);
        if (common == TYPE_INVALID) {
            return fail(checker, id, "numeric operands have no common type");
        }
        promote(checker, node.a, common);
        promote(checker, node.b, common);
        left.kind = common;
    }
    bool valid = integer_operator(kind) ? is_integer(left.kind) && is_integer(right.kind)
                                         : is_numeric(left.kind) && is_numeric(right.kind);
    if (!valid) {
        return fail(checker, id, "operator requires numeric operands of a supported type");
    }
    return is_comparison(kind) ? bool_type : left;
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
    SemaType type = check_value(checker, child, is_numeric(expected.kind) ? expected : no_type);
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

static SemaType check_member(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    SemaType base = value(checker, node.a, no_type);
    if (base.kind != TYPE_STRUCT) {
        return fail(checker, id, "member access requires a struct value");
    }
    AstSpan name = {ast->positions[id], node.b};
    AstNodeRange fields = ast->structs[base.record].fields;
    for (uint32_t i = 0; i < fields.count; i++) {
        AstNodeId field = ast->children[fields.first + i];
        if (same_name(ast, name, node_span(ast, field))) {
            SemaNode info = checker->sema->nodes[field];
            checker->sema->nodes[id].binding = info.binding;
            return info.type;
        }
    }
    return fail(checker, id, "unknown struct field");
}

static SemaType check_cast(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    SemaType target = resolve_type(checker, node.b);
    // A literal in a cast can use the full integer range before conversion.
    SemaType source_hint = no_type;
    AstKind operand = ast->kinds[node.a];
    if (operand == AST_INTEGER ||
        (operand == AST_POSITIVE && ast->kinds[ast->data[node.a].a] == AST_INTEGER)) {
        source_hint.kind = TYPE_U64;
    } else if (operand == AST_NEGATE && ast->kinds[ast->data[node.a].a] == AST_INTEGER) {
        source_hint.kind = TYPE_I64;
    }
    SemaType source = value(checker, node.a, source_hint);
    if ((is_integer(source.kind) && is_integer(target.kind)) ||
        (source.kind == TYPE_POINTER && target.kind == TYPE_U64)) {
        return target;
    }
    if (source.kind == TYPE_POINTER && target.kind == TYPE_POINTER) {
        if (source.read_only && !target.read_only) {
            return fail(checker, id, "cast cannot remove read-only access");
        }
        TypeKind from = checker->sema->pointees[source.pointee].kind;
        TypeKind to = checker->sema->pointees[target.pointee].kind;
        if (source.pointee == target.pointee || from == TYPE_U8 || to == TYPE_U8) {
            return target;
        }
    }
    return fail(checker, id, "unsupported cast between these types");
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
        if ((!is_array_like(base.kind) && base.kind != TYPE_POINTER) || !is_integer(index.kind)) {
            return fail(checker, id, "indexing requires an array, slice, or pointer and an integer index");
        }
        return base.kind == TYPE_POINTER ? checker->sema->pointees[base.pointee] : element_type(base);
    }
    case AST_SLICE: {
        SemaType base = value(checker, node.a, no_type);
        AstNodeData bounds = ast->data[node.b];
        SemaType start = value(checker, bounds.a, no_type);
        SemaType end = value(checker, bounds.b, no_type);
        if (!is_array_like(base.kind) || !is_integer(start.kind) || !is_integer(end.kind)) {
            return fail(checker, id, "slicing requires an array or slice and integer bounds");
        }
        return (SemaType){.kind = TYPE_SLICE, .element = base.element, .record = base.record,
                          .record_size = base.record_size, .pointee = base.pointee,
                          .read_only = base.read_only};
    }
    case AST_MEMBER:
        return check_member(checker, id);
    case AST_CAST:
        return check_cast(checker, id);
    case AST_ADDRESS_OF: {
        SemaType operand = value(checker, node.a, no_type);
        if (failed(checker)) {
            return no_type;
        }
        AddressAccess access = address_access(checker, node.a);
        if (access == NO_ADDRESS) {
            return fail(checker, node.a, "address-of requires an addressable value");
        }
        return pointer_to(checker, id, operand, access == READ_ONLY_ADDRESS);
    }
    case AST_DEREFERENCE: {
        SemaType operand = value(checker, node.a, no_type);
        if (operand.kind != TYPE_POINTER) {
            return fail(checker, id, "dereference requires a pointer");
        }
        return checker->sema->pointees[operand.pointee];
    }
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
    SemaType type = variable.type ? resolve_type(checker, variable.type) : no_type;
    if (variable.initializer) {
        type = value(checker, variable.initializer, type);
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
    checker->module = function.module;
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
    free(sema->pointees);
    *sema = (Sema){0};
}

bool sema_check(const Ast *ast, Sema *out, SemaError *error)
{
    *out = (Sema){0};
    *error = (SemaError){0};
    if (ast->node_count == 0) {
        return true;
    }
    out->nodes = calloc(ast->node_count, sizeof(*out->nodes));
    Checker checker = {.ast = ast, .sema = out};
    if (ast->function_count) {
        out->functions = calloc(ast->function_count, sizeof(*out->functions));
    }
    if (ast->struct_count) {
        checker.struct_sizes = calloc(ast->struct_count, sizeof(*checker.struct_sizes));
    }
    if (!out->nodes || (ast->function_count && !out->functions) ||
        (ast->struct_count && !checker.struct_sizes)) {
        fail_at(&checker, 0, "could not allocate semantic analysis storage");
    }
    for (uint32_t i = 0; i < ast->struct_count && !failed(&checker); i++) {
        layout_struct(&checker, i);
    }
    for (uint32_t i = 0; i < ast->struct_count && !failed(&checker); i++) {
        resolve_pointer_fields(&checker, ast->structs[i]);
    }
    if (!failed(&checker) && !register_functions(&checker)) {
        fail_at(&checker, 0, "could not allocate semantic analysis storage");
    }
    for (uint32_t i = 0; i < ast->function_count && !failed(&checker); i++) {
        check_function(&checker, i);
    }
    free(checker.locals);
    free(checker.function_map);
    free(checker.struct_sizes);
    if (failed(&checker)) {
        *error = checker.error;
        sema_destroy(out);
        return false;
    }
    return true;
}
