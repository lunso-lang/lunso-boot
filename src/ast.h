#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef uint32_t AstNodeId;
#define AST_NODE_NONE ((AstNodeId)0)

typedef struct {
    uint32_t offset;
    uint32_t length;
} AstSpan;

typedef struct {
    uint32_t first;
    uint32_t count;
} AstNodeRange;

typedef uint8_t AstKind;
enum {
    AST_INVALID,

    AST_FUNCTION,   // a: functions index
    AST_STRUCT,     // a: structs index
    AST_IMPORT,     // a: imports index
    AST_IMPORT_ITEM, // a: name length, b: alias identifier or AST_NODE_NONE
    AST_FIELD,      // a: name length, b: type node
    AST_PARAMETER,  // a: name length, b: type node
    AST_TYPE_NAME,  // a: name length
    AST_QUALIFIED_TYPE, // a: first path component in children, b: component count
    AST_ARRAY_TYPE, // a: element type, b: length literal or AST_NODE_NONE for a slice
    AST_POINTER_TYPE, // a: pointee type

    AST_BLOCK,   // a: first item in children, b: item count
    AST_RETURN,  // a: value node or AST_NODE_NONE
    AST_IF,      // a: condition node, b: if_branches index
    AST_FOR,          // a: condition node or AST_NODE_NONE, b: body block
    AST_FOR_C_STYLE,  // a: for_headers index, b: body block
    AST_VAR,          // a: variables index, position: variable name

    AST_IDENTIFIER,  // a: name length
    AST_INTEGER,     // a: raw literal length in bytes
    AST_FLOAT,       // a: raw literal length in bytes
    AST_STRING,      // a: raw literal length in bytes, including quotes
    AST_CALL,        // a: first item in children, b: item count (callee, then arguments)
    AST_INDEX,       // a: base node, b: index expression node
    AST_SLICE,       // a: base node, b: range node
    AST_SLICE_RANGE, // a: start expression, b: end expression
    AST_MEMBER,      // a: base node, b: member name length
    AST_CAST,        // a: operand node, b: destination type node
    AST_ADDRESS_OF,  // a: addressable operand node
    AST_DEREFERENCE, // a: pointer operand node

    // Unary operators store the operand node in a.
    AST_POSITIVE,
    AST_NEGATE,
    AST_LOGICAL_NOT,
    AST_BIT_NOT,

    // Binary operators store the left and right operands in a and b.
    AST_ADD,
    AST_SUBTRACT,
    AST_MULTIPLY,
    AST_DIVIDE,
    AST_REMAINDER,
    AST_EQUAL,
    AST_NOT_EQUAL,
    AST_LESS,
    AST_LESS_EQUAL,
    AST_GREATER,
    AST_GREATER_EQUAL,
    AST_LOGICAL_AND,
    AST_LOGICAL_OR,
    AST_BIT_AND,
    AST_BIT_OR,
    AST_BIT_XOR,
    AST_SHIFT_LEFT,
    AST_SHIFT_RIGHT,
    AST_ASSIGN,
    AST_ASSIGN_ADD,
    AST_ASSIGN_SUBTRACT,
    AST_ASSIGN_MULTIPLY,
    AST_ASSIGN_DIVIDE,
    AST_ASSIGN_REMAINDER,
    AST_ASSIGN_BIT_AND,
    AST_ASSIGN_BIT_OR,
    AST_ASSIGN_BIT_XOR,
    AST_ASSIGN_SHIFT_LEFT,
    AST_ASSIGN_SHIFT_RIGHT,

    AST_KIND_COUNT,
};

typedef struct {
    uint32_t a;
    uint32_t b;
} AstNodeData;

_Static_assert(AST_KIND_COUNT <= 256, "AST node kinds must fit in one byte");
_Static_assert(sizeof(AstNodeData) == 8, "AST node data must occupy eight bytes");

typedef struct {
    AstSpan name;
    AstNodeRange parameters;  // Range in children.
    AstNodeId return_type;    // AST_NODE_NONE for a function without a result.
    AstNodeId body;           // AST_NODE_NONE for an external function.
    uint32_t module;
    bool public;
} AstFunction;

typedef struct {
    AstSpan name;
    AstNodeRange fields;  // Range in children.
    uint32_t module;
    bool public;
} AstStruct;

typedef enum { IMPORT_MODULE, IMPORT_ALL, IMPORT_SELECTED } AstImportKind;

typedef struct {
    AstNodeRange path;
    AstNodeRange items;
    AstSpan alias;
    AstImportKind kind;
} AstImport;

typedef enum { BIND_MODULE, BIND_FUNCTION, BIND_STRUCT } AstBindingKind;

typedef struct {
    char *name;
    uint32_t index;
    uint32_t offset;
    AstBindingKind kind;
} AstBinding;

typedef struct {
    AstBinding *bindings;
    uint32_t count;
} AstModuleScope;

typedef struct {
    AstNodeId node;
    uint32_t index;
    AstBindingKind kind;
} AstQualified;

typedef struct {
    AstNodeId then_branch;
    AstNodeId else_branch;  // AST_NODE_NONE if absent.
} AstIfBranches;

typedef struct {
    // An omitted clause is AST_NODE_NONE.
    AstNodeId initializer;
    AstNodeId condition;
    AstNodeId step;
} AstForHeader;

typedef struct {
    uint32_t name_length;
    AstNodeId type;
    AstNodeId initializer;  // AST_NODE_NONE if absent.
} AstVariable;

typedef struct {
    const unsigned char *source;  // Borrowed for the life of the AST.
    uint32_t source_length;

    // Parallel arrays indexed by AstNodeId. A populated table reserves slot zero.
    // node_count is the number of slots, including the reserved slot when present.
    AstKind *kinds;
    AstNodeData *data;
    // Byte offset of the leading token, operator, or name (the field name for AST_MEMBER).
    uint32_t *positions;
    uint32_t node_count;

    // These tables use zero-based indices and counts of actual entries.
    AstNodeId *children;
    uint32_t child_count;
    AstFunction *functions;
    uint32_t function_count;
    AstStruct *structs;
    uint32_t struct_count;
    AstImport *imports;
    uint32_t import_count;
    AstIfBranches *if_branches;
    uint32_t if_branch_count;
    AstForHeader *for_headers;
    uint32_t for_header_count;
    AstVariable *variables;
    uint32_t variable_count;

    AstNodeRange declarations;  // Top-level nodes in children, in source order.
    AstModuleScope *modules;
    uint32_t module_count;
    AstQualified *qualified;    // Sorted by node ID.
    uint32_t qualified_count;
} Ast;

// Releases parser-owned AST storage. The caller retains ownership of the source buffer.
void ast_destroy(Ast *ast);
const AstBinding *ast_imported(const Ast *ast, uint32_t module, AstSpan name);
const AstQualified *ast_qualified(const Ast *ast, AstNodeId node);
