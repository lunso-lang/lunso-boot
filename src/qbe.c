#include "qbe.h"

#include "literal.h"

#include <inttypes.h>
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
    [TYPE_BYTES] = {'l', NULL, NULL},
};

static bool is_signed(TypeKind type)
{
    return type >= TYPE_I8 && type <= TYPE_I64;
}

static bool is_unsigned(TypeKind type)
{
    return type >= TYPE_U8 && type <= TYPE_U64;
}

static void write_symbol(FILE *out, const IrModule *module, AstSpan name)
{
    fputc('$', out);
    fwrite(module->source + name.offset, 1, name.length, out);
}

static void write_integer(FILE *out, const unsigned char *text, uint32_t length, bool negative)
{
    uint64_t bits = 0;
    for (uint32_t i = 0; i < length; i++) {
        bits = bits * 10 + (unsigned)(text[i] - '0');
    }
    if (negative) {
        bits = 0 - bits;
    }
    if (bits > INT64_MAX) {
        fprintf(out, "-%" PRIu64, UINT64_C(0) - bits);
    } else {
        fprintf(out, "%" PRIu64, bits);
    }
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

static void write_call(FILE *out, const IrModule *module, IrInstruction inst)
{
    IrFunction callee = module->functions[inst.a];
    QbeType result = types[callee.result.kind];
    fputc('\t', out);
    if (inst.result) {
        fprintf(out, "%%%s%" PRIu32 " =%s ", result.extend ? "raw" : "v", inst.result,
                result.abi);
    }
    fputs("call ", out);
    write_symbol(out, module, callee.name);
    fputc('(', out);
    for (uint32_t i = 0; i < inst.c; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        SemaType type = module->parameters[callee.first_parameter + i];
        fprintf(out, "%s %%v%" PRIu32, types[type.kind].abi, module->arguments[inst.b + i]);
    }
    fputs(")\n", out);
    if (result.extend) {
        fprintf(out, "\t%%v%" PRIu32 " =w %s %%raw%" PRIu32 "\n",
                inst.result, result.extend, inst.result);
    }
}

static void write_index(FILE *out, IrInstruction inst)
{
    if (types[inst.type.kind].base == 'l') {
        fprintf(out, "\t%%idx%" PRIu32 " =l copy %%v%" PRIu32 "\n", inst.result, inst.b);
    } else {
        fprintf(out, "\t%%idx%" PRIu32 " =l %s %%v%" PRIu32 "\n", inst.result,
                is_signed(inst.type.kind) ? "extsw" : "extuw", inst.b);
    }
    // An unsigned comparison also rejects sign-extended negative indices.
    fprintf(out, "\t%%in%" PRIu32 " =w cultl %%idx%" PRIu32 ", %" PRIu32 "\n",
            inst.result, inst.result, inst.c);
    fprintf(out, "\tjnz %%in%" PRIu32 ", @index_ok%" PRIu32 ", @bounds_fail\n",
            inst.result, inst.result);
    fprintf(out, "@index_ok%" PRIu32 "\n", inst.result);
    fprintf(out, "\t%%addr%" PRIu32 " =l add %%v%" PRIu32 ", %%idx%" PRIu32 "\n",
            inst.result, inst.a, inst.result);
    fprintf(out, "\t%%v%" PRIu32 " =w loadub %%addr%" PRIu32 "\n", inst.result, inst.result);
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

static void write_instruction(FILE *out, const IrModule *module, uint32_t index)
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
    case IR_ZERO:
        fprintf(out, "\t%%v%" PRIu32 " =%c copy %" PRIu32 "\n",
                inst.result, types[inst.type.kind].base, inst.a);
        break;
    case IR_BYTES:
        fprintf(out, "\t%%v%" PRIu32 " =l copy $.lunso.bytes.%" PRIu32 "\n", inst.result, index);
        break;
    case IR_LOAD:
        fprintf(out, "\t%%v%" PRIu32 " =%c copy %%s%" PRIu32 "\n",
                inst.result, types[inst.type.kind].base, inst.a);
        break;
    case IR_STORE:
        fprintf(out, "\t%%s%" PRIu32 " =%c copy %%v%" PRIu32 "\n",
                inst.a, types[inst.type.kind].base, inst.b);
        break;
    case IR_INDEX:
        write_index(out, inst);
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
        fputs("\tret", out);
        if (inst.a) {
            fprintf(out, " %%v%" PRIu32, inst.a);
        }
        fputc('\n', out);
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

static void write_function(FILE *out, const IrModule *module, IrFunction fn)
{
    bool is_main = fn.name.length == 4 && !memcmp(module->source + fn.name.offset, "main", 4);
    fputs(is_main ? "export function " : "function ", out);
    if (fn.result.kind != TYPE_VOID) {
        fprintf(out, "%s ", types[fn.result.kind].abi);
    }
    write_symbol(out, module, fn.name);
    fputc('(', out);
    for (uint32_t i = 0; i < fn.parameter_count; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        SemaType parameter = module->parameters[fn.first_parameter + i];
        fprintf(out, "%s %%p%" PRIu32, types[parameter.kind].abi, i);
    }
    fputs(") {\n", out);
    bool has_index = false;
    for (uint32_t i = 0; i < fn.instruction_count; i++) {
        uint32_t index = fn.first_instruction + i;
        write_instruction(out, module, index);
        has_index |= module->instructions[index].op == IR_INDEX;
        if (i == 0) {
            for (uint32_t p = 0; p < fn.parameter_count; p++) {
                QbeType type = types[module->parameters[fn.first_parameter + p].kind];
                fprintf(out, "\t%%s%" PRIu32 " =%c %s %%p%" PRIu32 "\n", p, type.base,
                        type.extend ? type.extend : "copy", p);
            }
        }
    }
    if (has_index) {
        fputs("@bounds_fail\n\thlt\n", out);
    }
    fputs("}\n", out);
}

bool qbe_write(FILE *out, const IrModule *module)
{
    if (!write_data(out, module)) {
        return false;
    }
    for (uint32_t i = 0; i < module->function_count; i++) {
        IrFunction fn = module->functions[i];
        if (!fn.external) {
            write_function(out, module, fn);
        }
    }
    return !ferror(out);
}
