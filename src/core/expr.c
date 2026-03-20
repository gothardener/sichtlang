#include "expr.h"
#include <stdint.h>
#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

static char* arena_strdup(const char* s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "Expression string exceeded maximum supported size.\n");
        exit(1);
    }
    char* out = arena_alloc(len + 1);
    memcpy(out, s, len + 1);
    return out;
}

static Expr* alloc_expr(ExprType type) {
    Expr* e = arena_alloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->type = type;
    e->line = 0;
    e->column = 0;
    return e;
}

Expr* expr_literal(int value) {
    Expr* e = alloc_expr(EXPR_LITERAL);
    e->int_value = value;
    return e;
}

Expr* expr_float(double value) {
    Expr* e = alloc_expr(EXPR_FLOAT_LITERAL);
    e->float_value = value;
    return e;
}

Expr* expr_bool(int value) {
    Expr* e = alloc_expr(EXPR_BOOL_LITERAL);
    e->int_value = value ? 1 : 0;
    return e;
}

Expr* expr_string(const char* s) {
    Expr* e = alloc_expr(EXPR_STRING_LITERAL);
    e->string_value = arena_strdup(s);
    return e;
}

Expr* expr_variable(const char* name) {
    Expr* e = alloc_expr(EXPR_VARIABLE);
    e->name = arena_strdup(name);
    return e;
}

Expr* expr_binary(OpType op, Expr* left, Expr* right) {
    Expr* e = alloc_expr(EXPR_BINARY);
    e->op = op;
    e->left = left;
    e->right = right;
    return e;
}

Expr* expr_unary(OpType op, Expr* expr) {
    Expr* e = alloc_expr(EXPR_UNARY);
    e->op = op;
    e->expr = expr;
    return e;
}

Expr* expr_cast(Expr* expression, CastType type) {
    Expr* e = alloc_expr(EXPR_CAST);
    e->cast_expr = expression;
    e->cast_type = type;
    return e;
}

Expr* expr_input(char* prompt) {
    Expr* e = alloc_expr(EXPR_INPUT);

    if (prompt)
        e->input_prompt = arena_strdup(prompt);
    else
        e->input_prompt = NULL;

    return e;
}

Expr* expr_builtin(BuiltinType type, Expr* arg) {
    Expr* e = alloc_expr(EXPR_BUILTIN);
    e->builtin_type = type;
    e->builtin_arg = arg;
    return e;
}

Expr* expr_char_at(Expr* string, Expr* index) {
    Expr* e = alloc_expr(EXPR_CHAR_AT);
    e->char_string = string;
    e->char_index = index;
    return e;
}

Expr* expr_array_literal(Expr** elements, int count) {
    Expr* e = alloc_expr(EXPR_ARRAY_LITERAL);
    e->elements = elements;
    e->element_count = count;
    return e;
}

Expr* expr_array_index(Expr* array, Expr* index) {
    Expr* e = alloc_expr(EXPR_ARRAY_INDEX);
    e->array_expr = array;
    e->index_expr = index;
    return e;
}

Expr* expr_list(Expr** items, int count) {
    Expr* e = alloc_expr(EXPR_LIST);
    e->list_items = items;
    e->list_count = count;
    return e;
}

Expr* expr_list_at(Expr* list, Expr* index) {
    Expr* e = alloc_expr(EXPR_LIST_AT);
    e->list_expr = list;
    e->list_index = index;
    return e;
}

Expr* expr_index_of(Expr* value, Expr* array) {
    Expr* e = alloc_expr(EXPR_INDEX_OF);
    e->index_value = value;
    e->index_array = array;
    return e;
}

Expr* expr_dict(Expr** keys, Expr** values, int count) {
    Expr* e = alloc_expr(EXPR_DICT);
    e->dict_keys = keys;
    e->dict_values = values;
    e->dict_count = count;
    return e;
}

Expr* expr_dict_get(Expr* key, Expr* dict) {
    Expr* e = alloc_expr(EXPR_DICT_GET);
    e->dict_key = key;
    e->dict_expr = dict;
    return e;
}

Expr* expr_list_add(char* name, Expr* value)
{
    Expr* e = alloc_expr(EXPR_LIST_ADD);
    e->list_add_name = arena_strdup(name);
    e->list_add_value = value;

    return e;
}

Expr* expr_call(const char* name, Expr** args, char** arg_names, int count) {
    Expr* e = alloc_expr(EXPR_CALL);
    e->call_name = arena_strdup(name);
    e->call_args = args;
    e->call_arg_names = arg_names;
    e->call_arg_count = count;
    return e;
}

Expr* expr_list_comprehension(const char* var_name, Expr* iterable, Expr* filter, Expr* result) {
    Expr* e = alloc_expr(EXPR_LIST_COMPREHENSION);
    e->comp_var_name = arena_strdup(var_name);
    e->comp_iterable = iterable;
    e->comp_filter = filter;
    e->comp_result = result;
    return e;
}

Expr* expr_file_read(Expr* path_expr) {
    Expr* e = alloc_expr(EXPR_FILE_READ);
    e->file_read_path = path_expr;
    return e;
}

Expr* expr_llvl_value_of(Expr* target) {
    Expr* e = alloc_expr(EXPR_LLVL_VALUE_OF);
    e->llvl_target = target;
    return e;
}

Expr* expr_llvl_byte_of(Expr* target, Expr* index) {
    Expr* e = alloc_expr(EXPR_LLVL_BYTE_OF);
    e->llvl_target = target;
    e->llvl_index = index;
    return e;
}

Expr* expr_llvl_bit_of(Expr* target, Expr* index) {
    Expr* e = alloc_expr(EXPR_LLVL_BIT_OF);
    e->llvl_target = target;
    e->llvl_index = index;
    return e;
}

Expr* expr_llvl_place_of(Expr* target) {
    Expr* e = alloc_expr(EXPR_LLVL_PLACE_OF);
    e->llvl_target = target;
    return e;
}

Expr* expr_llvl_read_pin(Expr* pin) {
    Expr* e = alloc_expr(EXPR_LLVL_READ_PIN);
    e->llvl_target = pin;
    return e;
}

Expr* expr_llvl_port_read(Expr* port) {
    Expr* e = alloc_expr(EXPR_LLVL_PORT_READ);
    e->llvl_target = port;
    return e;
}

Expr* expr_llvl_offset(Expr* base, Expr* offset) {
    Expr* e = alloc_expr(EXPR_LLVL_OFFSET);
    e->llvl_target = base;
    e->llvl_index = offset;
    return e;
}

Expr* expr_llvl_field(const char* field_name, Expr* target) {
    Expr* e = alloc_expr(EXPR_LLVL_FIELD);
    e->name = arena_strdup(field_name);
    e->llvl_target = target;
    return e;
}

Expr* expr_llvl_atomic_read(Expr* target) {
    Expr* e = alloc_expr(EXPR_LLVL_ATOMIC_READ);
    e->llvl_target = target;
    return e;
}

void expr_set_location(Expr* expr, int line, int column) {
    if (!expr)
        return;
    if (line > 0)
        expr->line = line;
    if (column > 0)
        expr->column = column;
}

static const char* op_type_name(OpType op) {
    switch (op) {
        case OP_ADD: return "OP_ADD";
        case OP_SUB: return "OP_SUB";
        case OP_MUL: return "OP_MUL";
        case OP_DIV: return "OP_DIV";
        case OP_AND: return "OP_AND";
        case OP_OR: return "OP_OR";
        case OP_NOT: return "OP_NOT";
        case OP_BIT_AND: return "OP_BIT_AND";
        case OP_BIT_OR: return "OP_BIT_OR";
        case OP_BIT_XOR: return "OP_BIT_XOR";
        case OP_BIT_NOT: return "OP_BIT_NOT";
        case OP_SHL: return "OP_SHL";
        case OP_SHR: return "OP_SHR";
        case OP_NEG: return "OP_NEG";
        case OP_EQ: return "OP_EQ";
        case OP_NE: return "OP_NE";
        case OP_GT: return "OP_GT";
        case OP_LT: return "OP_LT";
        case OP_GTE: return "OP_GTE";
        case OP_LTE: return "OP_LTE";
        case OP_CONTAINS: return "OP_CONTAINS";
    }
    return "OP_UNKNOWN";
}

static const char* cast_type_name(CastType type) {
    switch (type) {
        case CAST_TO_INTEGER: return "CAST_TO_INTEGER";
        case CAST_TO_FLOAT: return "CAST_TO_FLOAT";
        case CAST_TO_STRING: return "CAST_TO_STRING";
        case CAST_TO_BOOLEAN: return "CAST_TO_BOOLEAN";
    }
    return "CAST_UNKNOWN";
}

static const char* builtin_type_name(BuiltinType type) {
    switch (type) {
        case BUILTIN_UPPERCASE: return "BUILTIN_UPPERCASE";
        case BUILTIN_LOWERCASE: return "BUILTIN_LOWERCASE";
        case BUILTIN_TRIM: return "BUILTIN_TRIM";
        case BUILTIN_LENGTH: return "BUILTIN_LENGTH";
        case BUILTIN_SORT: return "BUILTIN_SORT";
        case BUILTIN_REVERSE: return "BUILTIN_REVERSE";
        case BUILTIN_NEXT: return "BUILTIN_NEXT";
    }
    return "BUILTIN_UNKNOWN";
}

static void dump_indent(int indent) {
    for (int i = 0; i < indent; i++)
        printf("  ");
}

static void expr_dump_node(const Expr* expr, int indent) {
    if (!expr) {
        dump_indent(indent);
        printf("(null)\n");
        return;
    }

    dump_indent(indent);
    printf("%s", expr_type_name(expr->type));

    switch (expr->type) {
        case EXPR_LITERAL:
            printf(" value=%d\n", expr->int_value);
            return;
        case EXPR_FLOAT_LITERAL:
            printf(" value=%g\n", expr->float_value);
            return;
        case EXPR_BOOL_LITERAL:
            printf(" value=%s\n", expr->int_value ? "true" : "false");
            return;
        case EXPR_STRING_LITERAL:
            printf(" value=\"%s\"\n", expr->string_value ? expr->string_value : "");
            return;
        case EXPR_VARIABLE:
            printf(" name=%s\n", expr->name ? expr->name : "");
            return;
        case EXPR_UNARY:
            printf(" op=%s\n", op_type_name(expr->op));
            expr_dump_node(expr->expr, indent + 1);
            return;
        case EXPR_BINARY:
            printf(" op=%s\n", op_type_name(expr->op));
            expr_dump_node(expr->left, indent + 1);
            expr_dump_node(expr->right, indent + 1);
            return;
        case EXPR_CAST:
            printf(" type=%s\n", cast_type_name(expr->cast_type));
            expr_dump_node(expr->cast_expr, indent + 1);
            return;
        case EXPR_INPUT:
            printf(" prompt=\"%s\"\n", expr->input_prompt ? expr->input_prompt : "");
            return;
        case EXPR_BUILTIN:
            printf(" builtin=%s\n", builtin_type_name(expr->builtin_type));
            expr_dump_node(expr->builtin_arg, indent + 1);
            return;
        case EXPR_CHAR_AT:
            printf("\n");
            expr_dump_node(expr->char_string, indent + 1);
            expr_dump_node(expr->char_index, indent + 1);
            return;
        case EXPR_ARRAY_LITERAL:
            printf(" elements=%d\n", expr->element_count);
            for (int i = 0; i < expr->element_count; i++)
                expr_dump_node(expr->elements[i], indent + 1);
            return;
        case EXPR_ARRAY_INDEX:
            printf("\n");
            expr_dump_node(expr->array_expr, indent + 1);
            expr_dump_node(expr->index_expr, indent + 1);
            return;
        case EXPR_LIST:
            printf(" items=%d\n", expr->list_count);
            for (int i = 0; i < expr->list_count; i++)
                expr_dump_node(expr->list_items[i], indent + 1);
            return;
        case EXPR_LIST_AT:
            printf("\n");
            expr_dump_node(expr->list_expr, indent + 1);
            expr_dump_node(expr->list_index, indent + 1);
            return;
        case EXPR_INDEX_OF:
            printf("\n");
            expr_dump_node(expr->index_value, indent + 1);
            expr_dump_node(expr->index_array, indent + 1);
            return;
        case EXPR_DICT:
            printf(" items=%d\n", expr->dict_count);
            for (int i = 0; i < expr->dict_count; i++) {
                dump_indent(indent + 1);
                printf("key:\n");
                expr_dump_node(expr->dict_keys[i], indent + 2);
                dump_indent(indent + 1);
                printf("value:\n");
                expr_dump_node(expr->dict_values[i], indent + 2);
            }
            return;
        case EXPR_DICT_GET:
            printf("\n");
            expr_dump_node(expr->dict_key, indent + 1);
            expr_dump_node(expr->dict_expr, indent + 1);
            return;
        case EXPR_CALL:
            printf(" name=%s args=%d\n", expr->call_name ? expr->call_name : "", expr->call_arg_count);
            for (int i = 0; i < expr->call_arg_count; i++) {
                dump_indent(indent + 1);
                if (expr->call_arg_names && expr->call_arg_names[i])
                    printf("arg %d name=%s:\n", i + 1, expr->call_arg_names[i]);
                else
                    printf("arg %d:\n", i + 1);
                expr_dump_node(expr->call_args[i], indent + 2);
            }
            return;
        case EXPR_LIST_ADD:
            printf(" list=%s\n", expr->list_add_name ? expr->list_add_name : "");
            expr_dump_node(expr->list_add_value, indent + 1);
            return;
        case EXPR_LIST_COMPREHENSION:
            printf(" var=%s\n", expr->comp_var_name ? expr->comp_var_name : "");
            dump_indent(indent + 1);
            printf("iterable:\n");
            expr_dump_node(expr->comp_iterable, indent + 2);
            if (expr->comp_filter) {
                dump_indent(indent + 1);
                printf("filter:\n");
                expr_dump_node(expr->comp_filter, indent + 2);
            }
            dump_indent(indent + 1);
            printf("result:\n");
            expr_dump_node(expr->comp_result, indent + 2);
            return;
        case EXPR_FILE_READ:
            printf("\n");
            expr_dump_node(expr->file_read_path, indent + 1);
            return;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_ATOMIC_READ:
            printf("\n");
            expr_dump_node(expr->llvl_target, indent + 1);
            return;
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            printf("\n");
            expr_dump_node(expr->llvl_target, indent + 1);
            return;
        case EXPR_LLVL_OFFSET:
            printf("\n");
            expr_dump_node(expr->llvl_target, indent + 1);
            expr_dump_node(expr->llvl_index, indent + 1);
            return;
        case EXPR_LLVL_FIELD:
            printf(" name=%s\n", expr->name ? expr->name : "");
            expr_dump_node(expr->llvl_target, indent + 1);
            return;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            printf("\n");
            expr_dump_node(expr->llvl_target, indent + 1);
            expr_dump_node(expr->llvl_index, indent + 1);
            return;
    }
}

void expr_dump(const Expr* expr) {
    expr_dump_node(expr, 0);
}

const char* expr_type_name(ExprType type) {
    switch (type) {
        case EXPR_LITERAL: return "EXPR_LITERAL";
        case EXPR_FLOAT_LITERAL: return "EXPR_FLOAT_LITERAL";
        case EXPR_BOOL_LITERAL: return "EXPR_BOOL_LITERAL";
        case EXPR_STRING_LITERAL: return "EXPR_STRING_LITERAL";
        case EXPR_VARIABLE: return "EXPR_VARIABLE";
        case EXPR_BINARY: return "EXPR_BINARY";
        case EXPR_UNARY: return "EXPR_UNARY";
        case EXPR_CAST: return "EXPR_CAST";
        case EXPR_INPUT: return "EXPR_INPUT";
        case EXPR_BUILTIN: return "EXPR_BUILTIN";
        case EXPR_CHAR_AT: return "EXPR_CHAR_AT";
        case EXPR_ARRAY_LITERAL: return "EXPR_ARRAY_LITERAL";
        case EXPR_ARRAY_INDEX: return "EXPR_ARRAY_INDEX";
        case EXPR_LIST: return "EXPR_LIST";
        case EXPR_LIST_AT: return "EXPR_LIST_AT";
        case EXPR_INDEX_OF: return "EXPR_INDEX_OF";
        case EXPR_DICT: return "EXPR_DICT";
        case EXPR_DICT_GET: return "EXPR_DICT_GET";
        case EXPR_CALL: return "EXPR_CALL";
        case EXPR_LIST_ADD: return "EXPR_LIST_ADD";
        case EXPR_LIST_COMPREHENSION: return "EXPR_LIST_COMPREHENSION";
        case EXPR_FILE_READ: return "EXPR_FILE_READ";
        case EXPR_LLVL_VALUE_OF: return "EXPR_LLVL_VALUE_OF";
        case EXPR_LLVL_BYTE_OF: return "EXPR_LLVL_BYTE_OF";
        case EXPR_LLVL_BIT_OF: return "EXPR_LLVL_BIT_OF";
        case EXPR_LLVL_PLACE_OF: return "EXPR_LLVL_PLACE_OF";
        case EXPR_LLVL_READ_PIN: return "EXPR_LLVL_READ_PIN";
        case EXPR_LLVL_PORT_READ: return "EXPR_LLVL_PORT_READ";
        case EXPR_LLVL_OFFSET: return "EXPR_LLVL_OFFSET";
        case EXPR_LLVL_FIELD: return "EXPR_LLVL_FIELD";
        case EXPR_LLVL_ATOMIC_READ: return "EXPR_LLVL_ATOMIC_READ";
    }
    return "EXPR_UNKNOWN";
}


