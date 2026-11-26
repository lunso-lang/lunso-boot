#include "qbe.h"

#include "literal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char base;
    const char *abi;
    const char *extend;
} QbeType;

static const QbeType types[] = {
    [TYPE_BOOL] = {'w', "ub", "extub"},
    [TYPE_I8] = {'w', "sb", "extsb"}, [TYPE_U8] = {'w', "ub", "extub"},
    [TYPE_I16] = {'w', "sh", "extsh"}, [TYPE_U16] = {'w', "uh", "extuh"},
    [TYPE_I32] = {'w', "w", NULL}, [TYPE_U32] = {'w', "w", NULL},
    [TYPE_I64] = {'l', "l", NULL}, [TYPE_U64] = {'l', "l", NULL},
    [TYPE_F32] = {'s', "s", NULL}, [TYPE_F64] = {'d', "d", NULL},
    [TYPE_STRUCT] = {'l', NULL, NULL}, [TYPE_ARRAY] = {'l', NULL, NULL},
    [TYPE_SLICE] = {'l', NULL, NULL},
    [TYPE_POINTER] = {'l', "l", NULL},
};

static bool is_signed(TypeKind type)
{
    return type >= TYPE_I8 && type <= TYPE_I64;
}

static bool is_unsigned(TypeKind type)
{
    return type >= TYPE_U8 && type <= TYPE_U64;
}

static bool entry_symbol(const IrModule *module, IrFunction function)
{
    return !function.external && !function.specialization && !function.module && function.name.length == 4 &&
           memcmp(module->source + function.name.offset, "main", 4) == 0;
}

static void write_symbol(FILE *out, const IrModule *module, uint32_t index)
{
    IrFunction function = module->functions[index];
    AstSpan name = function.name;
    fputc('$', out);
    if (function.specialization ||
        (module->module_count > 1 && !function.external && !entry_symbol(module, function))) {
        fprintf(out, ".lunso.fn.%" PRIu32 ".", index);
    }
    fwrite(module->source + name.offset, 1, name.length, out);
}

static void write_integer_bits(FILE *out, uint64_t bits)
{
    if (bits > INT64_MAX) {
        fprintf(out, "-%" PRIu64, UINT64_C(0) - bits);
    } else {
        fprintf(out, "%" PRIu64, bits);
    }
}

static void write_integer(FILE *out, const unsigned char *text, uint32_t length, bool negative)
{
    uint64_t bits = 0;
    (void)literal_integer_value(text, length, &bits);
    if (negative) {
        bits = 0 - bits;
    }
    write_integer_bits(out, bits);
}

static void write_number(FILE *out, const IrModule *module, IrInstruction inst)
{
    QbeType type = types[inst.type.kind];
    fprintf(out, "\t%%v%" PRIu32 " =%c copy ", inst.result, type.base);
    const unsigned char *text = module->source + inst.a;
    if (type.base == 's' || type.base == 'd') {
        fprintf(out, "%c_", type.base);
        fwrite(text, 1, inst.b, out);
    } else {
        write_integer(out, text, inst.b, inst.c != 0);
    }
    fputc('\n', out);
}

static bool write_data(FILE *out, const IrModule *module)
{
    for (uint32_t i = 0; i < module->instruction_count; i++) {
        IrInstruction inst = module->instructions[i];
        if (inst.op != IR_BYTES) {
            continue;
        }
        fprintf(out, "data $.lunso.bytes.%" PRIu32 " = align 1 {", i);
        if (inst.type.length == 0) {
            fputs(" z 0", out);
        } else {
            fputs(" b", out);
            const unsigned char *literal = module->source + inst.a;
            for (uint32_t cursor = 1; cursor < inst.b - 1;) {
                unsigned char byte;
                if (!literal_next_byte(literal, inst.b - 1, &cursor, &byte)) {
                    return false;
                }
                fprintf(out, " %u", (unsigned)byte);
            }
        }
        fputs(" }\n", out);
    }
    return true;
}

static bool uses_sret(TypeKind kind)
{
    return kind == TYPE_STRUCT || kind == TYPE_ARRAY || kind == TYPE_SLICE;
}

static void write_load(FILE *out, TypeKind kind, uint32_t result, const char *name, uint32_t address);

static void write_call(FILE *out, const IrModule *module, IrInstruction inst)
{
    IrFunction callee = module->functions[inst.a];
    QbeType result = types[callee.result.kind];
    bool sret = uses_sret(callee.result.kind);
    fputc('\t', out);
    if (inst.result && !sret) {
        fprintf(out, "%%%s%" PRIu32 " =%s ", result.extend ? "raw" : "v", inst.result,
                result.abi);
    }
    fputs("call ", out);
    write_symbol(out, module, inst.a);
    fputc('(', out);
    if (sret) {
        fprintf(out, "l %%%s%" PRIu32,
                callee.result.kind == TYPE_SLICE ? "retbuf" : "v", inst.result);
    }
    for (uint32_t i = 0; i < inst.c; i++) {
        if (sret || i != 0) {
            fputs(", ", out);
        }
        SemaType type = module->parameters[callee.first_parameter + i];
        uint32_t arg = module->arguments[inst.b + i];
        if (type.kind == TYPE_SLICE) {
            fprintf(out, "l %%v%" PRIu32 ", l %%len%" PRIu32, arg, arg);
        } else {
            fprintf(out, "%s %%v%" PRIu32, types[type.kind].abi, arg);
        }
    }
    fputs(")\n", out);
    if (callee.result.kind == TYPE_SLICE) {
        write_load(out, TYPE_SLICE, inst.result, "retbuf", inst.result);
    } else if (!sret && result.extend) {
        fprintf(out, "\t%%v%" PRIu32 " =w %s %%raw%" PRIu32 "\n",
                inst.result, result.extend, inst.result);
    }
}

static void write_length(FILE *out, SemaType type, uint32_t value)
{
    if (type.kind == TYPE_SLICE) {
        fprintf(out, "%%len%" PRIu32, value);
    } else {
        fprintf(out, "%" PRIu32, type.length);
    }
}

static void write_index_address(FILE *out, IrInstruction inst, uint32_t size)
{
    if (size != 1) {
        fprintf(out, "\t%%off%" PRIu32 " =l mul %%v%" PRIu32 ", %" PRIu32 "\n",
                inst.result, inst.b, size);
    }
    fprintf(out, "\t%%v%" PRIu32 " =l add %%v%" PRIu32 ", %%%s%" PRIu32 "\n",
            inst.result, inst.a, size == 1 ? "v" : "off", size == 1 ? inst.b : inst.result);
}

static void write_access(FILE *out, IrInstruction inst)
{
    bool slice = inst.op == IR_SLICE || inst.op == IR_POINTER_SLICE;
    uint32_t bound = slice ? inst.c : inst.b;
    uint32_t element_size = sema_element_size(inst.type);
    // Unsigned comparisons also reject negative indices widened from signed types.
    fprintf(out, "\t%%in%" PRIu32 " =w %s %%v%" PRIu32 ", ",
            inst.result, slice ? "culel" : "cultl", bound);
    if (inst.op == IR_POINTER_SLICE) {
        fprintf(out, "%" PRIu64, (uint64_t)INT64_MAX / (element_size ? element_size : 1));
    } else {
        write_length(out, inst.type, inst.a);
    }
    fputc('\n', out);
    if (slice) {
        fprintf(out, "\t%%order%" PRIu32 " =w culel %%v%" PRIu32 ", %%v%" PRIu32 "\n",
                inst.result, inst.b, inst.c);
        fprintf(out, "\t%%in%" PRIu32 " =w and %%in%" PRIu32 ", %%order%" PRIu32 "\n",
                inst.result, inst.result, inst.result);
    }
    fprintf(out, "\tjnz %%in%" PRIu32 ", @array_ok%" PRIu32 ", @bounds_fail\n",
            inst.result, inst.result);
    fprintf(out, "@array_ok%" PRIu32 "\n", inst.result);
    write_index_address(out, inst, element_size);
    if (slice) {
        fprintf(out, "\t%%len%" PRIu32 " =l sub %%v%" PRIu32 ", %%v%" PRIu32 "\n",
                inst.result, inst.c, inst.b);
    }
}

static void write_copy_bytes(FILE *out, const char *dst_reg, uint32_t dst_id, bool has_dst_id,
                             uint32_t src_id, bool has_src, uint32_t bytes, uint32_t at)
{
    if (!bytes) {
        return;
    }
    fprintf(out, "\t%%i%" PRIu32 " =l copy 0\n\tjmp @init%" PRIu32 "\n@init%" PRIu32 "\n",
            at, at, at);
    if (has_dst_id) {
        fprintf(out, "\t%%dst%" PRIu32 " =l add %%%s%" PRIu32 ", %%i%" PRIu32 "\n",
                at, dst_reg, dst_id, at);
    } else {
        fprintf(out, "\t%%dst%" PRIu32 " =l add %%%s, %%i%" PRIu32 "\n", at, dst_reg, at);
    }
    if (has_src) {
        fprintf(out, "\t%%src%" PRIu32 " =l add %%v%" PRIu32 ", %%i%" PRIu32 "\n", at, src_id, at);
        fprintf(out, "\t%%byte%" PRIu32 " =w loadub %%src%" PRIu32 "\n", at, at);
        fprintf(out, "\tstoreb %%byte%" PRIu32 ", %%dst%" PRIu32 "\n", at, at);
    } else {
        fprintf(out, "\tstoreb 0, %%dst%" PRIu32 "\n", at);
    }
    fprintf(out, "\t%%i%" PRIu32 " =l add %%i%" PRIu32 ", 1\n", at, at);
    fprintf(out, "\t%%more%" PRIu32 " =w cultl %%i%" PRIu32 ", %" PRIu32 "\n", at, at, bytes);
    fprintf(out, "\tjnz %%more%" PRIu32 ", @init%" PRIu32 ", @init_end%" PRIu32 "\n@init_end%" PRIu32 "\n",
            at, at, at, at);
}

static void write_storage_init(FILE *out, IrInstruction inst, uint32_t at)
{
    write_copy_bytes(out, "v", inst.a, true, inst.b, inst.b != 0, sema_storage_size(inst.type), at);
}

static char memory_suffix(TypeKind type)
{
    uint32_t size = sema_scalar_size(type);
    return size == 1 ? 'b' : size == 2 ? 'h' : types[type].base;
}

static void write_load(FILE *out, TypeKind kind, uint32_t result, const char *name, uint32_t address)
{
    if (kind == TYPE_SLICE) {
        fprintf(out, "\t%%v%" PRIu32 " =l loadl %%%s%" PRIu32 "\n", result, name, address);
        fprintf(out, "\t%%slen_addr%" PRIu32 " =l add %%%s%" PRIu32 ", 8\n", result, name, address);
        fprintf(out, "\t%%len%" PRIu32 " =l loadl %%slen_addr%" PRIu32 "\n", result, result);
        return;
    }
    QbeType type = types[kind];
    const char *sign = type.extend ? (is_signed(kind) ? "s" : "u") : "";
    fprintf(out, "\t%%v%" PRIu32 " =%c load%s%c %%%s%" PRIu32 "\n",
            result, type.base, sign, memory_suffix(kind), name, address);
}

static void write_store(FILE *out, TypeKind kind, const char *value_name, const char *len_name,
                        uint32_t value, const char *address_name, uint32_t address,
                        const char *tmp_prefix, uint32_t tmp_id)
{
    if (kind == TYPE_SLICE) {
        fprintf(out, "\tstorel %%%s%" PRIu32 ", %%%s%" PRIu32 "\n",
                value_name, value, address_name, address);
        fprintf(out, "\t%%s%s%" PRIu32 " =l add %%%s%" PRIu32 ", 8\n",
                tmp_prefix, tmp_id, address_name, address);
        fprintf(out, "\tstorel %%%s%" PRIu32 ", %%s%s%" PRIu32 "\n",
                len_name, value, tmp_prefix, tmp_id);
        return;
    }
    fprintf(out, "\tstore%c %%%s%" PRIu32 ", %%%s%" PRIu32 "\n",
            memory_suffix(kind), value_name, value, address_name, address);
}

static void write_comparison(FILE *out, IrInstruction inst)
{
    static const char *const relation[] = {
        [IR_EQUAL] = "eq", [IR_NOT_EQUAL] = "ne", [IR_LESS] = "lt",
        [IR_LESS_EQUAL] = "le", [IR_GREATER] = "gt", [IR_GREATER_EQUAL] = "ge",
    };
    const char *sign = "";
    if (inst.op != IR_EQUAL && inst.op != IR_NOT_EQUAL) {
        if (is_signed(inst.type.kind)) {
            sign = "s";
        } else if (is_unsigned(inst.type.kind)) {
            sign = "u";
        }
    }
    fprintf(out, "\t%%v%" PRIu32 " =w c%s%s%c %%v%" PRIu32 ", %%v%" PRIu32 "\n",
            inst.result, sign, relation[inst.op], types[inst.type.kind].base, inst.a, inst.b);
}

static const char *operation(IrInstruction inst)
{
    static const char *const names[] = {
        [IR_NEGATE] = "neg", [IR_LOGICAL_NOT] = "ceqw", [IR_BIT_NOT] = "xor",
        [IR_ADD] = "add", [IR_SUBTRACT] = "sub", [IR_MULTIPLY] = "mul",
        [IR_DIVIDE] = "div", [IR_REMAINDER] = "rem",
        [IR_BIT_AND] = "and", [IR_BIT_OR] = "or", [IR_BIT_XOR] = "xor",
        [IR_SHIFT_LEFT] = "shl", [IR_SHIFT_RIGHT] = "shr",
    };
    if (inst.op == IR_DIVIDE && is_unsigned(inst.type.kind)) {
        return "udiv";
    }
    if (inst.op == IR_REMAINDER && is_unsigned(inst.type.kind)) {
        return "urem";
    }
    if (inst.op == IR_SHIFT_RIGHT && is_signed(inst.type.kind)) {
        return "sar";
    }
    return names[inst.op];
}

static void write_operation(FILE *out, IrInstruction inst)
{
    QbeType type = types[inst.type.kind];
    const char *rhs = "v";
    uint32_t right = inst.b;
    if (type.extend && (inst.op == IR_SHIFT_LEFT || inst.op == IR_SHIFT_RIGHT)) {
        unsigned mask = inst.type.kind == TYPE_I8 || inst.type.kind == TYPE_U8 ? 7 : 15;
        fprintf(out, "\t%%shift%" PRIu32 " =w and %%v%" PRIu32 ", %u\n",
                inst.result, inst.b, mask);
        rhs = "shift";
        right = inst.result;
    }
    const char *normalize = inst.op == IR_LOGICAL_NOT ? NULL : type.extend;
    fprintf(out, "\t%%%s%" PRIu32 " =%c %s %%v%" PRIu32,
            normalize ? "raw" : "v", inst.result, type.base, operation(inst), inst.a);
    if (inst.op == IR_BIT_NOT) {
        fputs(", -1", out);
    } else if (inst.op == IR_LOGICAL_NOT) {
        fputs(", 0", out);
    } else if (right) {
        fprintf(out, ", %%%s%" PRIu32, rhs, right);
    }
    fputc('\n', out);
    if (normalize) {
        fprintf(out, "\t%%v%" PRIu32 " =w %s %%raw%" PRIu32 "\n",
                inst.result, normalize, inst.result);
    }
}

static void write_cast(FILE *out, uint32_t result, uint32_t value,
                       TypeKind source, TypeKind destination)
{
    QbeType target = types[destination];
    QbeType src = types[source];
    if (destination == TYPE_BOOL) {
        if (source == TYPE_BOOL) {
            fprintf(out, "\t%%v%" PRIu32 " =w copy %%v%" PRIu32 "\n", result, value);
        } else if (src.base == 's' || src.base == 'd') {
            fprintf(out, "\t%%v%" PRIu32 " =w cne%c %%v%" PRIu32 ", %c_0\n",
                    result, src.base, value, src.base);
        } else {
            fprintf(out, "\t%%v%" PRIu32 " =w cne%c %%v%" PRIu32 ", 0\n",
                    result, src.base, value);
        }
        return;
    }
    if (source == TYPE_BOOL && (target.base == 's' || target.base == 'd')) {
        fprintf(out, "\t%%v%" PRIu32 " =%c uwtof %%v%" PRIu32 "\n", result, target.base, value);
        return;
    }
    if (source == TYPE_F32 && destination == TYPE_F64) {
        fprintf(out, "\t%%v%" PRIu32 " =d exts %%v%" PRIu32 "\n", result, value);
        return;
    }
    if (source == TYPE_F64 && destination == TYPE_F32) {
        fprintf(out, "\t%%v%" PRIu32 " =s truncd %%v%" PRIu32 "\n", result, value);
        return;
    }
    if ((src.base == 's' || src.base == 'd') && (target.base == 'w' || target.base == 'l')) {
        const char *conv = src.base == 's'
                           ? (is_signed(destination) ? "stosi" : "stoui")
                           : (is_signed(destination) ? "dtosi" : "dtoui");
        if (target.extend) {
            fprintf(out, "\t%%raw%" PRIu32 " =w %s %%v%" PRIu32 "\n", result, conv, value);
            fprintf(out, "\t%%v%" PRIu32 " =w %s %%raw%" PRIu32 "\n",
                    result, target.extend, result);
        } else {
            fprintf(out, "\t%%v%" PRIu32 " =%c %s %%v%" PRIu32 "\n",
                    result, target.base, conv, value);
        }
        return;
    }
    if ((src.base == 'w' || src.base == 'l') && (target.base == 's' || target.base == 'd')) {
        const char *conv = src.base == 'w'
                           ? (is_signed(source) ? "swtof" : "uwtof")
                           : (is_signed(source) ? "sltof" : "ultof");
        fprintf(out, "\t%%v%" PRIu32 " =%c %s %%v%" PRIu32 "\n",
                result, target.base, conv, value);
        return;
    }

    const char *op = target.extend ? target.extend : "copy";
    if (target.base == 'l' && src.base == 'w') {
        op = is_signed(source) ? "extsw" : "extuw";
    }
    fprintf(out, "\t%%v%" PRIu32 " =%c %s %%v%" PRIu32 "\n", result, target.base, op, value);
}

static void write_instruction(FILE *out, const IrModule *module, uint32_t index,
                              const TypeKind *addressed, SemaType result)
{
    IrInstruction inst = module->instructions[index];
    switch (inst.op) {
    case IR_LABEL:
        fprintf(out, "@b%" PRIu32 "\n", inst.a);
        break;
    case IR_NUMBER:
        write_number(out, module, inst);
        break;
    case IR_BOOL:
    case IR_IMMEDIATE: {
        uint64_t bits = ((uint64_t)inst.b << 32) | inst.a;
        fprintf(out, "\t%%v%" PRIu32 " =%c copy ", inst.result, types[inst.type.kind].base);
        if (inst.type.kind == TYPE_F32) {
            uint32_t raw = (uint32_t)bits;
            float value;
            memcpy(&value, &raw, sizeof(value));
            fprintf(out, "s_%.9g", (double)value);
        } else if (inst.type.kind == TYPE_F64) {
            double value;
            memcpy(&value, &bits, sizeof(value));
            fprintf(out, "d_%.17g", value);
        } else {
            write_integer_bits(out, bits);
        }
        fputc('\n', out);
        if (inst.type.kind == TYPE_SLICE) {
            fprintf(out, "\t%%len%" PRIu32 " =l copy 0\n", inst.result);
        }
        break;
    }
    case IR_BYTES:
        fprintf(out, "\t%%v%" PRIu32 " =l copy $.lunso.bytes.%" PRIu32 "\n", inst.result, index);
        break;
    case IR_LOAD:
        if (addressed && addressed[inst.a] != TYPE_INVALID) {
            write_load(out, inst.type.kind, inst.result, "local", inst.a);
            break;
        }
        fprintf(out, "\t%%v%" PRIu32 " =%c copy %%s%" PRIu32 "\n",
                inst.result, types[inst.type.kind].base, inst.a);
        if (inst.type.kind == TYPE_SLICE) {
            fprintf(out, "\t%%len%" PRIu32 " =l copy %%slen%" PRIu32 "\n", inst.result, inst.a);
        }
        break;
    case IR_STORE:
        if (addressed && addressed[inst.a] != TYPE_INVALID) {
            write_store(out, inst.type.kind, "v", "len", inst.b, "local", inst.a, "store", index);
            break;
        }
        fprintf(out, "\t%%s%" PRIu32 " =%c copy %%v%" PRIu32 "\n",
                inst.a, types[inst.type.kind].base, inst.b);
        if (inst.type.kind == TYPE_SLICE) {
            fprintf(out, "\t%%slen%" PRIu32 " =l copy %%len%" PRIu32 "\n", inst.a, inst.b);
        }
        break;
    case IR_STORAGE_ALLOC:
        // Emitted once in the entry block.
        break;
    case IR_STORAGE_INIT:
        write_storage_init(out, inst, index);
        break;
    case IR_WIDEN_INDEX:
        write_cast(out, inst.result, inst.a, inst.type.kind, TYPE_U64);
        break;
    case IR_CAST:
        write_cast(out, inst.result, inst.a, inst.b, inst.type.kind);
        break;
    case IR_FLOAT_EXTEND:
        fprintf(out, "\t%%v%" PRIu32 " =d exts %%v%" PRIu32 "\n", inst.result, inst.a);
        break;
    case IR_ELEMENT_ADDRESS:
    case IR_SLICE:
    case IR_POINTER_SLICE:
        write_access(out, inst);
        break;
    case IR_POINTER_INDEX:
        write_index_address(out, inst, sema_type_size(inst.type));
        break;
    case IR_FIELD_ADDRESS:
        fprintf(out, "\t%%v%" PRIu32 " =l add %%v%" PRIu32 ", %" PRIu32 "\n",
                inst.result, inst.a, inst.b);
        break;
    case IR_LOCAL_ADDRESS:
        fprintf(out, "\t%%v%" PRIu32 " =l copy %%local%" PRIu32 "\n", inst.result, inst.a);
        break;
    case IR_ARRAY_VIEW:
        fprintf(out, "\t%%v%" PRIu32 " =l copy %%v%" PRIu32 "\n", inst.result, inst.a);
        fprintf(out, "\t%%len%" PRIu32 " =l copy %" PRIu32 "\n", inst.result, inst.type.length);
        break;
    case IR_MEMORY_LOAD:
        write_load(out, inst.type.kind, inst.result, "v", inst.a);
        break;
    case IR_MEMORY_STORE:
        write_store(out, inst.type.kind, "v", "len", inst.b, "v", inst.a, "mem", index);
        break;
    case IR_LEN:
        fprintf(out, "\t%%v%" PRIu32 " =l copy ", inst.result);
        write_length(out, inst.type, inst.a);
        fputc('\n', out);
        break;
    case IR_DATA:
        fprintf(out, "\t%%v%" PRIu32 " =l copy %%v%" PRIu32 "\n", inst.result, inst.a);
        break;
    case IR_CALL:
        write_call(out, module, inst);
        break;
    case IR_JUMP:
        fprintf(out, "\tjmp @b%" PRIu32 "\n", inst.a);
        break;
    case IR_BRANCH:
        fprintf(out, "\tjnz %%v%" PRIu32 ", @b%" PRIu32 ", @b%" PRIu32 "\n",
                inst.a, inst.b, inst.c);
        break;
    case IR_RETURN:
        if (inst.a && result.kind == TYPE_SLICE) {
            fprintf(out, "\tstorel %%v%" PRIu32 ", %%ret\n", inst.a);
            fprintf(out, "\t%%ret_len%" PRIu32 " =l add %%ret, 8\n", index);
            fprintf(out, "\tstorel %%len%" PRIu32 ", %%ret_len%" PRIu32 "\n", inst.a, index);
            fputs("\tret\n", out);
        } else if (inst.a && (result.kind == TYPE_STRUCT || result.kind == TYPE_ARRAY)) {
            write_copy_bytes(out, "ret", 0, false, inst.a, true, sema_storage_size(result), index);
            fputs("\tret\n", out);
        } else {
            fputs("\tret", out);
            if (inst.a) {
                fprintf(out, " %%v%" PRIu32, inst.a);
            }
            fputc('\n', out);
        }
        break;
    case IR_EQUAL:
    case IR_NOT_EQUAL:
    case IR_LESS:
    case IR_LESS_EQUAL:
    case IR_GREATER:
    case IR_GREATER_EQUAL:
        write_comparison(out, inst);
        break;
    default:
        write_operation(out, inst);
        break;
    }
}

static bool collect_addressed_slots(const IrModule *module, IrFunction fn, TypeKind **out)
{
    *out = NULL;
    for (uint32_t i = 0; i < fn.instruction_count; i++) {
        IrInstruction inst = module->instructions[fn.first_instruction + i];
        if (inst.op != IR_LOCAL_ADDRESS) {
            continue;
        }
        if (!*out) {
            *out = calloc(fn.slot_count, sizeof(**out));
            if (!*out) {
                return false;
            }
        }
        (*out)[inst.a] = inst.type.kind;
    }
    return true;
}

static bool write_function(FILE *out, const IrModule *module, uint32_t index)
{
    IrFunction fn = module->functions[index];
    TypeKind *addressed;
    if (!collect_addressed_slots(module, fn, &addressed)) {
        return false;
    }
    fputs(entry_symbol(module, fn) ? "export function " : "function ", out);
    bool sret = uses_sret(fn.result.kind);
    if (fn.result.kind != TYPE_VOID && !sret) {
        fprintf(out, "%s ", types[fn.result.kind].abi);
    }
    write_symbol(out, module, index);
    fputc('(', out);
    if (sret) {
        fputs("l %ret", out);
    }
    for (uint32_t i = 0; i < fn.parameter_count; i++) {
        if (sret || i != 0) {
            fputs(", ", out);
        }
        SemaType parameter = module->parameters[fn.first_parameter + i];
        if (parameter.kind == TYPE_SLICE) {
            fprintf(out, "l %%p%" PRIu32 ", l %%plen%" PRIu32, i, i);
        } else {
            fprintf(out, "%s %%p%" PRIu32, types[parameter.kind].abi, i);
        }
    }
    fputs(") {\n", out);
    bool has_bounds_check = false;
    for (uint32_t i = 0; i < fn.instruction_count; i++) {
        uint32_t index = fn.first_instruction + i;
        write_instruction(out, module, index, addressed, fn.result);
        IrOp op = module->instructions[index].op;
        has_bounds_check |= op == IR_ELEMENT_ADDRESS || op == IR_SLICE || op == IR_POINTER_SLICE;
        if (i == 0) {
            if (addressed) {
                for (uint32_t slot = 0; slot < fn.slot_count; slot++) {
                    if (addressed[slot] != TYPE_INVALID) {
                        uint32_t bytes = addressed[slot] == TYPE_SLICE
                                         ? 2 * sema_scalar_size(TYPE_POINTER)
                                         : sema_scalar_size(addressed[slot]);
                        fprintf(out, "\t%%local%" PRIu32 " =l alloc8 %" PRIu32 "\n",
                                slot, bytes);
                    }
                }
            }
            for (uint32_t p = 0; p < fn.parameter_count; p++) {
                TypeKind kind = module->parameters[fn.first_parameter + p].kind;
                if (addressed && addressed[p] != TYPE_INVALID) {
                    write_store(out, kind, "p", "plen", p, "local", p, "arg", p);
                    continue;
                }
                QbeType type = types[kind];
                fprintf(out, "\t%%s%" PRIu32 " =%c %s %%p%" PRIu32 "\n", p, type.base,
                        type.extend ? type.extend : "copy", p);
                if (kind == TYPE_SLICE) {
                    fprintf(out, "\t%%slen%" PRIu32 " =l copy %%plen%" PRIu32 "\n", p, p);
                }
            }
            for (uint32_t j = 1; j < fn.instruction_count; j++) {
                IrInstruction inst = module->instructions[fn.first_instruction + j];
                if (inst.op == IR_STORAGE_ALLOC) {
                    uint32_t size = sema_storage_size(inst.type);
                    fprintf(out, "\t%%v%" PRIu32 " =l %s %" PRIu32 "\n",
                            inst.result, size ? "alloc8" : "copy", size);
                } else if (inst.op == IR_CALL && uses_sret(inst.type.kind)) {
                    if (inst.type.kind == TYPE_SLICE) {
                        fprintf(out, "\t%%retbuf%" PRIu32 " =l alloc8 16\n", inst.result);
                    } else {
                        uint32_t size = sema_storage_size(inst.type);
                        fprintf(out, "\t%%v%" PRIu32 " =l %s %" PRIu32 "\n",
                                inst.result, size ? "alloc8" : "copy", size);
                    }
                }
            }
        }
    }
    if (has_bounds_check) {
        fputs("@bounds_fail\n\thlt\n", out);
    }
    fputs("}\n", out);
    free(addressed);
    return true;
}

bool qbe_write(FILE *out, const IrModule *module)
{
    if (!write_data(out, module)) {
        return false;
    }
    for (uint32_t i = 0; i < module->function_count; i++) {
        IrFunction fn = module->functions[i];
        if (!fn.template_only && !fn.external && !write_function(out, module, i)) {
            return false;
        }
    }
    return !ferror(out);
}
