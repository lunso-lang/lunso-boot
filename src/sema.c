#include "sema.h"
#include "literal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AstSpan name;
    SemaType type;
    uint32_t slot;
    uint32_t const_index;
    bool is_const;
} Local;

typedef struct {
    AstSpan label;
    bool broke;
} LoopInfo;

typedef struct {
    const Ast *ast;
    Sema *sema;
    SemaError error;
    uint32_t *function_map;
    size_t function_capacity;
    uint32_t *struct_sizes;
    uint32_t *struct_alignments;
    uint8_t *struct_state;
    SemaType *const_types;
    uint8_t *const_state;
    uint32_t pointee_capacity;
    uint32_t element_capacity;
    Local *locals;
    size_t local_count;
    size_t local_capacity;
    size_t scope_start;
    uint32_t next_slot;
    SemaType result;
    uint32_t module;
    unsigned depth;
    LoopInfo loops[256];
    uint32_t loop_depth;
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
    if (type.element_size) {
        return type.element_size;
    }
    if (type.element == TYPE_STRUCT) {
        return type.record_size;
    }
    if (type.element == TYPE_SLICE) {
        return 2 * sema_scalar_size(TYPE_POINTER);
    }
    return sema_scalar_size(type.element);
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

static bool is_enum(SemaType type)
{
    return is_integer(type.kind) && type.record != 0;
}

static bool is_plain_integer(SemaType type)
{
    return is_integer(type.kind) && !is_enum(type);
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
           a.element_type == b.element_type && a.record == b.record &&
           a.pointee == b.pointee && a.read_only == b.read_only;
}

static bool is_array_like(TypeKind type)
{
    return type == TYPE_ARRAY || type == TYPE_SLICE;
}

static SemaType element_type(const Checker *checker, SemaType collection)
{
    return checker->sema->elements[collection.element_type];
}

static uint32_t type_byte_size(SemaType type)
{
    if (type.kind == TYPE_STRUCT || type.kind == TYPE_ARRAY) {
        return sema_storage_size(type);
    }
    if (type.kind == TYPE_SLICE) {
        return 2 * sema_scalar_size(TYPE_POINTER);
    }
    return sema_scalar_size(type.kind);
}

static SemaType make_collection(Checker *checker, AstNodeId id, TypeKind kind,
                                uint32_t length, SemaType element)
{
    if (failed(checker)) {
        return no_type;
    }
    Sema *sema = checker->sema;
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < sema->element_count; i++) {
        if (same_type(sema->elements[i], element)) {
            index = i;
            break;
        }
    }
    if (index == UINT32_MAX) {
        if (sema->element_count == checker->element_capacity) {
            uint32_t capacity = checker->element_capacity;
            if (capacity > UINT32_MAX / 2) {
                return fail(checker, id, "could not allocate collection type storage");
            }
            capacity = capacity ? capacity * 2 : 16;
            size_t item_size = sizeof(*sema->elements);
            if (capacity > SIZE_MAX / item_size) {
                return fail(checker, id, "could not allocate collection type storage");
            }
            SemaType *elements = realloc(sema->elements, capacity * item_size);
            if (!elements) {
                return fail(checker, id, "could not allocate collection type storage");
            }
            sema->elements = elements;
            checker->element_capacity = capacity;
        }
        index = sema->element_count++;
        sema->elements[index] = element;
    }
    return (SemaType){
        .kind = kind,
        .length = length,
        .element = element.kind,
        .element_type = index,
        .element_size = type_byte_size(element),
        .record = element.record,
        .record_size = element.record_size,
        .pointee = element.pointee,
        .read_only = element.read_only,
    };
}

static SemaType pointer_to(Checker *checker, AstNodeId id, SemaType pointee, bool read_only)
{
    if (failed(checker)) {
        return no_type;
    }
    if (pointee.kind == TYPE_ARRAY) {
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

static uint32_t find_enum(const Ast *ast, uint32_t module, AstSpan name)
{
    for (uint32_t i = 0; i < ast->enum_count; i++) {
        if (ast->enums[i].module == module && same_name(ast, name, ast->enums[i].name)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static SemaType enum_type(const Checker *checker, uint32_t index)
{
    AstNodeId backing = checker->ast->enums[index].backing;
    return (SemaType){.kind = checker->sema->nodes[backing].type.kind, .record = index + 1};
}

static SemaType resolve_type(Checker *checker, AstNodeId id);

static uint64_t align_bytes(uint64_t size, uint32_t alignment)
{
    return (size + alignment - 1) & ~((uint64_t)alignment - 1);
}

enum { STRUCT_UNVISITED = 0, STRUCT_VISITING = 1, STRUCT_DONE = 2 };

static const Local *find_local(const Checker *checker, AstSpan name, size_t first);
static void check_const(Checker *checker, uint32_t index);

static uint32_t find_const(const Ast *ast, uint32_t module, AstSpan name)
{
    for (uint32_t i = 0; i < ast->const_count; i++) {
        if (!ast->consts[i].local && ast->consts[i].module == module &&
            same_name(ast, ast->consts[i].name, name)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool resolve_array_length(Checker *checker, AstNodeId count_node,
                                 uint32_t elem_size, uint32_t *out_length)
{
    uint32_t limit = elem_size ? (INT32_MAX - 7u) / elem_size : INT32_MAX - 7u;
    if (checker->ast->kinds[count_node] == AST_IDENTIFIER) {
        AstSpan name = node_span(checker->ast, count_node);
        const Local *local = find_local(checker, name, 0);
        uint32_t idx = UINT32_MAX;
        if (local && local->is_const) {
            idx = local->const_index;
        } else if (!local) {
            const AstBinding *binding = ast_imported(checker->ast, checker->module, name);
            idx = binding && binding->kind == BIND_CONST
                  ? binding->index
                  : find_const(checker->ast, checker->module, name);
        }
        if (idx != UINT32_MAX && checker->const_state[idx] == 2 &&
            is_integer(checker->const_types[idx].kind) && !is_enum(checker->const_types[idx])) {
            uint64_t val = checker->sema->const_values[idx];
            if (is_signed(checker->const_types[idx].kind) && (int64_t)val < 0) {
                fail(checker, count_node, "array length cannot be negative");
                return false;
            }
            if (val > limit) {
                fail(checker, count_node, "array is too large for local storage");
                return false;
            }
            *out_length = (uint32_t)val;
            return true;
        }
    }
    AstSpan count = node_span(checker->ast, count_node);
    uint64_t value = 0;
    if (!literal_integer_value(checker->ast->source + count.offset, count.length, &value) ||
        value > limit) {
        fail(checker, count_node, "array is too large for local storage");
        return false;
    }
    *out_length = (uint32_t)value;
    return true;
}

static void layout_struct(Checker *checker, uint32_t index)
{
    if (checker->struct_state[index] == STRUCT_DONE) {
        return;
    }
    const Ast *ast = checker->ast;
    AstStruct record = ast->structs[index];
    if (checker->struct_state[index] == STRUCT_VISITING) {
        fail_at(checker, record.name.offset, "recursive struct definition");
        return;
    }
    checker->struct_state[index] = STRUCT_VISITING;

    uint32_t saved_module = checker->module;
    checker->module = record.module;
    if (scalar_type(ast, record.name) != TYPE_INVALID ||
        find_struct(ast, record.module, record.name) != index) {
        fail_at(checker, record.name.offset, "duplicate or reserved struct name");
        checker->module = saved_module;
        return;
    }
    if (!record.fields.count) {
        fail_at(checker, record.name.offset, "struct must have at least one field");
        checker->module = saved_module;
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
                checker->module = saved_module;
                return;
            }
        }
        AstNodeId annotation = ast->data[field].b;
        AstKind annotation_kind = ast->kinds[annotation];
        if (annotation_kind != AST_TYPE_NAME && annotation_kind != AST_POINTER_TYPE &&
            annotation_kind != AST_QUALIFIED_TYPE && annotation_kind != AST_ARRAY_TYPE) {
            fail(checker, annotation, "struct fields must be scalar, struct, or fixed array");
            checker->module = saved_module;
            return;
        }
        SemaType type = {.kind = TYPE_INVALID};
        uint32_t width = 0;
        uint32_t field_align = 1;
        if (annotation_kind == AST_POINTER_TYPE) {
            type = (SemaType){.kind = TYPE_POINTER};
            width = sema_scalar_size(TYPE_POINTER);
            field_align = width;
        } else if (annotation_kind == AST_ARRAY_TYPE) {
            AstNodeId leaf = annotation;
            while (ast->kinds[leaf] == AST_ARRAY_TYPE && ast->data[leaf].b != AST_NODE_NONE) {
                leaf = ast->data[leaf].a;
            }
            bool deferred_pointer = ast->kinds[leaf] == AST_POINTER_TYPE ||
                                    (ast->kinds[leaf] == AST_ARRAY_TYPE &&
                                     ast->kinds[ast->data[leaf].a] == AST_POINTER_TYPE);
            if (!deferred_pointer) {
                if (ast->kinds[leaf] == AST_TYPE_NAME || ast->kinds[leaf] == AST_QUALIFIED_TYPE) {
                    SemaType leaf_type = resolve_type(checker, leaf);
                    if (failed(checker)) {
                        checker->module = saved_module;
                        return;
                    }
                    if (leaf_type.kind == TYPE_STRUCT) {
                        layout_struct(checker, leaf_type.record);
                        if (failed(checker)) {
                            checker->module = saved_module;
                            return;
                        }
                    }
                }
                type = resolve_type(checker, annotation);
                if (type.kind == TYPE_INVALID || failed(checker)) {
                    checker->module = saved_module;
                    return;
                }
                width = type_byte_size(type);
                if (ast->kinds[leaf] == AST_ARRAY_TYPE) {
                    field_align = sema_scalar_size(TYPE_POINTER);
                } else {
                    SemaType leaf_type = resolve_type(checker, leaf);
                    field_align = leaf_type.kind == TYPE_STRUCT
                                  ? checker->struct_alignments[leaf_type.record]
                                  : sema_scalar_size(leaf_type.kind);
                }
            } else {
                uint64_t total_count = 1;
                AstNodeId cur = annotation;
                while (ast->kinds[cur] == AST_ARRAY_TYPE && ast->data[cur].b != AST_NODE_NONE) {
                    uint32_t len = 0;
                    uint32_t ptr_size = sema_scalar_size(TYPE_POINTER);
                    if (!resolve_array_length(checker, ast->data[cur].b, ptr_size, &len)) {
                        checker->module = saved_module;
                        return;
                    }
                    if (total_count > (INT32_MAX - 7u) / (len ? len : 1)) {
                        fail(checker, cur, "array is too large for local storage");
                        checker->module = saved_module;
                        return;
                    }
                    total_count *= len;
                    cur = ast->data[cur].a;
                }
                uint32_t leaf_size = ast->kinds[cur] == AST_ARRAY_TYPE
                                     ? 2 * sema_scalar_size(TYPE_POINTER)
                                     : sema_scalar_size(TYPE_POINTER);
                if (total_count > (INT32_MAX - 7u) / leaf_size) {
                    fail(checker, annotation, "array is too large for local storage");
                    checker->module = saved_module;
                    return;
                }
                type = (SemaType){.kind = ast->data[annotation].b ? TYPE_ARRAY : TYPE_SLICE};
                width = (uint32_t)(total_count * leaf_size);
                field_align = sema_scalar_size(TYPE_POINTER);
            }
        } else {
            type = resolve_type(checker, annotation);
            if (type.kind == TYPE_INVALID || failed(checker)) {
                checker->module = saved_module;
                return;
            }
            if (type.kind == TYPE_STRUCT) {
                layout_struct(checker, type.record);
                if (failed(checker)) {
                    checker->module = saved_module;
                    return;
                }
                type.record_size = checker->struct_sizes[type.record];
                width = type.record_size;
                field_align = checker->struct_alignments[type.record];
            } else {
                width = sema_scalar_size(type.kind);
                if (!width) {
                    fail(checker, annotation, "struct fields must be scalar, struct, or fixed array");
                    checker->module = saved_module;
                    return;
                }
                field_align = width;
            }
        }
        size = align_bytes(size, field_align);
        if (size + width > INT32_MAX - 7u) {
            fail(checker, field, "struct is too large for local storage");
            checker->module = saved_module;
            return;
        }
        checker->sema->nodes[field].type = type;
        checker->sema->nodes[field].binding = (uint32_t)size;
        size += width;
        if (field_align > alignment) {
            alignment = field_align;
        }
    }
    checker->struct_alignments[index] = alignment;
    checker->struct_sizes[index] = (uint32_t)align_bytes(size, alignment);
    checker->struct_state[index] = STRUCT_DONE;
    checker->module = saved_module;
}

static SemaType resolve_type(Checker *checker, AstNodeId id)
{
    if (id == AST_NODE_NONE) {
        return void_type;
    }
    AstKind ast_kind = checker->ast->kinds[id];
    if (ast_kind == AST_QUALIFIED_TYPE) {
        const AstQualified *qualified = ast_qualified(checker->ast, id);
        if (qualified && qualified->kind == BIND_ENUM) {
            return enum_type(checker, qualified->index);
        }
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
        if (element.kind == TYPE_VOID || element.kind == TYPE_INVALID) {
            return fail(checker, data.a, "collection elements cannot have void type");
        }
        SemaType collection = make_collection(checker, id, TYPE_SLICE, 0, element);
        if (!data.b || failed(checker)) {
            return collection;
        }

        uint32_t elem_size = sema_element_size(collection);
        uint32_t length = 0;
        if (elem_size && !resolve_array_length(checker, data.b, elem_size, &length)) {
            return no_type;
        }
        return make_collection(checker, id, TYPE_ARRAY, length, element);
    }
    AstSpan name = node_span(checker->ast, id);
    const AstBinding *import = ast_imported(checker->ast, checker->module, name);
    if (import && import->kind == BIND_STRUCT) {
        return (SemaType){.kind = TYPE_STRUCT, .record = import->index,
                          .record_size = checker->struct_sizes[import->index]};
    }
    if (import && import->kind == BIND_ENUM) {
        return enum_type(checker, import->index);
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
    uint32_t enumeration = find_enum(checker->ast, checker->module, name);
    if (enumeration != UINT32_MAX) {
        return enum_type(checker, enumeration);
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
        AstNodeId leaf = annotation;
        while (ast->kinds[leaf] == AST_ARRAY_TYPE) {
            leaf = ast->data[leaf].a;
        }
        if (ast->kinds[leaf] == AST_POINTER_TYPE) {
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
    checker->locals[checker->local_count++] = (Local){.name = name, .type = type, .slot = slot};
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
        type.element_type == expected.element_type) {
        checker->sema->nodes[id].borrow_array = true;
        return expected;
    }
    if (type.kind == TYPE_POINTER && expected.kind == TYPE_POINTER &&
        type.pointee == expected.pointee && expected.read_only) {
        return expected;
    }
    if (!is_enum(type) && !is_enum(expected) && can_promote(type.kind, expected.kind)) {
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
    return literal_integer_value(ast->source + span.offset, span.length, number);
}

static uint64_t integer_limit(TypeKind type, bool negative)
{
    uint32_t bits = 8 * sema_scalar_size(type);
    uint64_t limit = UINT64_MAX >> (64 - bits);
    if (is_signed(type)) {
        limit = (limit >> 1) + negative;
    }
    return limit;
}

static bool integer_fits(TypeKind type, uint64_t number, bool negative)
{
    return (!negative || is_signed(type)) && number <= integer_limit(type, negative);
}

static void check_enum(Checker *checker, uint32_t index, uint32_t *next_value)
{
    const Ast *ast = checker->ast;
    AstEnum enumeration = ast->enums[index];
    if (scalar_type(ast, enumeration.name) != TYPE_INVALID || boolean_name(ast, enumeration.name) ||
        find_enum(ast, enumeration.module, enumeration.name) != index ||
        find_struct(ast, enumeration.module, enumeration.name) != UINT32_MAX) {
        fail_at(checker, enumeration.name.offset, "duplicate or reserved enum name");
        return;
    }
    TypeKind backing = scalar_type(ast, node_span(ast, enumeration.backing));
    if (!is_integer(backing)) {
        fail(checker, enumeration.backing, "enum backing type must be an integer");
        return;
    }
    if (!enumeration.cases.count) {
        fail_at(checker, enumeration.name.offset, "enum must have at least one member");
        return;
    }
    checker->sema->nodes[enumeration.backing].type.kind = backing;
    const AstNodeId *cases = ast->children + enumeration.cases.first;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < enumeration.cases.count; i++) {
        AstNodeId member = cases[i];
        for (uint32_t j = 0; j < i; j++) {
            if (same_name(ast, node_span(ast, member), node_span(ast, cases[j]))) {
                fail(checker, member, "duplicate enum member name");
                return;
            }
        }
        AstNodeId literal = ast->data[member].b;
        if (literal) {
            bool negative = ast->kinds[literal] == AST_NEGATE;
            AstNodeId magnitude = negative ? ast->data[literal].a : literal;
            uint64_t number;
            if (!integer_magnitude(ast, magnitude, &number) || !integer_fits(backing, number, negative)) {
                fail(checker, literal, "enum value is out of range for its backing type");
                return;
            }
            bits = negative ? UINT64_C(0) - number : number;
        } else if (i != 0) {
            if (bits == integer_limit(backing, false)) {
                fail(checker, member, "enum value is out of range for its backing type");
                return;
            }
            bits++;
        }
        uint32_t slot = (*next_value)++;
        checker->sema->nodes[member].binding = slot;
        checker->sema->enum_values[slot] = bits;
    }
}

static SemaType integer_literal(Checker *checker, AstNodeId id, SemaType expected, bool negative)
{
    if (is_enum(expected)) {
        expected = no_type;
    }
    if (negative && is_integer(expected.kind) && !is_signed(expected.kind)) {
        return fail(checker, id, "unary '-' requires a signed number");
    }
    uint64_t number;
    if (!integer_magnitude(checker->ast, id, &number)) {
        AstSpan span = node_span(checker->ast, id);
        const char *msg = checker->ast->source[span.offset] == '\''
                          ? "invalid character literal"
                          : "integer literal is out of range";
        return fail(checker, id, msg);
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
    return make_collection(checker, id, TYPE_ARRAY, length, (SemaType){.kind = TYPE_U8});
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
    if (local) {
        if (local->is_const) {
            checker->sema->nodes[id].is_const = true;
            checker->sema->nodes[id].binding = local->const_index;
        } else {
            checker->sema->nodes[id].binding = local->slot;
        }
        return local->type;
    }
    const AstBinding *binding = ast_imported(checker->ast, checker->module, name);
    uint32_t const_index = binding && binding->kind == BIND_CONST
                           ? binding->index
                           : find_const(checker->ast, checker->module, name);
    if (const_index != UINT32_MAX) {
        checker->sema->nodes[id].is_const = true;
        checker->sema->nodes[id].binding = const_index;
        return checker->const_types[const_index];
    }
    return fail(checker, id, "unknown variable");
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
                  : pointer_to(checker, id, element_type(checker, operand), true);
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
    return !is_enum(peer) && is_numeric(peer.kind) && literal_expression(ast, id, peer.kind, 0, false)
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
    if (checker->sema->nodes[id].is_enum_case || checker->sema->nodes[id].is_const) {
        return NO_ADDRESS;
    }
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
    if (checker->sema->nodes[id].is_const) {
        return checker->sema->const_strings[checker->sema->nodes[id].binding] != AST_NODE_NONE
               ? READ_ONLY_ADDRESS : NO_ADDRESS;
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
    SemaType context = !is_comparison(kind) && !is_enum(expected) && is_numeric(expected.kind)
                       ? expected : no_type;
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
    if (is_enum(left) || is_enum(right)) {
        if (is_comparison(kind) && same_type(left, right)) {
            return bool_type;
        }
        return fail(checker, id, "enum operands must be compared within the same enum or cast to integers");
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
    if (is_enum(type) || (kind == AST_BIT_NOT ? !is_integer(type.kind) : !is_numeric(type.kind))) {
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
    AstSpan name = {ast->positions[id], node.b};
    const AstQualified *qualified = ast_qualified(ast, id);
    if (qualified && qualified->kind == BIND_CONST) {
        if (qualified->inaccessible) {
            return fail(checker, id, "cannot access a private constant");
        }
        checker->sema->nodes[id].is_const = true;
        checker->sema->nodes[id].binding = qualified->index;
        return checker->const_types[qualified->index];
    }
    AstNodeId root = node.a;
    if (qualified && qualified->kind == BIND_ENUM) {
        while (ast->kinds[root] == AST_MEMBER) {
            root = ast->data[root].a;
        }
    }
    uint32_t enumeration = UINT32_MAX;
    if (ast->kinds[root] == AST_IDENTIFIER && !find_local(checker, node_span(ast, root), 0)) {
        if (qualified && qualified->kind == BIND_ENUM) {
            if (qualified->inaccessible) {
                return fail(checker, id, "cannot access a private enum");
            }
            enumeration = qualified->index;
        } else if (root == node.a) {
            AstSpan type_name = node_span(ast, root);
            const AstBinding *binding = ast_imported(ast, checker->module, type_name);
            enumeration = binding && binding->kind == BIND_ENUM ? binding->index
                                                               : find_enum(ast, checker->module, type_name);
        }
    }
    if (enumeration != UINT32_MAX) {
        AstNodeRange cases = ast->enums[enumeration].cases;
        for (uint32_t i = 0; i < cases.count; i++) {
            AstNodeId member = ast->children[cases.first + i];
            if (same_name(ast, name, node_span(ast, member))) {
                checker->sema->nodes[id].binding = checker->sema->nodes[member].binding;
                checker->sema->nodes[id].is_enum_case = true;
                return enum_type(checker, enumeration);
            }
        }
        return fail(checker, id, "unknown enum member");
    }
    SemaType base = value(checker, node.a, no_type);
    if (base.kind != TYPE_STRUCT) {
        return fail(checker, id, "member access requires a struct value");
    }
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
    if ((is_integer(source.kind) && is_integer(target.kind) &&
         (!is_enum(source) || !is_enum(target) || same_type(source, target))) ||
        (source.kind == TYPE_POINTER && target.kind == TYPE_U64)) {
        return target;
    }
    if (source.kind == TYPE_POINTER && target.kind == TYPE_POINTER) {
        if (source.read_only && !target.read_only) {
            return fail(checker, id, "cast cannot remove read-only access");
        }
        SemaType from = checker->sema->pointees[source.pointee];
        SemaType to = checker->sema->pointees[target.pointee];
        if (source.pointee == target.pointee || (from.kind == TYPE_U8 && !is_enum(from)) ||
            (to.kind == TYPE_U8 && !is_enum(to))) {
            return target;
        }
    }
    return fail(checker, id, "unsupported cast between these types");
}

static uint32_t resolve_struct_expr(Checker *checker, AstNodeId base)
{
    const Ast *ast = checker->ast;
    AstKind kind = ast->kinds[base];
    if (kind == AST_IDENTIFIER) {
        AstSpan name = node_span(ast, base);
        const AstBinding *binding = ast_imported(ast, checker->module, name);
        if (binding && binding->kind == BIND_STRUCT) {
            return binding->index;
        }
        uint32_t record = find_struct(ast, checker->module, name);
        if (record != UINT32_MAX) {
            return record;
        }
    } else if (kind == AST_MEMBER) {
        const AstQualified *qualified = ast_qualified(ast, base);
        if (qualified && qualified->kind == BIND_STRUCT) {
            if (qualified->inaccessible) {
                fail(checker, base, "cannot access a private struct");
                return UINT32_MAX;
            }
            return qualified->index;
        }
    }
    fail(checker, base, "expected a struct type");
    return UINT32_MAX;
}

static SemaType check_struct_literal(Checker *checker, AstNodeId id)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    uint32_t record = resolve_struct_expr(checker, ast->children[node.a]);
    if (record == UINT32_MAX) {
        return no_type;
    }

    AstNodeRange fields = ast->structs[record].fields;
    for (uint32_t i = 1; i < node.b && !failed(checker); i++) {
        AstNodeId init = ast->children[node.a + i];
        AstSpan name = {ast->positions[init], ast->data[init].a};

        for (uint32_t j = 1; j < i; j++) {
            AstNodeId prev = ast->children[node.a + j];
            AstSpan prev_name = {ast->positions[prev], ast->data[prev].a};
            if (same_name(ast, name, prev_name)) {
                return fail(checker, init, "duplicate struct field");
            }
        }

        AstNodeId matched = AST_NODE_NONE;
        for (uint32_t f = 0; f < fields.count; f++) {
            AstNodeId field = ast->children[fields.first + f];
            if (same_name(ast, name, node_span(ast, field))) {
                matched = field;
                break;
            }
        }
        if (!matched) {
            return fail(checker, init, "unknown struct field");
        }
        SemaNode info = checker->sema->nodes[matched];
        checker->sema->nodes[init].binding = info.binding;
        checker->sema->nodes[init].type = info.type;
        value(checker, ast->data[init].b, info.type);
    }

    return (SemaType){.kind = TYPE_STRUCT, .record = record,
                      .record_size = checker->struct_sizes[record]};
}

static SemaType check_array_literal(Checker *checker, AstNodeId id, SemaType expected)
{
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    uint32_t count = node.b;
    SemaType elem_expected = no_type;
    if (expected.kind == TYPE_ARRAY) {
        if (count != expected.length) {
            return fail(checker, id, "array initializer length mismatch");
        }
        elem_expected = element_type(checker, expected);
    } else if (expected.kind == TYPE_SLICE) {
        elem_expected = element_type(checker, expected);
    }

    SemaType elem_type;
    if (elem_expected.kind != TYPE_INVALID) {
        for (uint32_t i = 0; i < count && !failed(checker); i++) {
            value(checker, ast->children[node.a + i], elem_expected);
        }
        elem_type = elem_expected;
    } else {
        elem_type = value(checker, ast->children[node.a], no_type);
        if (failed(checker)) {
            return no_type;
        }
        if (elem_type.kind == TYPE_VOID) {
            return fail(checker, ast->children[node.a],
                        "collection elements cannot have void type");
        }
        for (uint32_t i = 1; i < count && !failed(checker); i++) {
            value(checker, ast->children[node.a + i], elem_type);
        }
    }

    SemaType result = make_collection(checker, id, TYPE_ARRAY, count, elem_type);
    uint32_t elem_size = sema_element_size(result);
    if (elem_size && count > (INT32_MAX - 7u) / elem_size) {
        return fail(checker, id, "array is too large for local storage");
    }
    return result;
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
    case AST_ARRAY_LITERAL:
        return check_array_literal(checker, id, expected);
    case AST_STRUCT_LITERAL:
        return check_struct_literal(checker, id);
    case AST_CALL:
        return check_call(checker, id);
    case AST_INDEX: {
        SemaType base = value(checker, node.a, no_type);
        SemaType index = value(checker, node.b, no_type);
        if ((!is_array_like(base.kind) && base.kind != TYPE_POINTER) || !is_plain_integer(index)) {
            return fail(checker, id, "indexing requires an array, slice, or pointer and an integer index");
        }
        return base.kind == TYPE_POINTER ? checker->sema->pointees[base.pointee]
                                         : element_type(checker, base);
    }
    case AST_SLICE: {
        SemaType base = value(checker, node.a, no_type);
        AstNodeData bounds = ast->data[node.b];
        SemaType start = value(checker, bounds.a, no_type);
        SemaType end = value(checker, bounds.b, no_type);
        if ((!is_array_like(base.kind) && base.kind != TYPE_POINTER) ||
            !is_plain_integer(start) || !is_plain_integer(end)) {
            return fail(checker, id, "slicing requires an array, slice, or pointer and integer bounds");
        }
        if (failed(checker)) {
            return no_type;
        }
        SemaType element = base.kind == TYPE_POINTER ? checker->sema->pointees[base.pointee]
                                                     : element_type(checker, base);
        return make_collection(checker, id, TYPE_SLICE, 0, element);
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
    if (header.label.length > 0) {
        for (uint32_t i = 0; i < checker->loop_depth; i++) {
            if (checker->loops[i].label.length > 0 &&
                same_name(ast, header.label, checker->loops[i].label)) {
                fail_at(checker, header.label.offset, "duplicate loop label");
                close_scope(checker, scope);
                return false;
            }
        }
    }
    if (checker->loop_depth >= MAX_ANALYSIS_DEPTH) {
        fail(checker, id, "loop nesting is too deep");
        close_scope(checker, scope);
        return false;
    }
    checker->loops[checker->loop_depth++] = (LoopInfo){.label = header.label, .broke = false};
    check_statement(checker, node.b);
    bool broke = checker->loops[--checker->loop_depth].broke;
    close_scope(checker, scope);
    return header.condition != AST_NODE_NONE || broke;
}

static uint32_t resolve_loop_target(Checker *checker, AstNodeId id, bool is_break)
{
    if (checker->loop_depth == 0) {
        fail(checker, id, is_break ? "break must be inside a loop" : "continue must be inside a loop");
        return 0;
    }
    AstNodeId label_node = checker->ast->data[id].a;
    if (label_node == AST_NODE_NONE) {
        return checker->loop_depth - 1;
    }
    AstSpan label = node_span(checker->ast, label_node);
    for (uint32_t i = checker->loop_depth; i > 0; i--) {
        LoopInfo *info = &checker->loops[i - 1];
        if (info->label.length > 0 && same_name(checker->ast, label, info->label)) {
            return i - 1;
        }
    }
    fail(checker, label_node, "unknown loop label");
    return 0;
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
    case AST_BREAK: {
        uint32_t target = resolve_loop_target(checker, id, true);
        if (!failed(checker)) {
            checker->sema->nodes[id].binding = target;
            checker->loops[target].broke = true;
        }
        return false;
    }
    case AST_CONTINUE: {
        uint32_t target = resolve_loop_target(checker, id, false);
        if (!failed(checker)) {
            checker->sema->nodes[id].binding = target;
        }
        return false;
    }
    case AST_CONST: {
        uint32_t idx = node.a;
        AstConst constant = ast->consts[idx];
        if (find_local(checker, constant.name, checker->scope_start)) {
            fail(checker, id, "duplicate variable name");
            return true;
        }
        check_const(checker, idx);
        if (!failed(checker)) {
            if (checker->local_count == checker->local_capacity) {
                size_t cap = checker->local_capacity ? checker->local_capacity * 2 : 16;
                Local *grown = realloc(checker->locals, cap * sizeof(*grown));
                if (!grown) {
                    fail(checker, id, "could not allocate local storage");
                    return true;
                }
                checker->locals = grown;
                checker->local_capacity = cap;
            }
            checker->locals[checker->local_count++] = (Local){
                .name = constant.name,
                .type = checker->const_types[idx],
                .const_index = idx,
                .is_const = true,
            };
        }
        return true;
    }
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
    checker->loop_depth = 0;
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
    free(sema->elements);
    free(sema->enum_values);
    free(sema->const_values);
    free(sema->const_strings);
    *sema = (Sema){0};
}

static void check_const(Checker *checker, uint32_t index);

static uint64_t normalize_int_bits(TypeKind kind, uint64_t bits)
{
    uint32_t width = 8 * sema_scalar_size(kind);
    if (!width) {
        return bits;
    }
    uint64_t mask = UINT64_MAX >> (64 - width);
    bits &= mask;
    if (is_signed(kind) && width < 64 && (bits & (UINT64_C(1) << (width - 1)))) {
        bits |= ~mask;
    }
    return bits;
}

static double read_float_bits(TypeKind kind, uint64_t bits)
{
    if (kind == TYPE_F32) {
        uint32_t raw = (uint32_t)bits;
        float f;
        memcpy(&f, &raw, sizeof(f));
        return (double)f;
    }
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static uint64_t write_float_bits(TypeKind kind, double value)
{
    if (kind == TYPE_F32) {
        float f = (float)value;
        uint32_t raw = 0;
        memcpy(&raw, &f, sizeof(raw));
        return raw;
    }
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return raw;
}

static SemaType eval_const(Checker *checker, AstNodeId id, SemaType expected,
                           uint64_t *out_bits, AstNodeId *out_string)
{
    if (!enter(checker, id)) {
        return no_type;
    }
    const Ast *ast = checker->ast;
    AstNodeData node = ast->data[id];
    SemaType type = no_type;
    uint64_t bits = 0;
    AstNodeId str_node = AST_NODE_NONE;

    switch (ast->kinds[id]) {
    case AST_INTEGER: {
        type = integer_literal(checker, id, expected, false);
        if (!failed(checker)) {
            integer_magnitude(ast, id, &bits);
            bits = normalize_int_bits(type.kind, bits);
        }
        break;
    }
    case AST_FLOAT: {
        type = float_literal(checker, id, expected);
        if (!failed(checker)) {
            AstSpan span = node_span(ast, id);
            char *buf = malloc(span.length + 1);
            if (!buf) {
                fail(checker, id, "could not allocate float buffer");
                break;
            }
            memcpy(buf, ast->source + span.offset, span.length);
            buf[span.length] = '\0';
            double d = strtod(buf, NULL);
            free(buf);
            bits = write_float_bits(type.kind, d);
        }
        break;
    }
    case AST_STRING: {
        type = string_literal(checker, id);
        if (!failed(checker)) {
            checker->sema->nodes[id].type = type;
            str_node = id;
        }
        break;
    }
    case AST_IDENTIFIER: {
        AstSpan name = node_span(ast, id);
        if (boolean_name(ast, name)) {
            type = bool_type;
            bits = name_is(ast, name, "true");
            break;
        }
        const Local *local = find_local(checker, name, 0);
        uint32_t idx = UINT32_MAX;
        if (local) {
            if (!local->is_const) {
                fail(checker, id, "constant expression required");
                break;
            }
            idx = local->const_index;
        } else {
            const AstBinding *binding = ast_imported(ast, checker->module, name);
            idx = binding && binding->kind == BIND_CONST
                  ? binding->index : find_const(ast, checker->module, name);
        }
        if (idx == UINT32_MAX) {
            fail(checker, id, "constant expression required");
            break;
        }
        check_const(checker, idx);
        if (!failed(checker)) {
            type = checker->const_types[idx];
            bits = checker->sema->const_values[idx];
            str_node = checker->sema->const_strings[idx];
        }
        break;
    }
    case AST_MEMBER: {
        const AstQualified *qualified = ast_qualified(ast, id);
        if (qualified && qualified->kind == BIND_CONST) {
            if (qualified->inaccessible) {
                fail(checker, id, "cannot access a private constant");
                break;
            }
            check_const(checker, qualified->index);
            if (!failed(checker)) {
                type = checker->const_types[qualified->index];
                bits = checker->sema->const_values[qualified->index];
                str_node = checker->sema->const_strings[qualified->index];
            }
            break;
        }
        type = check_member(checker, id);
        if (!failed(checker) && checker->sema->nodes[id].is_enum_case) {
            bits = checker->sema->enum_values[checker->sema->nodes[id].binding];
        } else if (!failed(checker)) {
            fail(checker, id, "constant expression required");
        }
        break;
    }
    case AST_POSITIVE:
    case AST_NEGATE:
    case AST_BIT_NOT:
    case AST_LOGICAL_NOT: {
        AstKind op = ast->kinds[id];
        if (op == AST_NEGATE && ast->kinds[node.a] == AST_INTEGER) {
            type = integer_literal(checker, node.a, expected, true);
            if (!failed(checker)) {
                uint64_t mag = 0;
                integer_magnitude(ast, node.a, &mag);
                bits = normalize_int_bits(type.kind, UINT64_C(0) - mag);
            }
            break;
        }
        SemaType sub_expected = op == AST_LOGICAL_NOT ? bool_type : expected;
        AstNodeId unused_str = AST_NODE_NONE;
        type = eval_const(checker, node.a, sub_expected, &bits, &unused_str);
        if (failed(checker)) {
            break;
        }
        if (op == AST_LOGICAL_NOT) {
            if (type.kind != TYPE_BOOL) {
                fail(checker, id, "logical '!' requires a bool operand");
            } else {
                bits = !bits;
            }
        } else if (op == AST_BIT_NOT) {
            if (!is_plain_integer(type)) {
                fail(checker, id, "bitwise '~' requires an integer operand");
            } else {
                bits = normalize_int_bits(type.kind, ~bits);
            }
        } else {
            if (is_enum(type) || !is_numeric(type.kind) || (op == AST_NEGATE && !is_signed(type.kind))) {
                fail(checker, id, op == AST_NEGATE ? "unary '-' requires a signed number"
                                                   : "unary '+' requires a numeric operand");
            } else if (op == AST_NEGATE) {
                bits = is_float(type.kind)
                       ? write_float_bits(type.kind, -read_float_bits(type.kind, bits))
                       : normalize_int_bits(type.kind, UINT64_C(0) - bits);
            }
        }
        break;
    }
    case AST_CAST: {
        SemaType target = resolve_type(checker, node.b);
        if (failed(checker)) {
            break;
        }
        if (!is_integer(target.kind) && !is_float(target.kind)) {
            fail(checker, node.b, "constant cast requires a numeric or enum type");
            break;
        }
        uint64_t src_bits = 0;
        AstNodeId unused_str = AST_NODE_NONE;
        SemaType src = eval_const(checker, node.a, no_type, &src_bits, &unused_str);
        if (failed(checker)) {
            break;
        }
        if (is_integer(target.kind) && is_integer(src.kind)) {
            bits = normalize_int_bits(target.kind, src_bits);
        } else if (is_float(target.kind) && is_float(src.kind)) {
            bits = write_float_bits(target.kind, read_float_bits(src.kind, src_bits));
        } else {
            fail(checker, id, "unsupported constant cast");
            break;
        }
        type = target;
        break;
    }
    default: {
        AstKind op = ast->kinds[id];
        if (op < AST_ADD || op > AST_SHIFT_RIGHT) {
            fail(checker, id, "constant expression required");
            break;
        }
        uint64_t lbits = 0, rbits = 0;
        AstNodeId lstr = AST_NODE_NONE, rstr = AST_NODE_NONE;
        SemaType ltype = no_type, rtype = no_type;
        if (op == AST_LOGICAL_AND || op == AST_LOGICAL_OR) {
            ltype = eval_const(checker, node.a, bool_type, &lbits, &lstr);
            if (!failed(checker)) {
                rtype = eval_const(checker, node.b, bool_type, &rbits, &rstr);
            }
            if (failed(checker)) {
                break;
            }
            if (ltype.kind != TYPE_BOOL || rtype.kind != TYPE_BOOL) {
                fail(checker, id, "logical operators require bool operands");
                break;
            }
            type = bool_type;
            bits = op == AST_LOGICAL_AND ? (lbits && rbits) : (lbits || rbits);
            break;
        }
        ltype = eval_const(checker, node.a, expected, &lbits, &lstr);
        if (!failed(checker)) {
            rtype = eval_const(checker, node.b, ltype, &rbits, &rstr);
        }
        if (failed(checker)) {
            break;
        }
        if (is_shift(op)) {
            if (!is_plain_integer(ltype) || !is_plain_integer(rtype)) {
                fail(checker, id, "shift requires integer operands");
                break;
            }
            unsigned width = 8 * sema_scalar_size(ltype.kind);
            unsigned shift = (unsigned)(rbits & (width - 1));
            type = ltype;
            if (op == AST_SHIFT_LEFT) {
                bits = normalize_int_bits(type.kind, lbits << shift);
            } else if (is_signed(type.kind)) {
                bits = normalize_int_bits(type.kind, (uint64_t)((int64_t)lbits >> shift));
            } else {
                bits = normalize_int_bits(type.kind, lbits >> shift);
            }
            break;
        }
        if (op == AST_EQUAL || op == AST_NOT_EQUAL) {
            if (ltype.kind == TYPE_BOOL && rtype.kind == TYPE_BOOL) {
                type = bool_type;
                bits = op == AST_EQUAL ? (lbits == rbits) : (lbits != rbits);
                break;
            }
            if (is_enum(ltype) || is_enum(rtype)) {
                if (!same_type(ltype, rtype)) {
                    fail(checker, id, "type mismatch");
                    break;
                }
                type = bool_type;
                bits = op == AST_EQUAL ? (lbits == rbits) : (lbits != rbits);
                break;
            }
        }
        TypeKind common = common_numeric_type(ltype.kind, rtype.kind);
        if (common == TYPE_INVALID || is_enum(ltype) || is_enum(rtype)) {
            fail(checker, id, "invalid constant operands");
            break;
        }
        if (integer_operator(op) && !is_integer(common)) {
            fail(checker, id, "integer operator requires integer operands");
            break;
        }
        if (is_float(common)) {
            double ld = read_float_bits(ltype.kind, lbits);
            double rd = read_float_bits(rtype.kind, rbits);
            if (is_comparison(op)) {
                type = bool_type;
                switch (op) {
                case AST_EQUAL: bits = ld == rd; break;
                case AST_NOT_EQUAL: bits = ld != rd; break;
                case AST_LESS: bits = ld < rd; break;
                case AST_LESS_EQUAL: bits = ld <= rd; break;
                case AST_GREATER: bits = ld > rd; break;
                case AST_GREATER_EQUAL: bits = ld >= rd; break;
                default: break;
                }
            } else {
                type = (SemaType){.kind = common};
                double res = 0.0;
                switch (op) {
                case AST_ADD: res = ld + rd; break;
                case AST_SUBTRACT: res = ld - rd; break;
                case AST_MULTIPLY: res = ld * rd; break;
                case AST_DIVIDE: res = ld / rd; break;
                default: break;
                }
                bits = write_float_bits(common, res);
            }
            break;
        }
        uint64_t lu = normalize_int_bits(common, lbits);
        uint64_t ru = normalize_int_bits(common, rbits);
        if (is_comparison(op)) {
            type = bool_type;
            if (is_signed(common)) {
                int64_t ls = (int64_t)lu, rs = (int64_t)ru;
                switch (op) {
                case AST_EQUAL: bits = ls == rs; break;
                case AST_NOT_EQUAL: bits = ls != rs; break;
                case AST_LESS: bits = ls < rs; break;
                case AST_LESS_EQUAL: bits = ls <= rs; break;
                case AST_GREATER: bits = ls > rs; break;
                case AST_GREATER_EQUAL: bits = ls >= rs; break;
                default: break;
                }
            } else {
                switch (op) {
                case AST_EQUAL: bits = lu == ru; break;
                case AST_NOT_EQUAL: bits = lu != ru; break;
                case AST_LESS: bits = lu < ru; break;
                case AST_LESS_EQUAL: bits = lu <= ru; break;
                case AST_GREATER: bits = lu > ru; break;
                case AST_GREATER_EQUAL: bits = lu >= ru; break;
                default: break;
                }
            }
        } else {
            type = (SemaType){.kind = common};
            if ((op == AST_DIVIDE || op == AST_REMAINDER) && ru == 0) {
                fail(checker, id, "division by zero in constant expression");
                break;
            }
            if (op == AST_DIVIDE && is_signed(common)) {
                bits = (uint64_t)((int64_t)lu / (int64_t)ru);
            } else if (op == AST_REMAINDER && is_signed(common)) {
                bits = (uint64_t)((int64_t)lu % (int64_t)ru);
            } else {
                switch (op) {
                case AST_ADD: bits = lu + ru; break;
                case AST_SUBTRACT: bits = lu - ru; break;
                case AST_MULTIPLY: bits = lu * ru; break;
                case AST_DIVIDE: bits = lu / ru; break;
                case AST_REMAINDER: bits = lu % ru; break;
                case AST_BIT_AND: bits = lu & ru; break;
                case AST_BIT_OR: bits = lu | ru; break;
                case AST_BIT_XOR: bits = lu ^ ru; break;
                default: break;
                }
            }
            bits = normalize_int_bits(common, bits);
        }
        break;
    }
    }

    checker->depth--;
    *out_bits = bits;
    *out_string = str_node;
    return type;
}

static void check_const(Checker *checker, uint32_t index)
{
    if (checker->const_state[index] == STRUCT_DONE) {
        return;
    }
    const Ast *ast = checker->ast;
    AstConst constant = ast->consts[index];
    if (checker->const_state[index] == STRUCT_VISITING) {
        fail_at(checker, constant.name.offset, "cyclic constant definition");
        return;
    }
    checker->const_state[index] = STRUCT_VISITING;

    uint32_t saved_module = checker->module;
    checker->module = constant.module;
    if (!constant.local &&
        (scalar_type(ast, constant.name) != TYPE_INVALID ||
         boolean_name(ast, constant.name) ||
         ast_imported(ast, constant.module, constant.name) ||
         find_const(ast, constant.module, constant.name) != index)) {
        fail_at(checker, constant.name.offset, "duplicate or reserved constant name");
        checker->module = saved_module;
        return;
    }

    SemaType expected = no_type;
    if (constant.type != AST_NODE_NONE) {
        expected = resolve_type(checker, constant.type);
        if (failed(checker)) {
            checker->module = saved_module;
            return;
        }
        bool is_byte_string_type = (expected.kind == TYPE_ARRAY || expected.kind == TYPE_SLICE) &&
                                   expected.element == TYPE_U8 && !expected.read_only;
        if (expected.kind != TYPE_BOOL && !is_numeric(expected.kind) && !is_byte_string_type) {
            fail(checker, constant.type, "constant must have a scalar, enum, or string type");
            checker->module = saved_module;
            return;
        }
    }

    uint64_t bits = 0;
    AstNodeId str_node = AST_NODE_NONE;
    SemaType actual = eval_const(checker, constant.value, expected, &bits, &str_node);
    if (failed(checker)) {
        checker->module = saved_module;
        return;
    }
    if (expected.kind != TYPE_INVALID && !same_type(actual, expected)) {
        if (str_node != AST_NODE_NONE && expected.kind == TYPE_SLICE &&
            actual.kind == TYPE_ARRAY && actual.element == TYPE_U8) {
            actual = expected;
        } else if (str_node == AST_NODE_NONE && !is_enum(actual) && !is_enum(expected) &&
                   can_promote(actual.kind, expected.kind)) {
            if (is_float(expected.kind)) {
                bits = write_float_bits(expected.kind, read_float_bits(actual.kind, bits));
            } else {
                bits = normalize_int_bits(expected.kind, bits);
            }
            actual = expected;
        } else {
            fail(checker, constant.value, "type mismatch");
            checker->module = saved_module;
            return;
        }
    }

    checker->const_types[index] = actual;
    checker->sema->const_values[index] = bits;
    checker->sema->const_strings[index] = str_node;
    checker->const_state[index] = STRUCT_DONE;
    checker->module = saved_module;
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
        checker.struct_alignments = calloc(ast->struct_count, sizeof(*checker.struct_alignments));
        checker.struct_state = calloc(ast->struct_count, sizeof(*checker.struct_state));
    }
    size_t enum_value_count = 0;
    for (uint32_t i = 0; i < ast->enum_count; i++) {
        enum_value_count += ast->enums[i].cases.count;
    }
    if (enum_value_count) {
        out->enum_values = calloc(enum_value_count, sizeof(*out->enum_values));
    }
    if (ast->const_count) {
        out->const_values = calloc(ast->const_count, sizeof(*out->const_values));
        out->const_strings = calloc(ast->const_count, sizeof(*out->const_strings));
        checker.const_types = calloc(ast->const_count, sizeof(*checker.const_types));
        checker.const_state = calloc(ast->const_count, sizeof(*checker.const_state));
    }
    if (!out->nodes || (ast->function_count && !out->functions) ||
        (ast->struct_count && (!checker.struct_sizes || !checker.struct_alignments || !checker.struct_state)) ||
        (enum_value_count && !out->enum_values) ||
        (ast->const_count && (!out->const_values || !out->const_strings ||
                              !checker.const_types || !checker.const_state))) {
        fail_at(&checker, 0, "could not allocate semantic analysis storage");
    }
    uint32_t next_value = 0;
    for (uint32_t i = 0; i < ast->enum_count && !failed(&checker); i++) {
        check_enum(&checker, i, &next_value);
    }
    for (uint32_t i = 0; i < ast->const_count && !failed(&checker); i++) {
        if (!ast->consts[i].local) {
            check_const(&checker, i);
        }
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
    free(checker.struct_alignments);
    free(checker.struct_state);
    free(checker.const_types);
    free(checker.const_state);
    if (failed(&checker)) {
        *error = checker.error;
        sema_destroy(out);
        return false;
    }
    return true;
}
