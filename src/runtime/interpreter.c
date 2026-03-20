#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

#if !defined(_WIN32)
#if defined(__has_include)
#if __has_include(<curl/curl.h>)
#define SICHT_HAS_LIBCURL 1
#endif
#endif
#if defined(SICHT_HAS_LIBCURL)
#include <curl/curl.h>
#endif
#if defined(SICHT_USE_LIBCURL) && !defined(SICHT_HAS_LIBCURL)
#error "SICHT_USE_LIBCURL set but curl/curl.h not found"
#endif
#if defined(SICHT_USE_LIBCURL) && defined(SICHT_HAS_LIBCURL)
#define SICHT_LIBCURL_ENABLED 1
#endif
#endif

#include "interpreter.h"
#include "value.h"
#include "ast.h"
#include "expr.h"
#include "gc.h"
#include "arena.h"
#include "lexer.h"
#include "parser.h"
#include "native_rt.h"
#include "debugger.h"
#include "runtime_error.h"
#include "source.h"

#ifdef _WIN32
#define sicht_popen _popen
#define sicht_pclose _pclose
#else
#define sicht_popen popen
#define sicht_pclose pclose
#endif


#define MAX_FRAMES 64
#define MAX_LIBRARIES 128
#define MAX_LIBRARY_OFFERS 1024
#define MAX_TYPES 128
#define MAX_LLVL_TYPES 128
#define MAX_CALL_SLOTS 64

typedef struct {
    char* name;
    Value value;
} Var;

typedef struct {
    const char* name;
    ASTNode* node;
    const char* owner_library;
    int imported;
    int is_generator;
    int owns_node;
} FunctionDef;

typedef struct {
    char* library_name;
    char* symbol_name;
} LibraryOffer;

typedef struct {
    char* name;
    char** field_names;
    int field_count;
} TypeDef;

typedef struct {
    char* name;
    LlvlTypeKind kind;
    char* type_name;
    int array_len;
    size_t offset;
    size_t size;
} LlvlFieldDef;

typedef struct {
    char* name;
    int is_union;
    LlvlFieldDef* fields;
    int field_count;
    size_t size;
} LlvlTypeDef;

static Var* vars = NULL;
static int var_count = 0;
static int var_capacity = 0;
static FunctionDef* functions = NULL;
static int function_count = 0;
static int function_capacity = 0;
static int frame_bases[MAX_FRAMES];
static int frame_depth = 0;
static int trace_enabled = 0;
static int in_function_call = 0;
static int function_has_value = 0;
static int function_returned = 0;
static Value function_last_value;
static List* function_yield_list = NULL;
static int function_yield_used = 0;
static char* loaded_libraries[MAX_LIBRARIES];
static int loaded_library_count = 0;
static char* loading_libraries[MAX_LIBRARIES];
static int loading_library_count = 0;
static int llvl_depth = 0;
static char* loading_files[MAX_LIBRARIES];
static int loading_file_count = 0;
static LibraryOffer library_offers[MAX_LIBRARY_OFFERS];
static int library_offer_count = 0;
static TypeDef types[MAX_TYPES];
static int type_count = 0;
static LlvlTypeDef llvl_types[MAX_LLVL_TYPES];
static int llvl_type_count = 0;
static Value* gc_roots_buffer = NULL;
static int gc_roots_capacity = 0;
static int gc_poll_budget = 0;
static long long execution_steps = 0;
static long long execution_step_limit = 0;
static long long generator_yield_limit = 0;
static const char* current_loading_library = NULL;
static const char* active_function_library = NULL;
static const char* call_stack_names[MAX_FRAMES];
static const char* call_stack_files[MAX_FRAMES];
static int call_stack_lines[MAX_FRAMES];
static int call_stack_columns[MAX_FRAMES];
static void print_value_inline(Value v);

static int ast_block_contains_yield(ASTNode* node);
static void materialize_generator(Generator* g);
static int generator_next_value(Generator* g, Value* out);
static void execution_tick(void);
static Value eval(Expr* e);
static int value_collection_count(Value source);
static int current_frame_base(void);

static void ensure_function_capacity(void) {
    if (function_count < function_capacity)
        return;

    int new_capacity = function_capacity > 0 ? function_capacity * 2 : 256;
    if (new_capacity < function_capacity)
        runtime_error("Out of memory", "Function table exceeded maximum supported size.", "");
    if ((size_t)new_capacity > SIZE_MAX / sizeof(FunctionDef))
        runtime_error("Out of memory", "Function table exceeded maximum supported size.", "");
    FunctionDef* grown = (FunctionDef*)realloc(functions, (size_t)new_capacity * sizeof(FunctionDef));
    if (!grown)
        runtime_error("Out of memory", "Could not grow function table.", "");

    functions = grown;
    function_capacity = new_capacity;
}

static void ensure_var_capacity(void) {
    if (var_count < var_capacity)
        return;

    int new_capacity = var_capacity > 0 ? var_capacity * 2 : 256;
    if (new_capacity < var_capacity)
        runtime_error("Out of memory", "Variable table exceeded maximum supported size.", "");
    if ((size_t)new_capacity > SIZE_MAX / sizeof(Var))
        runtime_error("Out of memory", "Variable table exceeded maximum supported size.", "");
    Var* grown = (Var*)realloc(vars, (size_t)new_capacity * sizeof(Var));
    if (!grown)
        runtime_error("Out of memory", "Could not grow variable table.", "");

    vars = grown;
    var_capacity = new_capacity;
}

static void ensure_gc_roots_capacity(int required) {
    if (required <= gc_roots_capacity)
        return;

    int new_capacity = gc_roots_capacity > 0 ? gc_roots_capacity : 256;
    while (new_capacity < required) {
        if (new_capacity > INT_MAX / 2)
            runtime_error("Out of memory", "GC roots buffer exceeded maximum supported size.", "");
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(Value))
        runtime_error("Out of memory", "GC roots buffer exceeded maximum supported size.", "");

    Value* grown = (Value*)realloc(gc_roots_buffer, (size_t)new_capacity * sizeof(Value));
    if (!grown)
        runtime_error("Out of memory", "Could not grow GC roots buffer.", "");

    gc_roots_buffer = grown;
    gc_roots_capacity = new_capacity;
}

static int int_add_checked(int a, int b, int* out) {
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b))
        return 0;
    *out = a + b;
    return 1;
}

static int int_sub_checked(int a, int b, int* out) {
    if ((b > 0 && a < INT_MIN + b) || (b < 0 && a > INT_MAX + b))
        return 0;
    *out = a - b;
    return 1;
}

static int int_mul_checked(int a, int b, int* out) {
    long long v = (long long)a * (long long)b;
    if (v < INT_MIN || v > INT_MAX)
        return 0;
    *out = (int)v;
    return 1;
}

static long long read_step_limit_from_env(void) {
    const char* raw = getenv("SICHT_MAX_STEPS");
    if (!raw || !raw[0])
        return 0;

    errno = 0;
    char* end = NULL;
    long long v = strtoll(raw, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v <= 0) {
        runtime_error(
            "Invalid environment setting",
            "SICHT_MAX_STEPS must be a positive integer.",
            "Set SICHT_MAX_STEPS to a value like 100000, or unset it."
        );
    }

    return v;
}

static long long read_generator_limit_from_env(void) {
    const char* raw = getenv("SICHT_MAX_GENERATOR_ITEMS");
    if (!raw || !raw[0])
        return 0;

    errno = 0;
    char* end = NULL;
    long long v = strtoll(raw, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v <= 0) {
        runtime_error(
            "Invalid environment setting",
            "SICHT_MAX_GENERATOR_ITEMS must be a positive integer.",
            "Set SICHT_MAX_GENERATOR_ITEMS to a value like 10000, or unset it."
        );
    }

    return v;
}

static void execution_tick(void) {
    if (execution_step_limit <= 0)
        return;

    execution_steps++;
    if (execution_steps > execution_step_limit) {
        runtime_error(
            "Execution step limit exceeded",
            "Program exceeded configured step budget.",
            "Raise SICHT_MAX_STEPS or simplify the loop/control flow."
        );
    }
}

static void llvl_enter(void) {
    llvl_depth++;
    if (llvl_depth == 1)
        gc_set_paused(1);
}

static void llvl_exit(void) {
    if (llvl_depth <= 0)
        return;
    llvl_depth--;
    if (llvl_depth == 0)
        gc_set_paused(0);
}

static void require_llvl(const char* title) {
    if (llvl_depth > 0)
        return;
    runtime_error(
        title,
        "This operation is only available inside llvl blocks.",
        "Wrap the code with: llvl ... end llvl"
    );
}

static char* sicht_strdup_checked(const char* s) {
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1)
        runtime_error("Out of memory", "Could not duplicate string.", "");
    len += 1;
    char* out = malloc(len);
    if (!out) {
        runtime_error("Out of memory", "Could not duplicate string.", "");
        return NULL;
    }
    memcpy(out, s, len);
    return out;
}

static char* heap_strdup_optional(const char* s) {
    if (!s)
        return NULL;
    return sicht_strdup_checked(s);
}

static void* malloc_checked(size_t size) {
    void* out = malloc(size);
    if (!out)
        runtime_error("Out of memory", "Allocation failed.", "");
    return out;
}

static void* calloc_checked(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size)
        runtime_error("Out of memory", "Allocation size overflow.", "");
    void* out = calloc(count, size);
    if (!out)
        runtime_error("Out of memory", "Allocation failed.", "");
    return out;
}

static Expr* expr_clone(const Expr* src);
static void expr_free(Expr* expr);
static ASTNode* ast_clone(const ASTNode* src);
static void ast_free(ASTNode* node);
static StringPart* string_parts_clone(const StringPart* parts, int count);
static void string_parts_free(StringPart* parts, int count);

static const char* value_type_name(ValueType type) {
    switch (type) {
        case VALUE_INT: return "integer";
        case VALUE_FLOAT: return "float";
        case VALUE_BOOL: return "boolean";
        case VALUE_STRING: return "string";
        case VALUE_LIST: return "list";
        case VALUE_DICT: return "dictionary";
        case VALUE_GENERATOR: return "generator";
        case VALUE_BUFFER: return "buffer";
        case VALUE_ADDRESS: return "address";
        default: return "unknown";
    }
}

static void json_escape(const char* input, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!input) return;
    size_t pos = 0;
    for (const unsigned char* p = (const unsigned char*)input; *p && pos + 1 < out_size; p++) {
        unsigned char c = *p;
        const char* repl = NULL;
        char tmp[2] = {0, 0};
        if (c == '\\') repl = "\\\\";
        else if (c == '\"') repl = "\\\"";
        else if (c == '\n') repl = "\\n";
        else if (c == '\r') repl = "\\r";
        else if (c == '\t') repl = "\\t";
        else if (c < 32) repl = " ";
        if (repl) {
            size_t rlen = strlen(repl);
            if (pos + rlen >= out_size)
                break;
            memcpy(out + pos, repl, rlen);
            pos += rlen;
            continue;
        }
        tmp[0] = (char)c;
        if (pos + 1 >= out_size)
            break;
        out[pos++] = tmp[0];
    }
    out[pos] = '\0';
}

static void debug_value_summary(Value v, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    switch (v.type) {
        case VALUE_STRING: {
            const char* s = v.string_value ? v.string_value : "";
            size_t len = strlen(s);
            size_t max_len = out_size > 4 ? out_size - 4 : 0;
            if (len > max_len && max_len > 0) {
                memcpy(out, s, max_len);
                out[max_len] = '\0';
                strncat(out, "...", out_size - strlen(out) - 1);
            } else {
                snprintf(out, out_size, "%s", s);
            }
            return;
        }
        case VALUE_BOOL:
            snprintf(out, out_size, "%s", v.int_value ? "true" : "false");
            return;
        case VALUE_FLOAT:
            snprintf(out, out_size, "%g", v.float_value);
            return;
        case VALUE_INT:
            snprintf(out, out_size, "%d", v.int_value);
            return;
        case VALUE_BUFFER: {
            size_t size = v.buffer_value ? v.buffer_value->size : 0;
            snprintf(out, out_size, "<buffer %zu bytes>", size);
            return;
        }
        case VALUE_ADDRESS:
            snprintf(out, out_size, "<address %p>", v.address_value);
            return;
        case VALUE_LIST: {
            int count = value_collection_count(v);
            if (count >= 0)
                snprintf(out, out_size, "<list %d>", count);
            else
                snprintf(out, out_size, "<list>");
            return;
        }
        case VALUE_DICT: {
            int count = value_collection_count(v);
            if (count >= 0)
                snprintf(out, out_size, "<dictionary %d>", count);
            else
                snprintf(out, out_size, "<dictionary>");
            return;
        }
        case VALUE_GENERATOR:
            snprintf(out, out_size, "<generator>");
            return;
        default:
            snprintf(out, out_size, "<unknown>");
            return;
    }
}

static void debug_push_call(const char* name, int line, int column) {
    if (frame_depth <= 0 || frame_depth > MAX_FRAMES)
        return;
    int idx = frame_depth - 1;
    call_stack_names[idx] = heap_strdup_optional(name ? name : "function");
    {
        const char* label = source_get_label();
        call_stack_files[idx] = heap_strdup_optional(label ? label : "");
    }
    call_stack_lines[idx] = line;
    call_stack_columns[idx] = column;
}

static void debug_pop_call(void) {
    if (frame_depth <= 0 || frame_depth > MAX_FRAMES)
        return;
    int idx = frame_depth - 1;
    free((void*)call_stack_names[idx]);
    free((void*)call_stack_files[idx]);
    call_stack_names[idx] = NULL;
    call_stack_files[idx] = NULL;
    call_stack_lines[idx] = 0;
    call_stack_columns[idx] = 0;
}

static void debug_reset_calls(void) {
    for (int i = 0; i < MAX_FRAMES; i++) {
        free((void*)call_stack_names[i]);
        free((void*)call_stack_files[i]);
        call_stack_names[i] = NULL;
        call_stack_files[i] = NULL;
        call_stack_lines[i] = 0;
        call_stack_columns[i] = 0;
    }
}

static void debug_append_json(char* buf, size_t buf_size, size_t* pos, const char* fmt, ...) {
    if (!buf || buf_size == 0 || !pos || *pos >= buf_size)
        return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, buf_size - *pos, fmt, args);
    va_end(args);
    if (n > 0) {
        size_t written = (size_t)n;
        if (written > buf_size - *pos)
            written = buf_size - *pos;
        *pos += written;
    }
}

static void build_debug_stack_json(const char* current_file, int current_line, int current_column, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t pos = 0;
    debug_append_json(out, out_size, &pos, "[");
    if (frame_depth <= 0) {
        char file_esc[512];
        char name_esc[64];
        json_escape(current_file ? current_file : "", file_esc, sizeof(file_esc));
        json_escape("main", name_esc, sizeof(name_esc));
        debug_append_json(out, out_size, &pos,
            "{\"name\":\"%s\",\"file\":\"%s\",\"line\":%d,\"column\":%d}",
            name_esc, file_esc, current_line, current_column
        );
    } else {
        for (int i = frame_depth - 1; i >= 0; i--) {
            char file_esc[512];
            char name_esc[128];
            json_escape(call_stack_files[i] ? call_stack_files[i] : "", file_esc, sizeof(file_esc));
            json_escape(call_stack_names[i] ? call_stack_names[i] : "function", name_esc, sizeof(name_esc));
            if (pos > 1)
                debug_append_json(out, out_size, &pos, ",");
            debug_append_json(out, out_size, &pos,
                "{\"name\":\"%s\",\"file\":\"%s\",\"line\":%d,\"column\":%d}",
                name_esc, file_esc, call_stack_lines[i], call_stack_columns[i]
            );
        }
    }
    debug_append_json(out, out_size, &pos, "]");
}

static void build_debug_locals_json(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t pos = 0;
    debug_append_json(out, out_size, &pos, "[");
    int base = current_frame_base();
    int count = 0;
    for (int i = base; i < var_count; i++) {
        if (!vars[i].name)
            continue;
        char name_esc[256];
        char value_raw[256];
        char value_esc[512];
        json_escape(vars[i].name, name_esc, sizeof(name_esc));
        debug_value_summary(vars[i].value, value_raw, sizeof(value_raw));
        json_escape(value_raw, value_esc, sizeof(value_esc));
        if (count > 0)
            debug_append_json(out, out_size, &pos, ",");
        debug_append_json(out, out_size, &pos,
            "{\"name\":\"%s\",\"type\":\"%s\",\"value\":\"%s\"}",
            name_esc,
            value_type_name(vars[i].value.type),
            value_esc
        );
        count++;
        if (count >= 200)
            break;
    }
    debug_append_json(out, out_size, &pos, "]");
}

static Expr* expr_clone(const Expr* src) {
    if (!src)
        return NULL;

    Expr* out = (Expr*)calloc_checked(1, sizeof(Expr));
    out->type = src->type;
    out->line = src->line;
    out->column = src->column;

    switch (src->type) {
        case EXPR_LITERAL:
        case EXPR_BOOL_LITERAL:
            out->int_value = src->int_value;
            break;
        case EXPR_FLOAT_LITERAL:
            out->float_value = src->float_value;
            break;
        case EXPR_STRING_LITERAL:
            out->string_value = heap_strdup_optional(src->string_value);
            break;
        case EXPR_VARIABLE:
            out->name = heap_strdup_optional(src->name);
            break;
        case EXPR_BINARY:
            out->op = src->op;
            out->left = expr_clone(src->left);
            out->right = expr_clone(src->right);
            break;
        case EXPR_UNARY:
            out->op = src->op;
            out->expr = expr_clone(src->expr);
            break;
        case EXPR_CAST:
            out->cast_type = src->cast_type;
            out->cast_expr = expr_clone(src->cast_expr);
            break;
        case EXPR_INPUT:
            out->input_prompt = heap_strdup_optional(src->input_prompt);
            break;
        case EXPR_BUILTIN:
            out->builtin_type = src->builtin_type;
            out->builtin_arg = expr_clone(src->builtin_arg);
            break;
        case EXPR_CHAR_AT:
            out->char_string = expr_clone(src->char_string);
            out->char_index = expr_clone(src->char_index);
            break;
        case EXPR_ARRAY_LITERAL:
            out->element_count = src->element_count;
            if (src->element_count > 0) {
                out->elements = (Expr**)malloc_checked(sizeof(Expr*) * (size_t)src->element_count);
                for (int i = 0; i < src->element_count; i++)
                    out->elements[i] = expr_clone(src->elements[i]);
            }
            break;
        case EXPR_ARRAY_INDEX:
            out->array_expr = expr_clone(src->array_expr);
            out->index_expr = expr_clone(src->index_expr);
            break;
        case EXPR_LIST:
            out->list_count = src->list_count;
            if (src->list_count > 0) {
                out->list_items = (Expr**)malloc_checked(sizeof(Expr*) * (size_t)src->list_count);
                for (int i = 0; i < src->list_count; i++)
                    out->list_items[i] = expr_clone(src->list_items[i]);
            }
            break;
        case EXPR_LIST_AT:
            out->list_expr = expr_clone(src->list_expr);
            out->list_index = expr_clone(src->list_index);
            break;
        case EXPR_INDEX_OF:
            out->index_value = expr_clone(src->index_value);
            out->index_array = expr_clone(src->index_array);
            break;
        case EXPR_DICT:
            out->dict_count = src->dict_count;
            if (src->dict_count > 0) {
                out->dict_keys = (Expr**)malloc_checked(sizeof(Expr*) * (size_t)src->dict_count);
                out->dict_values = (Expr**)malloc_checked(sizeof(Expr*) * (size_t)src->dict_count);
                for (int i = 0; i < src->dict_count; i++) {
                    out->dict_keys[i] = expr_clone(src->dict_keys[i]);
                    out->dict_values[i] = expr_clone(src->dict_values[i]);
                }
            }
            break;
        case EXPR_DICT_GET:
            out->dict_key = expr_clone(src->dict_key);
            out->dict_expr = expr_clone(src->dict_expr);
            break;
        case EXPR_CALL:
            out->call_name = heap_strdup_optional(src->call_name);
            out->call_arg_count = src->call_arg_count;
            if (src->call_arg_count > 0) {
                out->call_args = (Expr**)malloc_checked(sizeof(Expr*) * (size_t)src->call_arg_count);
                out->call_arg_names = (char**)calloc_checked((size_t)src->call_arg_count, sizeof(char*));
                for (int i = 0; i < src->call_arg_count; i++) {
                    out->call_args[i] = expr_clone(src->call_args[i]);
                    if (src->call_arg_names && src->call_arg_names[i])
                        out->call_arg_names[i] = heap_strdup_optional(src->call_arg_names[i]);
                }
            }
            break;
        case EXPR_LIST_ADD:
            out->list_add_name = heap_strdup_optional(src->list_add_name);
            out->list_add_value = expr_clone(src->list_add_value);
            break;
        case EXPR_LIST_COMPREHENSION:
            out->comp_var_name = heap_strdup_optional(src->comp_var_name);
            out->comp_iterable = expr_clone(src->comp_iterable);
            out->comp_filter = expr_clone(src->comp_filter);
            out->comp_result = expr_clone(src->comp_result);
            break;
        case EXPR_FILE_READ:
            out->file_read_path = expr_clone(src->file_read_path);
            break;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_ATOMIC_READ:
            out->llvl_target = expr_clone(src->llvl_target);
            break;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            out->llvl_target = expr_clone(src->llvl_target);
            out->llvl_index = expr_clone(src->llvl_index);
            break;
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            out->llvl_target = expr_clone(src->llvl_target);
            break;
        case EXPR_LLVL_OFFSET:
            out->llvl_target = expr_clone(src->llvl_target);
            out->llvl_index = expr_clone(src->llvl_index);
            break;
        case EXPR_LLVL_FIELD:
            out->name = heap_strdup_optional(src->name);
            out->llvl_target = expr_clone(src->llvl_target);
            break;
        default:
            break;
    }

    return out;
}

static void expr_free(Expr* expr) {
    if (!expr)
        return;

    switch (expr->type) {
        case EXPR_STRING_LITERAL:
            free(expr->string_value);
            break;
        case EXPR_VARIABLE:
            free(expr->name);
            break;
        case EXPR_BINARY:
            expr_free(expr->left);
            expr_free(expr->right);
            break;
        case EXPR_UNARY:
            expr_free(expr->expr);
            break;
        case EXPR_CAST:
            expr_free(expr->cast_expr);
            break;
        case EXPR_INPUT:
            free(expr->input_prompt);
            break;
        case EXPR_BUILTIN:
            expr_free(expr->builtin_arg);
            break;
        case EXPR_CHAR_AT:
            expr_free(expr->char_string);
            expr_free(expr->char_index);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->element_count; i++)
                expr_free(expr->elements[i]);
            free(expr->elements);
            break;
        case EXPR_ARRAY_INDEX:
            expr_free(expr->array_expr);
            expr_free(expr->index_expr);
            break;
        case EXPR_LIST:
            for (int i = 0; i < expr->list_count; i++)
                expr_free(expr->list_items[i]);
            free(expr->list_items);
            break;
        case EXPR_LIST_AT:
            expr_free(expr->list_expr);
            expr_free(expr->list_index);
            break;
        case EXPR_INDEX_OF:
            expr_free(expr->index_value);
            expr_free(expr->index_array);
            break;
        case EXPR_DICT:
            for (int i = 0; i < expr->dict_count; i++) {
                expr_free(expr->dict_keys[i]);
                expr_free(expr->dict_values[i]);
            }
            free(expr->dict_keys);
            free(expr->dict_values);
            break;
        case EXPR_DICT_GET:
            expr_free(expr->dict_key);
            expr_free(expr->dict_expr);
            break;
        case EXPR_CALL:
            free(expr->call_name);
            for (int i = 0; i < expr->call_arg_count; i++)
                expr_free(expr->call_args[i]);
            for (int i = 0; i < expr->call_arg_count; i++)
                free(expr->call_arg_names ? expr->call_arg_names[i] : NULL);
            free(expr->call_args);
            free(expr->call_arg_names);
            break;
        case EXPR_LIST_ADD:
            free(expr->list_add_name);
            expr_free(expr->list_add_value);
            break;
        case EXPR_LIST_COMPREHENSION:
            free(expr->comp_var_name);
            expr_free(expr->comp_iterable);
            expr_free(expr->comp_filter);
            expr_free(expr->comp_result);
            break;
        case EXPR_FILE_READ:
            expr_free(expr->file_read_path);
            break;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_ATOMIC_READ:
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            expr_free(expr->llvl_target);
            break;
        case EXPR_LLVL_OFFSET:
            expr_free(expr->llvl_target);
            expr_free(expr->llvl_index);
            break;
        case EXPR_LLVL_FIELD:
            free(expr->name);
            expr_free(expr->llvl_target);
            break;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            expr_free(expr->llvl_target);
            expr_free(expr->llvl_index);
            break;
        case EXPR_LITERAL:
        case EXPR_FLOAT_LITERAL:
        case EXPR_BOOL_LITERAL:
            break;
    }

    free(expr);
}

static StringPart* string_parts_clone(const StringPart* parts, int count) {
    if (!parts || count <= 0)
        return NULL;

    StringPart* out = (StringPart*)calloc_checked((size_t)count, sizeof(StringPart));
    for (int i = 0; i < count; i++) {
        out[i].type = parts[i].type;
        out[i].line = parts[i].line;
        out[i].column = parts[i].column;
        if (parts[i].type == STR_TEXT)
            out[i].text = heap_strdup_optional(parts[i].text);
        else
            out[i].expr = expr_clone(parts[i].expr);
    }

    return out;
}

static void string_parts_free(StringPart* parts, int count) {
    if (!parts || count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        if (parts[i].type == STR_TEXT)
            free(parts[i].text);
        else
            expr_free(parts[i].expr);
    }
    free(parts);
}

static ASTNode* ast_clone(const ASTNode* src) {
    if (!src)
        return NULL;

    ASTNode* out = (ASTNode*)calloc_checked(1, sizeof(ASTNode));
    out->type = src->type;
    out->line = src->line;
    out->column = src->column;

    switch (src->type) {
        case AST_SET:
            out->name = heap_strdup_optional(src->name);
            out->expr = expr_clone(src->expr);
            break;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
            out->expr = expr_clone(src->expr);
            break;
        case AST_PRINT_STRING:
            out->part_count = src->part_count;
            out->parts = string_parts_clone(src->parts, src->part_count);
            break;
        case AST_IF:
            out->branch_count = src->branch_count;
            out->branch_capacity = src->branch_count;
            if (src->branch_count > 0) {
                out->branches = (IfBranch*)calloc_checked((size_t)src->branch_count, sizeof(IfBranch));
                for (int i = 0; i < src->branch_count; i++) {
                    out->branches[i].condition = expr_clone(src->branches[i].condition);
                    out->branches[i].block = ast_clone(src->branches[i].block);
                }
            }
            out->else_block = ast_clone(src->else_block);
            break;
        case AST_TRY:
            out->try_stmt.try_block = ast_clone(src->try_stmt.try_block);
            out->try_stmt.otherwise_block = ast_clone(src->try_stmt.otherwise_block);
            break;
        case AST_BLOCK:
        case AST_PROGRAM:
        case AST_LLVL_BLOCK:
            out->count = src->count;
            out->body_capacity = src->count;
            if (src->count > 0) {
                out->body = (ASTNode**)malloc_checked(sizeof(ASTNode*) * (size_t)src->count);
                for (int i = 0; i < src->count; i++)
                    out->body[i] = ast_clone(src->body[i]);
            }
            break;
        case AST_LLVL_SAVE:
            out->llvl_save.name = heap_strdup_optional(src->llvl_save.name);
            out->llvl_save.size = expr_clone(src->llvl_save.size);
            out->llvl_save.has_type = src->llvl_save.has_type;
            out->llvl_save.elem_kind = src->llvl_save.elem_kind;
            out->llvl_save.elem_type_name = heap_strdup_optional(src->llvl_save.elem_type_name);
            break;
        case AST_LLVL_REMOVE:
            out->llvl_remove.name = heap_strdup_optional(src->llvl_remove.name);
            break;
        case AST_LLVL_RESIZE:
            out->llvl_resize.name = heap_strdup_optional(src->llvl_resize.name);
            out->llvl_resize.size = expr_clone(src->llvl_resize.size);
            out->llvl_resize.op = src->llvl_resize.op;
            out->llvl_resize.has_type = src->llvl_resize.has_type;
            out->llvl_resize.elem_kind = src->llvl_resize.elem_kind;
            out->llvl_resize.elem_type_name = heap_strdup_optional(src->llvl_resize.elem_type_name);
            break;
        case AST_LLVL_COPY:
            out->llvl_copy.src = heap_strdup_optional(src->llvl_copy.src);
            out->llvl_copy.dest = heap_strdup_optional(src->llvl_copy.dest);
            out->llvl_copy.size = expr_clone(src->llvl_copy.size);
            out->llvl_copy.has_size = src->llvl_copy.has_size;
            break;
        case AST_LLVL_MOVE:
            out->llvl_move.src = heap_strdup_optional(src->llvl_move.src);
            out->llvl_move.dest = heap_strdup_optional(src->llvl_move.dest);
            break;
        case AST_LLVL_SET_VALUE:
            out->llvl_set_value.name = heap_strdup_optional(src->llvl_set_value.name);
            out->llvl_set_value.value = expr_clone(src->llvl_set_value.value);
            break;
        case AST_LLVL_SET_BYTE:
            out->llvl_set_byte.name = heap_strdup_optional(src->llvl_set_byte.name);
            out->llvl_set_byte.index = expr_clone(src->llvl_set_byte.index);
            out->llvl_set_byte.value = expr_clone(src->llvl_set_byte.value);
            break;
        case AST_LLVL_BIT_OP:
            out->llvl_bit_op.name = heap_strdup_optional(src->llvl_bit_op.name);
            out->llvl_bit_op.index = expr_clone(src->llvl_bit_op.index);
            out->llvl_bit_op.op = src->llvl_bit_op.op;
            break;
        case AST_LLVL_SET_AT:
            out->llvl_set_at.address = expr_clone(src->llvl_set_at.address);
            out->llvl_set_at.value = expr_clone(src->llvl_set_at.value);
            break;
        case AST_LLVL_ATOMIC_OP:
            out->llvl_atomic_op.address = expr_clone(src->llvl_atomic_op.address);
            out->llvl_atomic_op.value = expr_clone(src->llvl_atomic_op.value);
            out->llvl_atomic_op.op = src->llvl_atomic_op.op;
            break;
        case AST_LLVL_MARK_VOLATILE:
            out->llvl_mark_volatile.target = expr_clone(src->llvl_mark_volatile.target);
            break;
        case AST_LLVL_SET_CHECK:
            out->llvl_set_check.kind = src->llvl_set_check.kind;
            out->llvl_set_check.enabled = src->llvl_set_check.enabled;
            break;
        case AST_LLVL_PORT_WRITE:
            out->llvl_port_write.port = expr_clone(src->llvl_port_write.port);
            out->llvl_port_write.value = expr_clone(src->llvl_port_write.value);
            break;
        case AST_LLVL_REGISTER_INTERRUPT:
            out->llvl_register_interrupt.interrupt_id = expr_clone(src->llvl_register_interrupt.interrupt_id);
            out->llvl_register_interrupt.handler_name = heap_strdup_optional(src->llvl_register_interrupt.handler_name);
            break;
        case AST_LLVL_SET_FIELD:
            out->llvl_set_field.field_name = heap_strdup_optional(src->llvl_set_field.field_name);
            out->llvl_set_field.target = expr_clone(src->llvl_set_field.target);
            out->llvl_set_field.value = expr_clone(src->llvl_set_field.value);
            break;
        case AST_LLVL_PIN_WRITE:
            out->llvl_pin_write.value = expr_clone(src->llvl_pin_write.value);
            out->llvl_pin_write.pin = expr_clone(src->llvl_pin_write.pin);
            break;
        case AST_LLVL_WAIT:
            out->llvl_wait.duration = expr_clone(src->llvl_wait.duration);
            break;
        case AST_LLVL_STRUCT_DECL:
            out->llvl_struct_decl.name = heap_strdup_optional(src->llvl_struct_decl.name);
            out->llvl_struct_decl.field_count = src->llvl_struct_decl.field_count;
            if (src->llvl_struct_decl.field_count > 0) {
                out->llvl_struct_decl.fields = (LlvlField*)calloc_checked((size_t)src->llvl_struct_decl.field_count, sizeof(LlvlField));
                for (int i = 0; i < src->llvl_struct_decl.field_count; i++) {
                    out->llvl_struct_decl.fields[i].name = heap_strdup_optional(src->llvl_struct_decl.fields[i].name);
                    out->llvl_struct_decl.fields[i].kind = src->llvl_struct_decl.fields[i].kind;
                    out->llvl_struct_decl.fields[i].type_name = heap_strdup_optional(src->llvl_struct_decl.fields[i].type_name);
                    out->llvl_struct_decl.fields[i].array_len = src->llvl_struct_decl.fields[i].array_len;
                }
            }
            break;
        case AST_LLVL_UNION_DECL:
            out->llvl_union_decl.name = heap_strdup_optional(src->llvl_union_decl.name);
            out->llvl_union_decl.field_count = src->llvl_union_decl.field_count;
            if (src->llvl_union_decl.field_count > 0) {
                out->llvl_union_decl.fields = (LlvlField*)calloc_checked((size_t)src->llvl_union_decl.field_count, sizeof(LlvlField));
                for (int i = 0; i < src->llvl_union_decl.field_count; i++) {
                    out->llvl_union_decl.fields[i].name = heap_strdup_optional(src->llvl_union_decl.fields[i].name);
                    out->llvl_union_decl.fields[i].kind = src->llvl_union_decl.fields[i].kind;
                    out->llvl_union_decl.fields[i].type_name = heap_strdup_optional(src->llvl_union_decl.fields[i].type_name);
                    out->llvl_union_decl.fields[i].array_len = src->llvl_union_decl.fields[i].array_len;
                }
            }
            break;
        case AST_LLVL_ENUM_DECL:
            out->llvl_enum_decl.name = heap_strdup_optional(src->llvl_enum_decl.name);
            out->llvl_enum_decl.count = src->llvl_enum_decl.count;
            if (src->llvl_enum_decl.count > 0) {
                out->llvl_enum_decl.names = (char**)calloc_checked((size_t)src->llvl_enum_decl.count, sizeof(char*));
                out->llvl_enum_decl.values = (int*)calloc_checked((size_t)src->llvl_enum_decl.count, sizeof(int));
                for (int i = 0; i < src->llvl_enum_decl.count; i++) {
                    out->llvl_enum_decl.names[i] = heap_strdup_optional(src->llvl_enum_decl.names[i]);
                    out->llvl_enum_decl.values[i] = src->llvl_enum_decl.values[i];
                }
            }
            break;
        case AST_LLVL_BITFIELD_DECL:
            out->llvl_bitfield_decl.name = heap_strdup_optional(src->llvl_bitfield_decl.name);
            out->llvl_bitfield_decl.count = src->llvl_bitfield_decl.count;
            if (src->llvl_bitfield_decl.count > 0) {
                out->llvl_bitfield_decl.names = (char**)calloc_checked((size_t)src->llvl_bitfield_decl.count, sizeof(char*));
                out->llvl_bitfield_decl.bits = (int*)calloc_checked((size_t)src->llvl_bitfield_decl.count, sizeof(int));
                for (int i = 0; i < src->llvl_bitfield_decl.count; i++) {
                    out->llvl_bitfield_decl.names[i] = heap_strdup_optional(src->llvl_bitfield_decl.names[i]);
                    out->llvl_bitfield_decl.bits[i] = src->llvl_bitfield_decl.bits[i];
                }
            }
            break;
        case AST_WHILE:
            out->while_stmt.condition = expr_clone(src->while_stmt.condition);
            out->while_stmt.repeat_limit = expr_clone(src->while_stmt.repeat_limit);
            out->while_stmt.body = ast_clone(src->while_stmt.body);
            break;
        case AST_REPEAT:
            out->repeat_stmt.times = expr_clone(src->repeat_stmt.times);
            out->repeat_stmt.body = ast_clone(src->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            out->for_each_stmt.item_name = heap_strdup_optional(src->for_each_stmt.item_name);
            out->for_each_stmt.iterable = expr_clone(src->for_each_stmt.iterable);
            out->for_each_stmt.body = ast_clone(src->for_each_stmt.body);
            break;
        case AST_MATCH:
            out->match_stmt.target = expr_clone(src->match_stmt.target);
            out->match_stmt.branch_count = src->match_stmt.branch_count;
            out->match_stmt.branch_capacity = src->match_stmt.branch_count;
            if (src->match_stmt.branch_count > 0) {
                out->match_stmt.branches = (MatchCaseBranch*)calloc_checked((size_t)src->match_stmt.branch_count, sizeof(MatchCaseBranch));
                for (int i = 0; i < src->match_stmt.branch_count; i++) {
                    out->match_stmt.branches[i].value = expr_clone(src->match_stmt.branches[i].value);
                    out->match_stmt.branches[i].block = ast_clone(src->match_stmt.branches[i].block);
                }
            }
            out->match_stmt.otherwise_block = ast_clone(src->match_stmt.otherwise_block);
            break;
        case AST_FUNCTION:
            out->function_decl.name = heap_strdup_optional(src->function_decl.name);
            out->function_decl.param_count = src->function_decl.param_count;
            out->function_decl.required_param_count = src->function_decl.required_param_count;
            if (src->function_decl.param_count > 0) {
                out->function_decl.params = (char**)malloc_checked(sizeof(char*) * (size_t)src->function_decl.param_count);
                out->function_decl.param_defaults = (Expr**)calloc_checked((size_t)src->function_decl.param_count, sizeof(Expr*));
                for (int i = 0; i < src->function_decl.param_count; i++) {
                    out->function_decl.params[i] = heap_strdup_optional(src->function_decl.params[i]);
                    if (src->function_decl.param_defaults)
                        out->function_decl.param_defaults[i] = expr_clone(src->function_decl.param_defaults[i]);
                }
            }
            out->function_decl.body = ast_clone(src->function_decl.body);
            break;
        case AST_CREATE_LIBRARY:
            out->library_decl.name = heap_strdup_optional(src->library_decl.name);
            break;
        case AST_LOAD_LIBRARY:
            out->library_load.name = heap_strdup_optional(src->library_load.name);
            break;
        case AST_LOAD_FILE:
            out->file_load.path = expr_clone(src->file_load.path);
            break;
        case AST_LIBRARY_OFFER:
            out->library_offer.name = heap_strdup_optional(src->library_offer.name);
            break;
        case AST_LIBRARY_TAKE:
            out->library_take.name = heap_strdup_optional(src->library_take.name);
            out->library_take.library_name = heap_strdup_optional(src->library_take.library_name);
            out->library_take.alias_name = heap_strdup_optional(src->library_take.alias_name);
            break;
        case AST_LIBRARY_TAKE_ALL:
            out->library_take_all.library_name = heap_strdup_optional(src->library_take_all.library_name);
            break;
        case AST_TYPE_DECL:
            out->type_decl.name = heap_strdup_optional(src->type_decl.name);
            out->type_decl.field_count = src->type_decl.field_count;
            if (src->type_decl.field_count > 0) {
                out->type_decl.fields = (char**)malloc_checked(sizeof(char*) * (size_t)src->type_decl.field_count);
                for (int i = 0; i < src->type_decl.field_count; i++)
                    out->type_decl.fields[i] = heap_strdup_optional(src->type_decl.fields[i]);
            }
            break;
        case AST_RETURN:
            out->return_stmt.value = expr_clone(src->return_stmt.value);
            break;
        case AST_FILE_WRITE:
        case AST_FILE_APPEND:
            out->file_io_stmt.path = expr_clone(src->file_io_stmt.path);
            out->file_io_stmt.content = expr_clone(src->file_io_stmt.content);
            break;
        case AST_EXIT:
        case AST_NEXT:
            break;
        case AST_SET_ELEMENT:
            out->set_element.name = heap_strdup_optional(src->set_element.name);
            out->set_element.index = expr_clone(src->set_element.index);
            out->set_element.value = expr_clone(src->set_element.value);
            break;
        case AST_LIST_ADD:
            out->list_add.list_name = heap_strdup_optional(src->list_add.list_name);
            out->list_add.value = expr_clone(src->list_add.value);
            break;
        case AST_LIST_REMOVE:
            out->list_remove.list_name = heap_strdup_optional(src->list_remove.list_name);
            out->list_remove.value = expr_clone(src->list_remove.value);
            break;
        case AST_LIST_REMOVE_ELEMENT:
            out->list_remove_element.list_name = heap_strdup_optional(src->list_remove_element.list_name);
            out->list_remove_element.index = expr_clone(src->list_remove_element.index);
            break;
        case AST_LIST_CLEAR:
            out->list_clear.list_name = heap_strdup_optional(src->list_clear.list_name);
            break;
        case AST_DICT_ADD:
            out->dict_add.dict_name = heap_strdup_optional(src->dict_add.dict_name);
            out->dict_add.key = expr_clone(src->dict_add.key);
            out->dict_add.value = expr_clone(src->dict_add.value);
            break;
        case AST_DICT_REMOVE:
            out->dict_remove.dict_name = heap_strdup_optional(src->dict_remove.dict_name);
            out->dict_remove.key = expr_clone(src->dict_remove.key);
            break;
        case AST_DICT_CONTAINS_ITEM:
            out->dict_contains.dict_name = heap_strdup_optional(src->dict_contains.dict_name);
            out->dict_contains.key = expr_clone(src->dict_contains.key);
            break;
        case AST_DICT_CLEAR:
            out->dict_clear.dict_name = heap_strdup_optional(src->dict_clear.dict_name);
            break;
        case AST_YIELD:
            out->expr = expr_clone(src->expr);
            break;
    }

    return out;
}

static void ast_free(ASTNode* node) {
    if (!node)
        return;

    switch (node->type) {
        case AST_SET:
            free(node->name);
            expr_free(node->expr);
            break;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
            expr_free(node->expr);
            break;
        case AST_PRINT_STRING:
            string_parts_free(node->parts, node->part_count);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++) {
                expr_free(node->branches[i].condition);
                ast_free(node->branches[i].block);
            }
            free(node->branches);
            ast_free(node->else_block);
            break;
        case AST_TRY:
            ast_free(node->try_stmt.try_block);
            ast_free(node->try_stmt.otherwise_block);
            break;
        case AST_BLOCK:
        case AST_PROGRAM:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++)
                ast_free(node->body[i]);
            free(node->body);
            break;
        case AST_WHILE:
            expr_free(node->while_stmt.condition);
            expr_free(node->while_stmt.repeat_limit);
            ast_free(node->while_stmt.body);
            break;
        case AST_REPEAT:
            expr_free(node->repeat_stmt.times);
            ast_free(node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            free(node->for_each_stmt.item_name);
            expr_free(node->for_each_stmt.iterable);
            ast_free(node->for_each_stmt.body);
            break;
        case AST_MATCH:
            expr_free(node->match_stmt.target);
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                expr_free(node->match_stmt.branches[i].value);
                ast_free(node->match_stmt.branches[i].block);
            }
            free(node->match_stmt.branches);
            ast_free(node->match_stmt.otherwise_block);
            break;
        case AST_FUNCTION:
            free(node->function_decl.name);
            if (node->function_decl.params) {
                for (int i = 0; i < node->function_decl.param_count; i++)
                    free(node->function_decl.params[i]);
                free(node->function_decl.params);
            }
            if (node->function_decl.param_defaults) {
                for (int i = 0; i < node->function_decl.param_count; i++)
                    expr_free(node->function_decl.param_defaults[i]);
                free(node->function_decl.param_defaults);
            }
            ast_free(node->function_decl.body);
            break;
        case AST_CREATE_LIBRARY:
            free(node->library_decl.name);
            break;
        case AST_LOAD_LIBRARY:
            free(node->library_load.name);
            break;
        case AST_LOAD_FILE:
            expr_free(node->file_load.path);
            break;
        case AST_LIBRARY_OFFER:
            free(node->library_offer.name);
            break;
        case AST_LIBRARY_TAKE:
            free(node->library_take.name);
            free(node->library_take.library_name);
            free(node->library_take.alias_name);
            break;
        case AST_LIBRARY_TAKE_ALL:
            free(node->library_take_all.library_name);
            break;
        case AST_TYPE_DECL:
            free(node->type_decl.name);
            if (node->type_decl.fields) {
                for (int i = 0; i < node->type_decl.field_count; i++)
                    free(node->type_decl.fields[i]);
                free(node->type_decl.fields);
            }
            break;
        case AST_RETURN:
            expr_free(node->return_stmt.value);
            break;
        case AST_FILE_WRITE:
        case AST_FILE_APPEND:
            expr_free(node->file_io_stmt.path);
            expr_free(node->file_io_stmt.content);
            break;
        case AST_LLVL_SAVE:
            free(node->llvl_save.name);
            expr_free(node->llvl_save.size);
            free(node->llvl_save.elem_type_name);
            break;
        case AST_LLVL_REMOVE:
            free(node->llvl_remove.name);
            break;
        case AST_LLVL_RESIZE:
            free(node->llvl_resize.name);
            expr_free(node->llvl_resize.size);
            free(node->llvl_resize.elem_type_name);
            break;
        case AST_LLVL_COPY:
            free(node->llvl_copy.src);
            free(node->llvl_copy.dest);
            expr_free(node->llvl_copy.size);
            break;
        case AST_LLVL_MOVE:
            free(node->llvl_move.src);
            free(node->llvl_move.dest);
            break;
        case AST_LLVL_SET_VALUE:
            free(node->llvl_set_value.name);
            expr_free(node->llvl_set_value.value);
            break;
        case AST_LLVL_SET_BYTE:
            free(node->llvl_set_byte.name);
            expr_free(node->llvl_set_byte.index);
            expr_free(node->llvl_set_byte.value);
            break;
        case AST_LLVL_BIT_OP:
            free(node->llvl_bit_op.name);
            expr_free(node->llvl_bit_op.index);
            break;
        case AST_LLVL_SET_AT:
            expr_free(node->llvl_set_at.address);
            expr_free(node->llvl_set_at.value);
            break;
        case AST_LLVL_ATOMIC_OP:
            expr_free(node->llvl_atomic_op.address);
            expr_free(node->llvl_atomic_op.value);
            break;
        case AST_LLVL_MARK_VOLATILE:
            expr_free(node->llvl_mark_volatile.target);
            break;
        case AST_LLVL_SET_CHECK:
            break;
        case AST_LLVL_PORT_WRITE:
            expr_free(node->llvl_port_write.port);
            expr_free(node->llvl_port_write.value);
            break;
        case AST_LLVL_REGISTER_INTERRUPT:
            expr_free(node->llvl_register_interrupt.interrupt_id);
            free(node->llvl_register_interrupt.handler_name);
            break;
        case AST_LLVL_SET_FIELD:
            free(node->llvl_set_field.field_name);
            expr_free(node->llvl_set_field.target);
            expr_free(node->llvl_set_field.value);
            break;
        case AST_LLVL_PIN_WRITE:
            expr_free(node->llvl_pin_write.value);
            expr_free(node->llvl_pin_write.pin);
            break;
        case AST_LLVL_WAIT:
            expr_free(node->llvl_wait.duration);
            break;
        case AST_LLVL_STRUCT_DECL:
            free(node->llvl_struct_decl.name);
            if (node->llvl_struct_decl.fields) {
                for (int i = 0; i < node->llvl_struct_decl.field_count; i++) {
                    free(node->llvl_struct_decl.fields[i].name);
                    free(node->llvl_struct_decl.fields[i].type_name);
                }
                free(node->llvl_struct_decl.fields);
            }
            break;
        case AST_LLVL_UNION_DECL:
            free(node->llvl_union_decl.name);
            if (node->llvl_union_decl.fields) {
                for (int i = 0; i < node->llvl_union_decl.field_count; i++) {
                    free(node->llvl_union_decl.fields[i].name);
                    free(node->llvl_union_decl.fields[i].type_name);
                }
                free(node->llvl_union_decl.fields);
            }
            break;
        case AST_LLVL_ENUM_DECL:
            free(node->llvl_enum_decl.name);
            if (node->llvl_enum_decl.names) {
                for (int i = 0; i < node->llvl_enum_decl.count; i++)
                    free(node->llvl_enum_decl.names[i]);
                free(node->llvl_enum_decl.names);
                free(node->llvl_enum_decl.values);
            }
            break;
        case AST_LLVL_BITFIELD_DECL:
            free(node->llvl_bitfield_decl.name);
            if (node->llvl_bitfield_decl.names) {
                for (int i = 0; i < node->llvl_bitfield_decl.count; i++)
                    free(node->llvl_bitfield_decl.names[i]);
                free(node->llvl_bitfield_decl.names);
                free(node->llvl_bitfield_decl.bits);
            }
            break;
        case AST_EXIT:
        case AST_NEXT:
            break;
        case AST_SET_ELEMENT:
            free(node->set_element.name);
            expr_free(node->set_element.index);
            expr_free(node->set_element.value);
            break;
        case AST_LIST_ADD:
            free(node->list_add.list_name);
            expr_free(node->list_add.value);
            break;
        case AST_LIST_REMOVE:
            free(node->list_remove.list_name);
            expr_free(node->list_remove.value);
            break;
        case AST_LIST_REMOVE_ELEMENT:
            free(node->list_remove_element.list_name);
            expr_free(node->list_remove_element.index);
            break;
        case AST_LIST_CLEAR:
            free(node->list_clear.list_name);
            break;
        case AST_DICT_ADD:
            free(node->dict_add.dict_name);
            expr_free(node->dict_add.key);
            expr_free(node->dict_add.value);
            break;
        case AST_DICT_REMOVE:
            free(node->dict_remove.dict_name);
            expr_free(node->dict_remove.key);
            break;
        case AST_DICT_CONTAINS_ITEM:
            free(node->dict_contains.dict_name);
            expr_free(node->dict_contains.key);
            break;
        case AST_DICT_CLEAR:
            free(node->dict_clear.dict_name);
            break;
        case AST_YIELD:
            expr_free(node->expr);
            break;
    }

    free(node);
}

static int current_frame_base(void) {
    if (frame_depth <= 0)
        return 0;
    return frame_bases[frame_depth - 1];
}

static int global_var_end(void) {
    if (frame_depth <= 0)
        return var_count;
    int end = frame_bases[0];
    if (end < 0)
        return 0;
    if (end > var_count)
        return var_count;
    return end;
}

static Var* find_var_in_range(const char* name, int start, int end_exclusive) {
    for (int i = end_exclusive - 1; i >= start; i--) {
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }
    return NULL;
}

static Var* find_var(const char* name) {
    int base = current_frame_base();

    if (frame_depth > 0) {
        Var* local = find_var_in_range(name, base, var_count);
        if (local)
            return local;
        return find_var_in_range(name, 0, global_var_end());
    }

    return find_var_in_range(name, 0, var_count);
}

static Var* find_global_var(const char* name) {
    return find_var_in_range(name, 0, global_var_end());
}

static Var* find_local_var(const char* name) {
    if (frame_depth <= 0)
        return find_var(name);
    return find_var_in_range(name, current_frame_base(), var_count);
}

static int find_var_index_in_range(const char* name, int start, int end_exclusive) {
    for (int i = end_exclusive - 1; i >= start; i--) {
        if (strcmp(vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int find_binding_target_index(const char* name) {
    if (frame_depth > 0)
        return find_var_index_in_range(name, current_frame_base(), var_count);
    return find_var_index_in_range(name, 0, var_count);
}

typedef struct {
    int had_existing;
    int existing_index;
    Value saved_value;
    int created_index;
} TempBinding;

static Var* add_var(const char* name, Value value) {
    ensure_var_capacity();
    Var* v = &vars[var_count++];
    v->name = sicht_strdup_checked(name);
    v->value = value;
    return v;
}

static void cleanup_locals_from(int base) {
    for (int i = base; i < var_count; i++) {
        free(vars[i].name);
        vars[i].name = NULL;
    }
    var_count = base;
}

static void temp_binding_begin(const char* name, TempBinding* binding) {
    int idx = find_binding_target_index(name);
    if (idx >= 0) {
        binding->had_existing = 1;
        binding->existing_index = idx;
        binding->saved_value = vars[idx].value;
        binding->created_index = -1;
        return;
    }

    binding->had_existing = 0;
    binding->existing_index = -1;
    binding->saved_value = value_int(0);
    binding->created_index = var_count;
    add_var(name, value_int(0));
}

static void temp_binding_set(const TempBinding* binding, Value value) {
    if (binding->had_existing) {
        vars[binding->existing_index].value = value;
        return;
    }

    vars[binding->created_index].value = value;
}

static void temp_binding_end(const TempBinding* binding) {
    if (binding->had_existing) {
        vars[binding->existing_index].value = binding->saved_value;
        return;
    }

    int idx = binding->created_index;
    free(vars[idx].name);

    for (int i = idx; i < var_count - 1; i++)
        vars[i] = vars[i + 1];

    var_count--;
}

static int function_is_accessible(const FunctionDef* fn) {
    if (!fn->owner_library)
        return 1;

    if (fn->imported)
        return 1;

    if (active_function_library && strcmp(active_function_library, fn->owner_library) == 0)
        return 1;

    if (current_loading_library && strcmp(current_loading_library, fn->owner_library) == 0)
        return 1;

    return 0;
}

static FunctionDef* find_function_any(const char* name) {
    for (int i = 0; i < function_count; i++) {
        if (strcmp(functions[i].name, name) == 0)
            return &functions[i];
    }
    return NULL;
}

static FunctionDef* find_function_in_library(const char* library_name, const char* name) {
    for (int i = 0; i < function_count; i++) {
        FunctionDef* fn = &functions[i];
        if (!fn->owner_library)
            continue;
        if (strcmp(fn->owner_library, library_name) != 0)
            continue;
        if (strcmp(fn->name, name) == 0)
            return fn;
    }
    return NULL;
}

static FunctionDef* find_function(const char* name) {
    for (int i = 0; i < function_count; i++) {
        FunctionDef* fn = &functions[i];
        if (strcmp(fn->name, name) != 0)
            continue;
        if (function_is_accessible(fn))
            return fn;
    }
    return NULL;
}

static TypeDef* find_type(const char* name) {
    for (int i = 0; i < type_count; i++) {
        if (strcmp(types[i].name, name) == 0)
            return &types[i];
    }
    return NULL;
}

static LlvlTypeDef* llvl_find_type(const char* name) {
    if (!name)
        return NULL;
    for (int i = 0; i < llvl_type_count; i++) {
        if (strcmp(llvl_types[i].name, name) == 0)
            return &llvl_types[i];
    }
    return NULL;
}

static BufferElemKind llvl_kind_to_buffer_kind(LlvlTypeKind kind) {
    switch (kind) {
        case LLVL_TYPE_BYTE: return BUFFER_ELEM_BYTE;
        case LLVL_TYPE_INT: return BUFFER_ELEM_INT;
        case LLVL_TYPE_FLOAT: return BUFFER_ELEM_FLOAT;
        case LLVL_TYPE_STRUCT: return BUFFER_ELEM_STRUCT;
        case LLVL_TYPE_UNION: return BUFFER_ELEM_UNION;
    }
    return BUFFER_ELEM_RAW;
}

static size_t llvl_type_size_from_kind(LlvlTypeKind kind, const char* type_name) {
    switch (kind) {
        case LLVL_TYPE_BYTE: return 1;
        case LLVL_TYPE_INT: return 4;
        case LLVL_TYPE_FLOAT: return 8;
        case LLVL_TYPE_STRUCT:
        case LLVL_TYPE_UNION: {
            LlvlTypeDef* def = llvl_find_type(type_name);
            if (!def) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Unknown low-level type `%s`.", type_name ? type_name : "<null>");
                runtime_error("Unknown low-level type", msg, "Define the struct/union before using it.");
            }
            return def->size;
        }
    }
    return 0;
}

static void llvl_register_struct_decl(const ASTNode* node, int is_union) {
    if (!node)
        return;
    const char* name = is_union ? node->llvl_union_decl.name : node->llvl_struct_decl.name;
    int field_count = is_union ? node->llvl_union_decl.field_count : node->llvl_struct_decl.field_count;
    LlvlField* fields = is_union ? node->llvl_union_decl.fields : node->llvl_struct_decl.fields;

    if (!name)
        runtime_error("Invalid low-level type", "Missing type name.", "");

    if (llvl_find_type(name)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Low-level type `%s` already exists.", name);
        runtime_error("Duplicate low-level type", msg, "");
    }

    if (llvl_type_count >= MAX_LLVL_TYPES)
        runtime_error("Too many low-level types", "Increase MAX_LLVL_TYPES.", "");

    LlvlTypeDef* def = &llvl_types[llvl_type_count++];
    def->name = heap_strdup_optional(name);
    def->is_union = is_union ? 1 : 0;
    def->field_count = field_count;
    def->fields = NULL;
    def->size = 0;

    if (field_count <= 0 || !fields)
        return;

    def->fields = (LlvlFieldDef*)calloc_checked((size_t)field_count, sizeof(LlvlFieldDef));
    size_t offset = 0;
    size_t max_size = 0;

    for (int i = 0; i < field_count; i++) {
        def->fields[i].name = heap_strdup_optional(fields[i].name);
        def->fields[i].kind = fields[i].kind;
        def->fields[i].type_name = heap_strdup_optional(fields[i].type_name);
        def->fields[i].array_len = fields[i].array_len;

        size_t base_size = llvl_type_size_from_kind(fields[i].kind, fields[i].type_name);
        size_t field_size = base_size;
        if (fields[i].array_len > 0) {
            if (base_size > 0 && (size_t)fields[i].array_len > SIZE_MAX / base_size)
                runtime_error("Struct size overflow", "Array field is too large.", "");
            field_size = base_size * (size_t)fields[i].array_len;
        }

        def->fields[i].size = field_size;
        def->fields[i].offset = is_union ? 0 : offset;

        if (!is_union) {
            if (offset > SIZE_MAX - field_size)
                runtime_error("Struct size overflow", "Struct size is too large.", "");
            offset += field_size;
        } else if (field_size > max_size) {
            max_size = field_size;
        }
    }

    def->size = is_union ? max_size : offset;
}

static void llvl_register_enum_decl(const ASTNode* node) {
    if (!node)
        return;
    for (int i = 0; i < node->llvl_enum_decl.count; i++) {
        if (!node->llvl_enum_decl.names[i])
            continue;
        if (find_var(node->llvl_enum_decl.names[i])) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Enum name `%s` conflicts with existing variable.", node->llvl_enum_decl.names[i]);
            runtime_error("Enum conflict", msg, "");
        }
        add_var(node->llvl_enum_decl.names[i], value_int(node->llvl_enum_decl.values[i]));
    }
}

static void llvl_register_bitfield_decl(const ASTNode* node) {
    if (!node)
        return;
    for (int i = 0; i < node->llvl_bitfield_decl.count; i++) {
        if (!node->llvl_bitfield_decl.names[i])
            continue;
        if (find_var(node->llvl_bitfield_decl.names[i])) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Bitfield name `%s` conflicts with existing variable.", node->llvl_bitfield_decl.names[i]);
            runtime_error("Bitfield conflict", msg, "");
        }
        add_var(node->llvl_bitfield_decl.names[i], value_int(node->llvl_bitfield_decl.bits[i]));
    }
}

static int dict_find_index(Dict* dict, const char* key) {
    return gc_dict_find_index(dict, key);
}

static void dict_set_entry_gc(Dict* dict, const char* key, Value value) {
    gc_dict_set_key(dict, key, value, 1);
}

static void dict_set_entry_raw(Dict* dict, const char* key, Value value) {
    gc_dict_set_key(dict, key, value, 0);
}

static int dict_remove_entry(Dict* dict, const char* key) {
    return gc_dict_remove(dict, key);
}

static void dict_clear_entries(Dict* dict) {
    gc_dict_clear(dict);
}

#define MAX_DEEP_EQUALS_DEPTH 64

static int value_equals_internal(Value a, Value b, int depth) {
    if (a.type != b.type)
        return 0;

    switch (a.type) {
        case VALUE_INT:
        case VALUE_BOOL:
            return a.int_value == b.int_value;
        case VALUE_FLOAT:
            return a.float_value == b.float_value;
        case VALUE_STRING:
            return strcmp(a.string_value, b.string_value) == 0;
        case VALUE_LIST: {
            if (a.list_value == b.list_value)
                return 1;
            if (!a.list_value || !b.list_value)
                return 0;
            if (depth > MAX_DEEP_EQUALS_DEPTH)
                return 0;
            if (a.list_value->count != b.list_value->count)
                return 0;
            for (int i = 0; i < a.list_value->count; i++) {
                if (!value_equals_internal(a.list_value->items[i], b.list_value->items[i], depth + 1))
                    return 0;
            }
            return 1;
        }
        case VALUE_DICT: {
            if (a.dict_value == b.dict_value)
                return 1;
            if (!a.dict_value || !b.dict_value)
                return 0;
            if (depth > MAX_DEEP_EQUALS_DEPTH)
                return 0;
            if (a.dict_value->count != b.dict_value->count)
                return 0;
            for (int i = 0; i < a.dict_value->count; i++) {
                const char* key = a.dict_value->entries[i].key;
                if (!key)
                    return 0;
                int idx = dict_find_index(b.dict_value, key);
                if (idx < 0)
                    return 0;
                if (!value_equals_internal(a.dict_value->entries[i].value, b.dict_value->entries[idx].value, depth + 1))
                    return 0;
            }
            return 1;
        }
        case VALUE_GENERATOR:
            return a.generator_value == b.generator_value;
        case VALUE_BUFFER:
            return a.buffer_value == b.buffer_value;
        case VALUE_ADDRESS:
            return a.address_value == b.address_value;
    }

    return 0;
}

static int value_equals(Value a, Value b) {
    return value_equals_internal(a, b, 0);
}

static int value_bool_or_error(Value v, const char* title) {
    if (v.type != VALUE_BOOL)
        runtime_error(title, "Condition must be boolean.", "");
    return v.int_value ? 1 : 0;
}

static size_t llvl_size_from_value(Value v, const char* title) {
    double d = 0.0;
    if (v.type == VALUE_INT)
        d = (double)v.int_value;
    else if (v.type == VALUE_FLOAT)
        d = v.float_value;
    else if (v.type == VALUE_BOOL)
        d = v.int_value ? 1.0 : 0.0;
    else
        runtime_error(title, "Size must be an integer, float, or boolean.", "");

    if (!(d >= 0.0))
        runtime_error(title, "Size must be non-negative.", "");
    if (d > (double)SIZE_MAX)
        runtime_error(title, "Size is too large.", "");
    return (size_t)d;
}

static int size_to_int_or_error(size_t n, const char* title, const char* message) {
    if (n > (size_t)INT_MAX)
        runtime_error(title, message, "");
    return (int)n;
}

static void list_append(List* list, Value value) {
    gc_list_reserve(list, list->count + 1);
    list->items[list->count++] = value;
}

static int compare_int_values(const void* a, const void* b) {
    const Value* va = (const Value*)a;
    const Value* vb = (const Value*)b;
    if (va->int_value < vb->int_value)
        return -1;
    if (va->int_value > vb->int_value)
        return 1;
    return 0;
}

static int value_collection_count(Value source) {
    if (source.type == VALUE_LIST)
        return source.list_value->count;
    if (source.type == VALUE_DICT)
        return source.dict_value->count;
    if (source.type == VALUE_STRING)
        return size_to_int_or_error(
            strlen(source.string_value),
            "String too large",
            "String length exceeds supported integer range."
        );
    if (source.type == VALUE_GENERATOR) {
        materialize_generator(source.generator_value);
        if (!source.generator_value->cache)
            return 0;
        return source.generator_value->cache->count - source.generator_value->index;
    }
    return -1;
}

static Value value_collection_item(Value source, int index) {
    if (source.type == VALUE_LIST)
        return source.list_value->items[index];
    if (source.type == VALUE_DICT)
        return value_string(source.dict_value->entries[index].key);
    if (source.type == VALUE_STRING) {
        char buf[2];
        buf[0] = source.string_value[index];
        buf[1] = '\0';
        return value_string(buf);
    }
    if (source.type == VALUE_GENERATOR) {
        Generator* g = source.generator_value;
        materialize_generator(g);
        return g->cache->items[g->index + index];
    }

    runtime_error("Invalid iterable", "Target must be a list, dictionary, string, or generator.", "");
    return value_int(0);
}

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf* sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char*)malloc_checked(sb->cap);
    sb->buf[0] = '\0';
}

static void sb_ensure(StrBuf* sb, size_t needed) {
    if (needed <= sb->cap)
        return;
    size_t new_cap = sb->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            runtime_error("Out of memory", "String buffer exceeded supported size.", "");
        new_cap *= 2;
    }
    char* grown = (char*)realloc(sb->buf, new_cap);
    if (!grown)
        runtime_error("Out of memory", "Could not grow string buffer.", "");
    sb->buf = grown;
    sb->cap = new_cap;
}

static void sb_append(StrBuf* sb, const char* text) {
    if (!text)
        return;
    size_t text_len = strlen(text);
    if (text_len == 0)
        return;
    sb_ensure(sb, sb->len + text_len + 1);
    memcpy(sb->buf + sb->len, text, text_len);
    sb->len += text_len;
    sb->buf[sb->len] = '\0';
}

static void sb_append_char(StrBuf* sb, char ch) {
    sb_ensure(sb, sb->len + 2);
    sb->buf[sb->len++] = ch;
    sb->buf[sb->len] = '\0';
}

static void sb_append_value(StrBuf* sb, Value v, int depth) {
    const int max_depth = 16;
    if (depth > max_depth) {
        sb_append(sb, "<...>");
        return;
    }

    switch (v.type) {
        case VALUE_STRING:
            sb_append(sb, v.string_value ? v.string_value : "");
            return;
        case VALUE_BOOL:
            sb_append(sb, v.int_value ? "true" : "false");
            return;
        case VALUE_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", v.float_value);
            sb_append(sb, buf);
            return;
        }
        case VALUE_INT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d", v.int_value);
            sb_append(sb, buf);
            return;
        }
        case VALUE_BUFFER: {
            char buf[64];
            size_t size = v.buffer_value ? v.buffer_value->size : 0;
            snprintf(buf, sizeof(buf), "<buffer %zu bytes>", size);
            sb_append(sb, buf);
            return;
        }
        case VALUE_ADDRESS: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<address %p>", v.address_value);
            sb_append(sb, buf);
            return;
        }
        case VALUE_LIST: {
            sb_append_char(sb, '[');
            if (v.list_value) {
                for (int i = 0; i < v.list_value->count; i++) {
                    sb_append_value(sb, v.list_value->items[i], depth + 1);
                    if (i < v.list_value->count - 1)
                        sb_append(sb, ", ");
                }
            }
            sb_append_char(sb, ']');
            return;
        }
        case VALUE_DICT: {
            sb_append_char(sb, '{');
            if (v.dict_value) {
                for (int i = 0; i < v.dict_value->count; i++) {
                    DictEntry* e = &v.dict_value->entries[i];
                    sb_append_char(sb, '"');
                    sb_append(sb, e->key ? e->key : "");
                    sb_append(sb, "\": ");
                    sb_append_value(sb, e->value, depth + 1);
                    if (i < v.dict_value->count - 1)
                        sb_append(sb, ", ");
                }
            }
            sb_append_char(sb, '}');
            return;
        }
        case VALUE_GENERATOR: {
            Generator* g = v.generator_value;
            if (!g) {
                sb_append(sb, "<generator:null>");
                return;
            }
            char buf[128];
            if (g->initialized && g->cache)
                snprintf(buf, sizeof(buf), "<generator index=%d remaining=%d>", g->index, g->cache->count - g->index);
            else
                snprintf(buf, sizeof(buf), "<generator index=%d>", g->index);
            sb_append(sb, buf);
            return;
        }
    }
}

static Value stringify_value(Value v) {
    StrBuf sb;
    sb_init(&sb);
    sb_append_value(&sb, v, 0);
    Value out = value_string(sb.buf);
    free(sb.buf);
    return out;
}

static Value cast_to_string_value(Value v) {
    if (v.type == VALUE_STRING)
        return v;
    if (v.type == VALUE_INT) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d", v.int_value);
        return value_string(buf);
    }
    if (v.type == VALUE_FLOAT) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", v.float_value);
        return value_string(buf);
    }
    if (v.type == VALUE_BOOL)
        return value_string(v.int_value ? "true" : "false");
    return stringify_value(v);
}

static char* find_last_substring(char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0')
        return NULL;

    char* found = NULL;
    char* cur = haystack;
    size_t needle_len = strlen(needle);
    while ((cur = strstr(cur, needle)) != NULL) {
        found = cur;
        cur += needle_len;
    }
    return found;
}

static int http_method_is_safe(const char* text) {
    if (!text || text[0] == '\0')
        return 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (!(*p >= 'A' && *p <= 'Z'))
            return 0;
    }
    return 1;
}

static int http_url_is_safe(const char* text) {
    if (!text || text[0] == '\0')
        return 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < 32 || *p == 127)
            return 0;
    }
    return 1;
}

static int http_header_token_is_safe(const char* text) {
    if (!text || text[0] == '\0')
        return 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

static int http_header_value_is_safe(const char* text) {
    if (!text)
        return 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < 32 || *p == 127)
            return 0;
        if (*p == '\r' || *p == '\n')
            return 0;
    }
    return 1;
}

#if !defined(_WIN32) && defined(SICHT_LIBCURL_ENABLED)
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} CurlBuffer;

static int curl_global_ready = 0;

static int ensure_curl_global(char* err, size_t err_size) {
    if (curl_global_ready)
        return 1;
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        snprintf(err, err_size, "libcurl init failed: %s", curl_easy_strerror(rc));
        return 0;
    }
    curl_global_ready = 1;
    return 1;
}

static size_t curl_write_buffer(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CurlBuffer* buf = (CurlBuffer*)userdata;
    size_t total = size * nmemb;
    if (!buf || total == 0)
        return 0;
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : 4096;
        while (new_cap < needed)
            new_cap *= 2;
        char* grown = (char*)realloc(buf->data, new_cap);
        if (!grown)
            return 0;
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static size_t curl_write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* file = (FILE*)userdata;
    if (!file)
        return 0;
    size_t written = fwrite(ptr, size, nmemb, file);
    return written * size;
}

static int run_http_libcurl_request(const char* method, const char* url, const char* body, Value headers, char** out_body, int* out_status, char* err, size_t err_size) {
    if (!method || !url || !out_body || !out_status || !err || err_size == 0)
        return 0;
    err[0] = '\0';
    *out_body = NULL;
    *out_status = 0;

    if (!ensure_curl_global(err, err_size))
        return 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_size, "libcurl init failed.");
        return 0;
    }

    CurlBuffer buf;
    buf.data = NULL;
    buf.len = 0;
    buf.cap = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Sicht/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    if (method && method[0]) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    const char* body_text = body ? body : "";
    if (body_text[0] != '\0' && strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_text);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body_text));
    }

    struct curl_slist* header_list = NULL;
    if (headers.type == VALUE_DICT) {
        Dict* dict = headers.dict_value;
        if (dict) {
            for (int i = 0; i < dict->count; i++) {
                DictEntry* entry = &dict->entries[i];
                const char* key = entry->key ? entry->key : "";
                Value v = entry->value;
                if (v.type != VALUE_STRING) {
                    snprintf(err, err_size, "HTTP header values must be strings.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    free(buf.data);
                    return 0;
                }
                const char* value = v.string_value ? v.string_value : "";
                if (!http_header_token_is_safe(key) || !http_header_value_is_safe(value)) {
                    snprintf(err, err_size, "HTTP headers contain unsupported characters.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    free(buf.data);
                    return 0;
                }
                char header_line[1024];
                int wrote = snprintf(header_line, sizeof(header_line), "%s: %s", key, value);
                if (wrote <= 0 || (size_t)wrote >= sizeof(header_line)) {
                    snprintf(err, err_size, "HTTP headers are too large.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    free(buf.data);
                    return 0;
                }
                header_list = curl_slist_append(header_list, header_line);
            }
        }
    }
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(err, err_size, "libcurl request failed: %s", curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        curl_slist_free_all(header_list);
        free(buf.data);
        return 0;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    *out_status = (int)status;

    curl_easy_cleanup(curl);
    curl_slist_free_all(header_list);

    if (!buf.data)
        buf.data = sicht_strdup_checked("");
    *out_body = buf.data;
    return 1;
}

static int run_http_libcurl_download(const char* url, Value headers, const char* output_path, int* out_status, char* err, size_t err_size) {
    if (!url || !output_path || !out_status || !err || err_size == 0)
        return 0;
    err[0] = '\0';
    *out_status = 0;

    if (!ensure_curl_global(err, err_size))
        return 0;

    FILE* file = fopen(output_path, "wb");
    if (!file) {
        snprintf(err, err_size, "Could not open output file.");
        return 0;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        snprintf(err, err_size, "libcurl init failed.");
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Sicht/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    struct curl_slist* header_list = NULL;
    if (headers.type == VALUE_DICT) {
        Dict* dict = headers.dict_value;
        if (dict) {
            for (int i = 0; i < dict->count; i++) {
                DictEntry* entry = &dict->entries[i];
                const char* key = entry->key ? entry->key : "";
                Value v = entry->value;
                if (v.type != VALUE_STRING) {
                    snprintf(err, err_size, "HTTP header values must be strings.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    fclose(file);
                    return 0;
                }
                const char* value = v.string_value ? v.string_value : "";
                if (!http_header_token_is_safe(key) || !http_header_value_is_safe(value)) {
                    snprintf(err, err_size, "HTTP headers contain unsupported characters.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    fclose(file);
                    return 0;
                }
                char header_line[1024];
                int wrote = snprintf(header_line, sizeof(header_line), "%s: %s", key, value);
                if (wrote <= 0 || (size_t)wrote >= sizeof(header_line)) {
                    snprintf(err, err_size, "HTTP headers are too large.");
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(header_list);
                    fclose(file);
                    return 0;
                }
                header_list = curl_slist_append(header_list, header_line);
            }
        }
    }
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(err, err_size, "libcurl download failed: %s", curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        curl_slist_free_all(header_list);
        fclose(file);
        return 0;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    *out_status = (int)status;

    curl_easy_cleanup(curl);
    curl_slist_free_all(header_list);
    fclose(file);
    return 1;
}
#endif

#ifdef _WIN32
static void winhttp_set_error(char* err, size_t err_size, const char* prefix) {
    if (!err || err_size == 0)
        return;
    DWORD code = GetLastError();
    char msg[256];
    msg[0] = '\0';
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        0,
        msg,
        (DWORD)sizeof(msg),
        NULL
    );
    size_t len = strlen(msg);
    while (len > 0) {
        char c = msg[len - 1];
        if (c != '\r' && c != '\n' && c != ' ')
            break;
        msg[--len] = '\0';
    }
    if (prefix && prefix[0])
        snprintf(err, err_size, "%s (winhttp %lu) %s", prefix, (unsigned long)code, msg);
    else
        snprintf(err, err_size, "WinHTTP error %lu %s", (unsigned long)code, msg);
}

static wchar_t* winhttp_utf8_to_wide(const char* text, char* err, size_t err_size) {
    if (!text)
        return NULL;
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (needed <= 0) {
        winhttp_set_error(err, err_size, "UTF-8 decode failed");
        return NULL;
    }
    wchar_t* out = (wchar_t*)malloc((size_t)needed * sizeof(wchar_t));
    if (!out) {
        snprintf(err, err_size, "Out of memory while converting text.");
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, needed)) {
        winhttp_set_error(err, err_size, "UTF-8 decode failed");
        free(out);
        return NULL;
    }
    return out;
}

static int winhttp_split_url(const char* url, wchar_t** out_host, wchar_t** out_path, INTERNET_PORT* out_port, DWORD* out_flags, char* err, size_t err_size) {
    if (!url || !out_host || !out_path || !out_port || !out_flags)
        return 0;

    *out_host = NULL;
    *out_path = NULL;
    *out_port = 0;
    *out_flags = 0;

    wchar_t* url_w = winhttp_utf8_to_wide(url, err, err_size);
    if (!url_w)
        return 0;

    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(url_w, 0, 0, &parts)) {
        winhttp_set_error(err, err_size, "Failed to parse URL");
        free(url_w);
        return 0;
    }

    if (parts.nScheme == INTERNET_SCHEME_HTTPS)
        *out_flags = WINHTTP_FLAG_SECURE;
    *out_port = parts.nPort;

    DWORD host_len = parts.dwHostNameLength;
    if (host_len == 0 || !parts.lpszHostName) {
        snprintf(err, err_size, "URL missing host.");
        free(url_w);
        return 0;
    }
    wchar_t* host = (wchar_t*)malloc(((size_t)host_len + 1) * sizeof(wchar_t));
    if (!host) {
        snprintf(err, err_size, "Out of memory while parsing URL.");
        free(url_w);
        return 0;
    }
    memcpy(host, parts.lpszHostName, host_len * sizeof(wchar_t));
    host[host_len] = L'\0';

    DWORD path_len = parts.dwUrlPathLength;
    DWORD extra_len = parts.dwExtraInfoLength;
    if (path_len == 0 && extra_len == 0) {
        wchar_t* path = (wchar_t*)malloc(2 * sizeof(wchar_t));
        if (!path) {
            snprintf(err, err_size, "Out of memory while parsing URL.");
            free(host);
            free(url_w);
            return 0;
        }
        path[0] = L'/';
        path[1] = L'\0';
        *out_host = host;
        *out_path = path;
        free(url_w);
        return 1;
    }

    DWORD total = path_len + extra_len;
    wchar_t* path = (wchar_t*)malloc(((size_t)total + 1) * sizeof(wchar_t));
    if (!path) {
        snprintf(err, err_size, "Out of memory while parsing URL.");
        free(host);
        free(url_w);
        return 0;
    }
    DWORD offset = 0;
    if (path_len > 0 && parts.lpszUrlPath) {
        memcpy(path + offset, parts.lpszUrlPath, path_len * sizeof(wchar_t));
        offset += path_len;
    }
    if (extra_len > 0 && parts.lpszExtraInfo) {
        memcpy(path + offset, parts.lpszExtraInfo, extra_len * sizeof(wchar_t));
        offset += extra_len;
    }
    path[offset] = L'\0';

    *out_host = host;
    *out_path = path;
    free(url_w);
    return 1;
}

static void winhttp_enable_tls(HINTERNET session) {
    if (!session)
        return;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
}

static void winhttp_apply_proxy(HINTERNET session, const char* url) {
    if (!session || !url || !url[0])
        return;

    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie = {0};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie))
        return;

    WINHTTP_PROXY_INFO proxy = {0};
    if (ie.lpszProxy) {
        proxy.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy.lpszProxy = ie.lpszProxy;
        proxy.lpszProxyBypass = ie.lpszProxyBypass;
        WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &proxy, sizeof(proxy));
    } else if (ie.lpszAutoConfigUrl || ie.fAutoDetect) {
        WINHTTP_AUTOPROXY_OPTIONS opt = {0};
        opt.fAutoLogonIfChallenged = TRUE;
        if (ie.lpszAutoConfigUrl) {
            opt.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
            opt.lpszAutoConfigUrl = ie.lpszAutoConfigUrl;
        } else {
            opt.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
            opt.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        }
        wchar_t* url_w = winhttp_utf8_to_wide(url, NULL, 0);
        if (url_w) {
            if (WinHttpGetProxyForUrl(session, url_w, &opt, &proxy)) {
                WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &proxy, sizeof(proxy));
            }
            free(url_w);
        }
        if (proxy.lpszProxy)
            GlobalFree(proxy.lpszProxy);
        if (proxy.lpszProxyBypass)
            GlobalFree(proxy.lpszProxyBypass);
    }

    if (ie.lpszProxy)
        GlobalFree(ie.lpszProxy);
    if (ie.lpszProxyBypass)
        GlobalFree(ie.lpszProxyBypass);
    if (ie.lpszAutoConfigUrl)
        GlobalFree(ie.lpszAutoConfigUrl);
}

static int winhttp_add_headers(HINTERNET request, Value headers, char* err, size_t err_size) {
    if (headers.type != VALUE_DICT)
        return 1;
    Dict* dict = headers.dict_value;
    if (!dict || dict->count <= 0)
        return 1;

    for (int i = 0; i < dict->count; i++) {
        DictEntry* entry = &dict->entries[i];
        const char* key = entry->key ? entry->key : "";
        Value v = entry->value;
        if (v.type != VALUE_STRING) {
            snprintf(err, err_size, "HTTP header values must be strings.");
            return 0;
        }
        const char* value = v.string_value ? v.string_value : "";
        if (!http_header_token_is_safe(key) || !http_header_value_is_safe(value)) {
            snprintf(err, err_size, "HTTP headers contain unsupported characters.");
            return 0;
        }

        char header_line[1024];
        int wrote = snprintf(header_line, sizeof(header_line), "%s: %s", key, value);
        if (wrote <= 0 || (size_t)wrote >= sizeof(header_line)) {
            snprintf(err, err_size, "HTTP headers are too large.");
            return 0;
        }

        wchar_t* header_w = winhttp_utf8_to_wide(header_line, err, err_size);
        if (!header_w)
            return 0;
        BOOL ok = WinHttpAddRequestHeaders(request, header_w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        free(header_w);
        if (!ok) {
            winhttp_set_error(err, err_size, "Failed to add HTTP headers");
            return 0;
        }
    }
    return 1;
}

static int winhttp_read_response(HINTERNET request, char** out_body, char* err, size_t err_size) {
    if (!out_body)
        return 0;
    *out_body = NULL;
    size_t cap = 0;
    size_t len = 0;
    char* buffer = NULL;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            winhttp_set_error(err, err_size, "Failed reading HTTP response");
            free(buffer);
            return 0;
        }
        if (available == 0)
            break;
        size_t needed = len + (size_t)available + 1;
        if (needed > cap) {
            size_t new_cap = cap == 0 ? 4096 : cap;
            while (new_cap < needed)
                new_cap *= 2;
            char* grown = (char*)realloc(buffer, new_cap);
            if (!grown) {
                snprintf(err, err_size, "Out of memory while reading HTTP response.");
                free(buffer);
                return 0;
            }
            buffer = grown;
            cap = new_cap;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + len, available, &read)) {
            winhttp_set_error(err, err_size, "Failed reading HTTP response");
            free(buffer);
            return 0;
        }
        len += (size_t)read;
    }

    if (!buffer) {
        buffer = (char*)malloc(1);
        if (!buffer) {
            snprintf(err, err_size, "Out of memory while reading HTTP response.");
            return 0;
        }
    }
    buffer[len] = '\0';
    *out_body = buffer;
    return 1;
}

static int run_http_winhttp_request(const char* method, const char* url, const char* body, Value headers, char** out_body, int* out_status, char* err, size_t err_size) {
    if (!method || !url || !out_body || !out_status || !err || err_size == 0)
        return 0;
    err[0] = '\0';
    *out_body = NULL;
    *out_status = 0;

    wchar_t* host = NULL;
    wchar_t* path = NULL;
    INTERNET_PORT port = 0;
    DWORD flags = 0;
    if (!winhttp_split_url(url, &host, &path, &port, &flags, err, err_size))
        return 0;

    wchar_t* method_w = winhttp_utf8_to_wide(method, err, err_size);
    if (!method_w) {
        free(host);
        free(path);
        return 0;
    }

    HINTERNET session = WinHttpOpen(L"Sicht/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session) {
        winhttp_set_error(err, err_size, "WinHttpOpen failed");
        free(host);
        free(path);
        free(method_w);
        return 0;
    }
    winhttp_enable_tls(session);
    winhttp_apply_proxy(session, url);
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        winhttp_set_error(err, err_size, "WinHttpConnect failed");
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    HINTERNET request = WinHttpOpenRequest(connect, method_w, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        winhttp_set_error(err, err_size, "WinHttpOpenRequest failed");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    if (!winhttp_add_headers(request, headers, err, err_size)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    const char* body_text = body ? body : "";
    DWORD body_len = (DWORD)strlen(body_text);
    BOOL send_ok = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        (LPVOID)(body_len ? body_text : NULL),
        body_len,
        body_len,
        0
    );
    if (!send_ok) {
        winhttp_set_error(err, err_size, "WinHttpSendRequest failed");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    if (!WinHttpReceiveResponse(request, NULL)) {
        winhttp_set_error(err, err_size, "WinHttpReceiveResponse failed");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &status_size, NULL)) {
        winhttp_set_error(err, err_size, "Failed reading HTTP status");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }
    *out_status = (int)status;

    if (!winhttp_read_response(request, out_body, err, err_size)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        free(method_w);
        return 0;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    free(host);
    free(path);
    free(method_w);
    return 1;
}

static int run_http_winhttp_download(const char* url, Value headers, const char* output_path, int* out_status, char* err, size_t err_size) {
    if (!url || !output_path || !out_status || !err || err_size == 0)
        return 0;
    err[0] = '\0';
    *out_status = 0;

    wchar_t* host = NULL;
    wchar_t* path = NULL;
    INTERNET_PORT port = 0;
    DWORD flags = 0;
    if (!winhttp_split_url(url, &host, &path, &port, &flags, err, err_size))
        return 0;

    HINTERNET session = WinHttpOpen(L"Sicht/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session) {
        winhttp_set_error(err, err_size, "WinHttpOpen failed");
        free(host);
        free(path);
        return 0;
    }
    winhttp_enable_tls(session);
    winhttp_apply_proxy(session, url);
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        winhttp_set_error(err, err_size, "WinHttpConnect failed");
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        winhttp_set_error(err, err_size, "WinHttpOpenRequest failed");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    if (!winhttp_add_headers(request, headers, err, err_size)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    BOOL send_ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0);
    if (!send_ok) {
        winhttp_set_error(err, err_size, "WinHttpSendRequest failed");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    if (!WinHttpReceiveResponse(request, NULL)) {
        winhttp_set_error(err, err_size, "WinHttpReceiveResponse failed");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &status_size, NULL)) {
        winhttp_set_error(err, err_size, "Failed reading HTTP status");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }
    *out_status = (int)status;

    FILE* file = fopen(output_path, "wb");
    if (!file) {
        snprintf(err, err_size, "Could not open output file.");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(host);
        free(path);
        return 0;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            winhttp_set_error(err, err_size, "Failed reading HTTP response");
            fclose(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            free(host);
            free(path);
            return 0;
        }
        if (available == 0)
            break;
        char* buffer = (char*)malloc(available);
        if (!buffer) {
            snprintf(err, err_size, "Out of memory while downloading.");
            fclose(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            free(host);
            free(path);
            return 0;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, available, &read)) {
            winhttp_set_error(err, err_size, "Failed reading HTTP response");
            free(buffer);
            fclose(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            free(host);
            free(path);
            return 0;
        }
        if (read > 0 && fwrite(buffer, 1, read, file) != read) {
            snprintf(err, err_size, "File write failed.");
            free(buffer);
            fclose(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            free(host);
            free(path);
            return 0;
        }
        free(buffer);
    }

    fclose(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    free(host);
    free(path);
    return 1;
}
#endif

static Value make_http_result(int ok, int status, const char* body, const char* error_message) {
    const char* err = (error_message && error_message[0]) ? error_message : "";
    if (status == 0 && err[0] == '\0')
        err = "HTTP request failed.";
    Dict* out = gc_dict_new(4);
    dict_set_entry_raw(out, "ok", value_bool(ok ? 1 : 0));
    dict_set_entry_raw(out, "status", value_int(status));
    dict_set_entry_raw(out, "body", value_string(body ? body : ""));
    dict_set_entry_raw(out, "error", value_string(err));
    return value_dict(out);
}

typedef struct {
    char** items;
    int count;
    int capacity;
} ArgList;

static void arglist_init(ArgList* list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void arglist_ensure(ArgList* list, int needed) {
    if (!list)
        return;
    if (needed <= list->capacity)
        return;
    int next = list->capacity > 0 ? list->capacity : 16;
    while (next < needed) {
        if (next > INT_MAX / 2)
            runtime_error("Out of memory", "Argument list exceeded supported size.", "");
        next *= 2;
    }
    if ((size_t)next > SIZE_MAX / sizeof(char*))
        runtime_error("Out of memory", "Argument list exceeded supported size.", "");
    char** grown = (char**)realloc(list->items, sizeof(char*) * (size_t)next);
    if (!grown)
        runtime_error("Out of memory", "Could not grow argument list.", "");
    list->items = grown;
    list->capacity = next;
}

static void arglist_push(ArgList* list, const char* value) {
    if (!value)
        value = "";
    arglist_ensure(list, list->count + 1);
    list->items[list->count++] = sicht_strdup_checked(value);
}

static void arglist_free(ArgList* list) {
    if (!list || !list->items)
        return;
    for (int i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int normalize_http_method(const char* input, char* out, size_t out_size, char* err, size_t err_size) {
    if (!input || input[0] == '\0') {
        snprintf(err, err_size, "HTTP method must be non-empty.");
        return 0;
    }
    size_t len = strlen(input);
    if (len + 1 > out_size) {
        snprintf(err, err_size, "HTTP method is too long.");
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (!isalpha(c)) {
            snprintf(err, err_size, "HTTP method contains unsupported characters.");
            return 0;
        }
        out[i] = (char)toupper(c);
    }
    out[len] = '\0';
    return 1;
}

static int append_curl_headers(Value headers, ArgList* args, char* err, size_t err_size) {
    if (!args)
        return 0;

    if (headers.type != VALUE_DICT)
        return 1;

    Dict* dict = headers.dict_value;
    if (!dict || dict->count <= 0)
        return 1;

    for (int i = 0; i < dict->count; i++) {
        DictEntry* entry = &dict->entries[i];
        const char* key = entry->key ? entry->key : "";
        Value v = entry->value;
        if (v.type != VALUE_STRING) {
            snprintf(err, err_size, "HTTP header values must be strings.");
            return 0;
        }
        const char* value = v.string_value ? v.string_value : "";
        if (!http_header_token_is_safe(key) || !http_header_value_is_safe(value)) {
            snprintf(err, err_size, "HTTP headers contain unsupported characters.");
            return 0;
        }

        char header_line[1024];
        int wrote = snprintf(header_line, sizeof(header_line), "%s: %s", key, value);
        if (wrote <= 0 || (size_t)wrote >= sizeof(header_line)) {
            snprintf(err, err_size, "HTTP headers are too large.");
            return 0;
        }
        arglist_push(args, "-H");
        arglist_push(args, header_line);
    }

    return 1;
}

static char* write_temp_http_body(const char* body, char* err, size_t err_size) {
    if (!body)
        body = "";
#ifdef _WIN32
    char temp_dir[MAX_PATH];
    DWORD got = GetTempPathA(MAX_PATH, temp_dir);
    if (got == 0 || got > MAX_PATH - 1) {
        snprintf(err, err_size, "Could not resolve temp directory for HTTP body.");
        return NULL;
    }
    char temp_file[MAX_PATH];
    if (!GetTempFileNameA(temp_dir, "sht", 0, temp_file)) {
        snprintf(err, err_size, "Could not create temp file for HTTP body.");
        return NULL;
    }
    FILE* bf = fopen(temp_file, "wb");
    if (!bf) {
        DeleteFileA(temp_file);
        snprintf(err, err_size, "Could not open temp file for HTTP body.");
        return NULL;
    }
    size_t body_len = strlen(body);
    if (body_len > 0 && fwrite(body, 1, body_len, bf) != body_len) {
        fclose(bf);
        DeleteFileA(temp_file);
        snprintf(err, err_size, "Could not write HTTP body to temp file.");
        return NULL;
    }
    fclose(bf);
    return sicht_strdup_checked(temp_file);
#else
    char tmpl[] = "/tmp/sicht_http_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        snprintf(err, err_size, "Could not create temp file for HTTP body.");
        return NULL;
    }
    FILE* bf = fdopen(fd, "wb");
    if (!bf) {
        close(fd);
        unlink(tmpl);
        snprintf(err, err_size, "Could not open temp file for HTTP body.");
        return NULL;
    }
    size_t body_len = strlen(body);
    if (body_len > 0 && fwrite(body, 1, body_len, bf) != body_len) {
        fclose(bf);
        unlink(tmpl);
        snprintf(err, err_size, "Could not write HTTP body to temp file.");
        return NULL;
    }
    fclose(bf);
    return sicht_strdup_checked(tmpl);
#endif
}

static int size_add_checked(size_t a, size_t b, size_t* out) {
    if (a > SIZE_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

static int ensure_buffer_capacity(char** buffer, size_t* cap, size_t needed, char* err, size_t err_size, const char* oom_msg, const char* overflow_msg) {
    if (needed <= *cap)
        return 1;
    size_t new_cap = *cap ? *cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            snprintf(err, err_size, "%s", overflow_msg);
            return 0;
        }
        new_cap *= 2;
    }
    char* grown = realloc(*buffer, new_cap);
    if (!grown) {
        snprintf(err, err_size, "%s", oom_msg);
        return 0;
    }
    *buffer = grown;
    *cap = new_cap;
    return 1;
}

static int run_curl_process(const ArgList* args, char** out_body, char* err, size_t err_size) {
    if (!args || args->count <= 0 || !out_body || !err || err_size == 0)
        return 0;

    err[0] = '\0';
    *out_body = NULL;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        snprintf(err, err_size, "Could not create pipe for curl output.");
        return 0;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_handle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (null_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        snprintf(err, err_size, "Could not open NUL for curl stderr.");
        return 0;
    }

    size_t cmd_cap = 1024;
    size_t cmd_len = 0;
    char* cmd = malloc(cmd_cap);
    if (!cmd) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        CloseHandle(null_handle);
        snprintf(err, err_size, "Out of memory while building curl command.");
        return 0;
    }
    cmd[0] = '\0';

    for (int i = 0; i < args->count; i++) {
        const char* arg = args->items[i] ? args->items[i] : "";
        size_t needed = 0;
        if (!size_add_checked(cmd_len, 3, &needed) ||
            !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                "Out of memory while building curl command.",
                "Curl command too long.")) {
            free(cmd);
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            CloseHandle(null_handle);
            return 0;
        }
        if (cmd_len > 0)
            cmd[cmd_len++] = ' ';

        cmd[cmd_len++] = '"';
        int backslashes = 0;
        for (const char* p = arg; *p; p++) {
            if (*p == '\\') {
                backslashes++;
                if (!size_add_checked(cmd_len, 2, &needed) ||
                    !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                        "Out of memory while building curl command.",
                        "Curl command too long.")) {
                    free(cmd);
                    CloseHandle(read_pipe);
                    CloseHandle(write_pipe);
                    CloseHandle(null_handle);
                    return 0;
                }
                cmd[cmd_len++] = '\\';
                continue;
            }
            if (*p == '"') {
                for (int j = 0; j < backslashes; j++) {
                    if (!size_add_checked(cmd_len, 1, &needed) ||
                        !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                            "Out of memory while building curl command.",
                            "Curl command too long.")) {
                        free(cmd);
                        CloseHandle(read_pipe);
                        CloseHandle(write_pipe);
                        CloseHandle(null_handle);
                        return 0;
                    }
                    cmd[cmd_len++] = '\\';
                }
                backslashes = 0;
                if (!size_add_checked(cmd_len, 2, &needed) ||
                    !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                        "Out of memory while building curl command.",
                        "Curl command too long.")) {
                    free(cmd);
                    CloseHandle(read_pipe);
                    CloseHandle(write_pipe);
                    CloseHandle(null_handle);
                    return 0;
                }
                cmd[cmd_len++] = '\\';
                cmd[cmd_len++] = '"';
                continue;
            }
            backslashes = 0;
            if (!size_add_checked(cmd_len, 1, &needed) ||
                !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                    "Out of memory while building curl command.",
                    "Curl command too long.")) {
                free(cmd);
                CloseHandle(read_pipe);
                CloseHandle(write_pipe);
                CloseHandle(null_handle);
                return 0;
            }
            cmd[cmd_len++] = *p;
        }
        for (int j = 0; j < backslashes; j++) {
            if (!size_add_checked(cmd_len, 1, &needed) ||
                !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                    "Out of memory while building curl command.",
                    "Curl command too long.")) {
                free(cmd);
                CloseHandle(read_pipe);
                CloseHandle(write_pipe);
                CloseHandle(null_handle);
                return 0;
            }
            cmd[cmd_len++] = '\\';
        }
        if (!size_add_checked(cmd_len, 2, &needed) ||
            !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                "Out of memory while building curl command.",
                "Curl command too long.")) {
            free(cmd);
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            CloseHandle(null_handle);
            return 0;
        }
        cmd[cmd_len++] = '"';
        cmd[cmd_len] = '\0';
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = null_handle;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    CloseHandle(write_pipe);
    CloseHandle(null_handle);
    if (!ok) {
        CloseHandle(read_pipe);
        snprintf(err, err_size, "Could not launch curl process.");
        return 0;
    }

    size_t cap = 4096;
    size_t len = 0;
    char* buffer = malloc(cap);
    if (!buffer) {
        CloseHandle(read_pipe);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        snprintf(err, err_size, "Out of memory while reading HTTP response.");
        return 0;
    }
    buffer[0] = '\0';

    for (;;) {
        DWORD got = 0;
        char chunk[1024];
        if (!ReadFile(read_pipe, chunk, sizeof(chunk) - 1, &got, NULL) || got == 0)
            break;
        chunk[got] = '\0';
        size_t needed = 0;
        if (!size_add_checked(len, (size_t)got, &needed) ||
            !size_add_checked(needed, 1, &needed)) {
            free(buffer);
            CloseHandle(read_pipe);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            snprintf(err, err_size, "HTTP response too large.");
            return 0;
        }
        if (needed > cap) {
            if (!ensure_buffer_capacity(&buffer, &cap, needed, err, err_size,
                    "Out of memory while growing HTTP response buffer.",
                    "HTTP response too large.")) {
                free(buffer);
                CloseHandle(read_pipe);
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
        }
        memcpy(buffer + len, chunk, got + 1);
        len += got;
    }

    CloseHandle(read_pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    *out_body = buffer;
    return 1;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err, err_size, "Could not create pipe for curl output.");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(err, err_size, "Could not fork curl process.");
        return 0;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (devnull >= 0)
            close(devnull);

        char** argv = calloc((size_t)args->count + 1, sizeof(char*));
        if (!argv)
            _exit(127);
        for (int i = 0; i < args->count; i++)
            argv[i] = args->items[i];
        argv[args->count] = NULL;
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t cap = 4096;
    size_t len = 0;
    char* buffer = malloc(cap);
    if (!buffer) {
        close(pipefd[0]);
        snprintf(err, err_size, "Out of memory while reading HTTP response.");
        return 0;
    }
    buffer[0] = '\0';

    for (;;) {
        char chunk[1024];
        ssize_t got = read(pipefd[0], chunk, sizeof(chunk) - 1);
        if (got <= 0)
            break;
        chunk[got] = '\0';
        size_t needed = 0;
        if (!size_add_checked(len, (size_t)got, &needed) ||
            !size_add_checked(needed, 1, &needed)) {
            free(buffer);
            close(pipefd[0]);
            snprintf(err, err_size, "HTTP response too large.");
            return 0;
        }
        if (needed > cap) {
            if (!ensure_buffer_capacity(&buffer, &cap, needed, err, err_size,
                    "Out of memory while growing HTTP response buffer.",
                    "HTTP response too large.")) {
                free(buffer);
                close(pipefd[0]);
                return 0;
            }
        }
        memcpy(buffer + len, chunk, (size_t)got + 1);
        len += (size_t)got;
    }
    close(pipefd[0]);
    (void)waitpid(pid, NULL, 0);

    *out_body = buffer;
    return 1;
#endif
}

static void read_stderr_snippet(const char* path, char* err, size_t err_size);

static int run_http_curl_process_request(const char* method, const char* url, const char* body, Value headers, char** out_body, int* out_status, char* err, size_t err_size) {
    if (!http_method_is_safe(method) || !http_url_is_safe(url)) {
        snprintf(err, err_size, "HTTP method or URL contains unsupported characters.");
        return 0;
    }

    char* body_file = NULL;
    int use_body_file = body && body[0] != '\0' && strcmp(method, "GET") != 0;
    char* stderr_file = NULL;

#ifdef _WIN32
    char temp_dir[MAX_PATH];
    DWORD got = GetTempPathA(MAX_PATH, temp_dir);
    if (got > 0 && got < MAX_PATH) {
        char temp_file[MAX_PATH];
        if (GetTempFileNameA(temp_dir, "sht", 0, temp_file))
            stderr_file = sicht_strdup_checked(temp_file);
    }
#else
    char tmpl[] = "/tmp/sicht_http_err_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        close(fd);
        stderr_file = sicht_strdup_checked(tmpl);
    }
#endif

    ArgList args;
    arglist_init(&args);
    arglist_push(&args, "curl");
    arglist_push(&args, "-sS");
    arglist_push(&args, "-L");
    arglist_push(&args, "-X");
    arglist_push(&args, method);

    if (!append_curl_headers(headers, &args, err, err_size)) {
        arglist_free(&args);
        return 0;
    }

    if (use_body_file) {
        body_file = write_temp_http_body(body, err, err_size);
        if (!body_file) {
            arglist_free(&args);
            if (stderr_file) {
#ifdef _WIN32
                DeleteFileA(stderr_file);
#else
                unlink(stderr_file);
#endif
                free(stderr_file);
            }
            return 0;
        }
        char body_ref[1024];
        int wrote = snprintf(body_ref, sizeof(body_ref), "@%s", body_file);
        if (wrote <= 0 || (size_t)wrote >= sizeof(body_ref)) {
            arglist_free(&args);
            snprintf(err, err_size, "HTTP body path is too long.");
            if (stderr_file) {
#ifdef _WIN32
                DeleteFileA(stderr_file);
#else
                unlink(stderr_file);
#endif
                free(stderr_file);
            }
            return 0;
        }
        arglist_push(&args, "--data-binary");
        arglist_push(&args, body_ref);
    }

    if (stderr_file) {
        arglist_push(&args, "--stderr");
        arglist_push(&args, stderr_file);
    }

    arglist_push(&args, url);
    arglist_push(&args, "-w");
    arglist_push(&args, "\n__SICHT_HTTP_STATUS__:%{http_code}");

    char* buffer = NULL;
    if (!run_curl_process(&args, &buffer, err, err_size)) {
        arglist_free(&args);
        if (body_file) {
#ifdef _WIN32
            DeleteFileA(body_file);
#else
            unlink(body_file);
#endif
            free(body_file);
        }
        if (stderr_file) {
            if (err[0] == '\0')
                read_stderr_snippet(stderr_file, err, err_size);
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    arglist_free(&args);
    if (body_file) {
#ifdef _WIN32
        DeleteFileA(body_file);
#else
        unlink(body_file);
#endif
        free(body_file);
    }

    const char* marker = "\n__SICHT_HTTP_STATUS__:";
    char* at = find_last_substring(buffer, marker);
    if (!at) {
        snprintf(err, err_size, "HTTP status marker missing. Ensure curl is installed and reachable.");
        if (stderr_file)
            read_stderr_snippet(stderr_file, err, err_size);
        free(buffer);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    char* status_text = at + strlen(marker);
    char* endptr = NULL;
    long status = strtol(status_text, &endptr, 10);
    if (endptr == status_text || status < 0 || status > INT_MAX) {
        snprintf(err, err_size, "HTTP status parse failed.");
        if (stderr_file)
            read_stderr_snippet(stderr_file, err, err_size);
        free(buffer);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    *at = '\0';
    *out_status = (int)status;
    if (*out_status == 0) {
        if (stderr_file && err[0] == '\0')
            read_stderr_snippet(stderr_file, err, err_size);
        if (err[0] == '\0')
            snprintf(err, err_size, "HTTP request failed.");
        free(buffer);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }
    *out_body = buffer;
    if (stderr_file) {
#ifdef _WIN32
        DeleteFileA(stderr_file);
#else
        unlink(stderr_file);
#endif
        free(stderr_file);
    }
    return 1;
}

static int run_http_curl_request(const char* method, const char* url, const char* body, Value headers, char** out_body, int* out_status, char* err, size_t err_size) {
    if (!method || !url || !out_body || !out_status || !err || err_size == 0)
        return 0;

    err[0] = '\0';
    *out_body = NULL;
    *out_status = 0;

#ifdef _WIN32
    char win_err[256];
    if (run_http_winhttp_request(method, url, body, headers, out_body, out_status, win_err, sizeof(win_err)))
        return 1;
    win_err[sizeof(win_err) - 1] = '\0';
    err[0] = '\0';
    if (run_http_curl_process_request(method, url, body, headers, out_body, out_status, err, err_size))
        return 1;
    if (err[0] == '\0' && win_err[0] != '\0') {
        strncpy(err, win_err, err_size - 1);
        err[err_size - 1] = '\0';
    }
    return 0;
#else
#ifdef SICHT_LIBCURL_ENABLED
    return run_http_libcurl_request(method, url, body, headers, out_body, out_status, err, err_size);
#endif
    return run_http_curl_process_request(method, url, body, headers, out_body, out_status, err, err_size);
#endif
}

static void read_stderr_snippet(const char* path, char* err, size_t err_size) {
    if (!path || !err || err_size == 0)
        return;
    FILE* file = fopen(path, "rb");
    if (!file)
        return;
    size_t read_count = fread(err, 1, err_size - 1, file);
    fclose(file);
    if (read_count == 0) {
        err[0] = '\0';
        return;
    }
    err[read_count] = '\0';
    while (read_count > 0) {
        char c = err[read_count - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
        err[--read_count] = '\0';
    }
}

static int run_http_curl_process_download(const char* url, const char* output_path, Value headers, int* out_status, char* err, size_t err_size) {
    if (!http_url_is_safe(url) || !http_url_is_safe(output_path)) {
        snprintf(err, err_size, "HTTP url or output path contains unsupported characters.");
        return 0;
    }

    char* stderr_file = NULL;
#ifdef _WIN32
    char temp_dir[MAX_PATH];
    DWORD got = GetTempPathA(MAX_PATH, temp_dir);
    if (got > 0 && got < MAX_PATH) {
        char temp_file[MAX_PATH];
        if (GetTempFileNameA(temp_dir, "sht", 0, temp_file))
            stderr_file = sicht_strdup_checked(temp_file);
    }
#else
    char tmpl[] = "/tmp/sicht_http_err_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        close(fd);
        stderr_file = sicht_strdup_checked(tmpl);
    }
#endif

    ArgList args;
    arglist_init(&args);
    arglist_push(&args, "curl");
    arglist_push(&args, "-sS");
    arglist_push(&args, "-L");
    arglist_push(&args, "-X");
    arglist_push(&args, "GET");

    if (!append_curl_headers(headers, &args, err, err_size)) {
        arglist_free(&args);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    if (stderr_file) {
        arglist_push(&args, "--stderr");
        arglist_push(&args, stderr_file);
    }

    arglist_push(&args, "-o");
    arglist_push(&args, output_path);
    arglist_push(&args, url);
    arglist_push(&args, "-w");
    arglist_push(&args, "\n__SICHT_HTTP_STATUS__:%{http_code}");

    char* buffer = NULL;
    if (!run_curl_process(&args, &buffer, err, err_size)) {
        arglist_free(&args);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        if (buffer)
            free(buffer);
        return 0;
    }

    arglist_free(&args);

    const char* marker = "\n__SICHT_HTTP_STATUS__:";
    char* at = find_last_substring(buffer, marker);
    if (!at) {
        snprintf(err, err_size, "HTTP status marker missing. Ensure curl is installed and reachable.");
        if (stderr_file)
            read_stderr_snippet(stderr_file, err, err_size);
        if (buffer)
            free(buffer);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    char* status_text = at + strlen(marker);
    char* endptr = NULL;
    long status = strtol(status_text, &endptr, 10);
    if (endptr == status_text || status < 0 || status > INT_MAX) {
        snprintf(err, err_size, "HTTP status parse failed.");
        if (buffer)
            free(buffer);
        if (stderr_file) {
#ifdef _WIN32
            DeleteFileA(stderr_file);
#else
            unlink(stderr_file);
#endif
            free(stderr_file);
        }
        return 0;
    }

    *out_status = (int)status;
    if (*out_status == 0 && err[0] == '\0' && stderr_file)
        read_stderr_snippet(stderr_file, err, err_size);

    if (buffer)
        free(buffer);
    if (stderr_file) {
#ifdef _WIN32
        DeleteFileA(stderr_file);
#else
        unlink(stderr_file);
#endif
        free(stderr_file);
    }
    return 1;
}

static int run_http_curl_download(const char* url, const char* output_path, Value headers, int* out_status, char* err, size_t err_size) {
    if (!url || !output_path || !out_status || !err || err_size == 0)
        return 0;

    err[0] = '\0';
    *out_status = 0;
    char* buffer = NULL;

#ifdef _WIN32
    (void)buffer;
    {
        char win_err[256];
        if (run_http_winhttp_download(url, headers, output_path, out_status, win_err, sizeof(win_err)))
            return 1;
        win_err[sizeof(win_err) - 1] = '\0';
        err[0] = '\0';
        if (run_http_curl_process_download(url, output_path, headers, out_status, err, err_size))
            return 1;
        if (err[0] == '\0' && win_err[0] != '\0') {
            strncpy(err, win_err, err_size - 1);
            err[err_size - 1] = '\0';
        }
        return 0;
    }
#else
#ifdef SICHT_LIBCURL_ENABLED
    return run_http_libcurl_download(url, headers, output_path, out_status, err, err_size);
#endif
    return run_http_curl_process_download(url, output_path, headers, out_status, err, err_size);
#endif
}

static int try_eval_native_http_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;

    if (strcmp(name, "native.http.download") == 0) {
        if (e->call_arg_count < 2 || e->call_arg_count > 3) {
            runtime_error(
                "Invalid native HTTP call",
                "HTTP native download requires: download(url, path, [headers]).",
                ""
            );
        }

        Value url_v = eval(e->call_args[0]);
        Value path_v = eval(e->call_args[1]);
        Value headers_v;
        headers_v.type = VALUE_BOOL;
        headers_v.int_value = 0;
        if (e->call_arg_count == 3)
            headers_v = eval(e->call_args[2]);

        if (url_v.type != VALUE_STRING || path_v.type != VALUE_STRING) {
            runtime_error(
                "Invalid native HTTP call",
                "HTTP url/path must be strings.",
                ""
            );
        }
        if (headers_v.type != VALUE_BOOL && headers_v.type != VALUE_DICT) {
            runtime_error(
                "Invalid native HTTP call",
                "HTTP headers must be a dictionary of string values.",
                ""
            );
        }

        int status = 0;
        char err[256];
        if (!run_http_curl_download(url_v.string_value, path_v.string_value, headers_v, &status, err, sizeof(err))) {
            *out = make_http_result(0, 0, "", err);
            return 1;
        }
        *out = make_http_result(status >= 200 && status < 300, status, "", err);
        return 1;
    }

    int mode = 0; /* 1=get, 2=post, 3=request */
    if (strcmp(name, "native.http.get") == 0)
        mode = 1;
    else if (strcmp(name, "native.http.post") == 0)
        mode = 2;
    else if (strcmp(name, "native.http.request") == 0)
        mode = 3;
    else
        return 0;

    if ((mode == 1 && (e->call_arg_count < 1 || e->call_arg_count > 2)) ||
        (mode == 2 && (e->call_arg_count < 2 || e->call_arg_count > 3)) ||
        (mode == 3 && (e->call_arg_count < 2 || e->call_arg_count > 4))) {
        runtime_error(
            "Invalid native HTTP call",
            "HTTP native calls require: get(url, [headers]), post(url, body, [headers]), or request(method, url, [body], [headers]).",
            ""
        );
    }

    Value method_v = value_string("GET");
    Value url_v = value_string("");
    Value body_v = value_string("");
    Value headers_v;
    headers_v.type = VALUE_BOOL;
    headers_v.int_value = 0;

    if (mode == 1) {
        url_v = eval(e->call_args[0]);
        if (e->call_arg_count == 2)
            headers_v = eval(e->call_args[1]);
    } else if (mode == 2) {
        method_v = value_string("POST");
        url_v = eval(e->call_args[0]);
        body_v = eval(e->call_args[1]);
        if (e->call_arg_count == 3)
            headers_v = eval(e->call_args[2]);
    } else {
        method_v = eval(e->call_args[0]);
        url_v = eval(e->call_args[1]);
        if (e->call_arg_count == 3) {
            Value third = eval(e->call_args[2]);
            if (third.type == VALUE_DICT) {
                headers_v = third;
            } else {
                body_v = third;
            }
        } else if (e->call_arg_count == 4) {
            body_v = eval(e->call_args[2]);
            headers_v = eval(e->call_args[3]);
        }
    }

    if (method_v.type != VALUE_STRING || url_v.type != VALUE_STRING || body_v.type != VALUE_STRING) {
        runtime_error(
            "Invalid native HTTP call",
            "HTTP method/url/body must be strings.",
            ""
        );
    }
    if (headers_v.type != VALUE_BOOL && headers_v.type != VALUE_DICT) {
        runtime_error(
            "Invalid native HTTP call",
            "HTTP headers must be a dictionary of string values.",
            ""
        );
    }

    char* response_body = NULL;
    int status = 0;
    char err[256];
    char method_buf[16];
    if (!normalize_http_method(method_v.string_value, method_buf, sizeof(method_buf), err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return 1;
    }
    if (!run_http_curl_request(method_buf, url_v.string_value, body_v.string_value, headers_v, &response_body, &status, err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return 1;
    }

    *out = make_http_result(status >= 200 && status < 300, status, response_body ? response_body : "", "");
    free(response_body);
    return 1;
}

static int try_eval_cli_args_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;
    if (strcmp(name, "args") != 0)
        return 0;
    if (e->call_arg_count != 0)
        runtime_error("Invalid args call", "args() does not take arguments.", "");
    rt_native_cli_args(out);
    return 1;
}

static int try_eval_native_io_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;

    int mode = 0; /* 1=dir exists, 2=create, 3=list dirs, 4=remove, 5=delete, 6=list files, 7=copy file, 8=copy dir */
    if (strcmp(name, "native.io.directory_exists") == 0)
        mode = 1;
    else if (strcmp(name, "native.io.create_directory") == 0)
        mode = 2;
    else if (strcmp(name, "native.io.list_directories") == 0)
        mode = 3;
    else if (strcmp(name, "native.io.remove_directory") == 0)
        mode = 4;
    else if (strcmp(name, "native.io.delete_file") == 0)
        mode = 5;
    else if (strcmp(name, "native.io.list_files") == 0)
        mode = 6;
    else if (strcmp(name, "native.io.copy_file") == 0)
        mode = 7;
    else if (strcmp(name, "native.io.copy_directory") == 0)
        mode = 8;
    else
        return 0;

    int expected_args = (mode == 7 || mode == 8) ? 2 : 1;
    if (e->call_arg_count != expected_args) {
        runtime_error(
            "Invalid native IO call",
            expected_args == 1 ? "IO native calls require a single path argument."
                               : "IO native calls require two path arguments.",
            ""
        );
    }

    Value path_v = eval(e->call_args[0]);
    if (path_v.type != VALUE_STRING)
        runtime_error("Invalid native IO call", "Path must be a string.", "");
    Value path_v2 = value_int(0);
    if (expected_args == 2) {
        path_v2 = eval(e->call_args[1]);
        if (path_v2.type != VALUE_STRING)
            runtime_error("Invalid native IO call", "Path must be a string.", "");
    }

    switch (mode) {
        case 1:
            rt_native_io_directory_exists(out, &path_v);
            break;
        case 2:
            rt_native_io_create_directory(out, &path_v);
            break;
        case 3:
            rt_native_io_list_directories(out, &path_v);
            break;
        case 4:
            rt_native_io_remove_directory(out, &path_v);
            break;
        case 5:
            rt_native_io_delete_file(out, &path_v);
            break;
        case 6:
            rt_native_io_list_files(out, &path_v);
            break;
        case 7:
            rt_native_io_copy_file(out, &path_v, &path_v2);
            break;
        case 8:
            rt_native_io_copy_directory(out, &path_v, &path_v2);
            break;
        default:
            return 0;
    }
    return 1;
}

static int try_eval_native_env_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;
    if (strcmp(name, "native.env.get") != 0)
        return 0;

    if (e->call_arg_count != 1)
        runtime_error("Invalid native env call", "env.get requires exactly one argument.", "");
    Value key_v = eval(e->call_args[0]);
    if (key_v.type != VALUE_STRING)
        runtime_error("Invalid native env call", "Environment variable name must be a string.", "");
    rt_native_env_get(out, &key_v);
    return 1;
}

static int try_eval_native_process_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;

    if (strcmp(name, "native.process.id") == 0) {
        if (e->call_arg_count != 0)
            runtime_error("Invalid native process call", "process.id takes no arguments.", "");
        rt_native_process_id(out);
        return 1;
    }

    if (strcmp(name, "native.process.run") != 0)
        return 0;

    if (e->call_arg_count != 1)
        runtime_error("Invalid native process call", "process.run expects a single command string.", "");
    Value cmd_v = eval(e->call_args[0]);
    if (cmd_v.type != VALUE_STRING)
        runtime_error("Invalid native process call", "Command must be a string.", "");
    rt_native_process_run(out, &cmd_v);
    return 1;
}

static int try_eval_native_time_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;
    if (strcmp(name, "native.time.ms") != 0)
        return 0;

    if (e->call_arg_count != 0)
        runtime_error("Invalid native time call", "time.ms takes no arguments.", "");
    rt_native_time_ms(out);
    return 1;
}

static int try_eval_native_zip_call(Expr* e, Value* out) {
    const char* name = e->call_name;
    if (!name || !out)
        return 0;
    if (strcmp(name, "native.zip.extract") != 0)
        return 0;

    if (e->call_arg_count != 2)
        runtime_error("Invalid native zip call", "zip.extract requires zip path and destination.", "");
    Value zip_v = eval(e->call_args[0]);
    Value dest_v = eval(e->call_args[1]);
    if (zip_v.type != VALUE_STRING || dest_v.type != VALUE_STRING)
        runtime_error("Invalid native zip call", "zip.extract expects string arguments.", "");
    rt_native_zip_extract(out, &zip_v, &dest_v);
    return 1;
}

void interpreter_reset(void) {
    runtime_error_set_location(0, 0);

    for (int i = 0; i < var_count; i++) {
        free(vars[i].name);
        vars[i].name = NULL;
    }

    var_count = 0;
    for (int i = 0; i < function_count; i++) {
        if (functions[i].node && functions[i].owns_node)
            ast_free(functions[i].node);
        functions[i].node = NULL;
        functions[i].name = NULL;
        functions[i].owner_library = NULL;
        functions[i].imported = 0;
        functions[i].is_generator = 0;
        functions[i].owns_node = 0;
    }
    function_count = 0;
    for (int i = 0; i < type_count; i++) {
        free(types[i].name);
        if (types[i].field_names) {
            for (int j = 0; j < types[i].field_count; j++)
                free(types[i].field_names[j]);
            free(types[i].field_names);
        }
        types[i].name = NULL;
        types[i].field_names = NULL;
        types[i].field_count = 0;
    }
    type_count = 0;
    frame_depth = 0;
    llvl_depth = 0;
    gc_set_paused(0);
    debug_reset_calls();
    in_function_call = 0;
    function_has_value = 0;
    function_returned = 0;
    function_yield_list = NULL;
    function_yield_used = 0;
    gc_poll_budget = 0;
    execution_steps = 0;
    execution_step_limit = read_step_limit_from_env();
    generator_yield_limit = read_generator_limit_from_env();
    for (int i = 0; i < loaded_library_count; i++) {
        free(loaded_libraries[i]);
        loaded_libraries[i] = NULL;
    }
    loaded_library_count = 0;
    for (int i = 0; i < loading_library_count; i++) {
        free(loading_libraries[i]);
        loading_libraries[i] = NULL;
    }
    loading_library_count = 0;
    for (int i = 0; i < loading_file_count; i++) {
        free(loading_files[i]);
        loading_files[i] = NULL;
    }
    loading_file_count = 0;
    for (int i = 0; i < library_offer_count; i++) {
        free(library_offers[i].library_name);
        free(library_offers[i].symbol_name);
        library_offers[i].library_name = NULL;
        library_offers[i].symbol_name = NULL;
    }
    library_offer_count = 0;
    current_loading_library = NULL;
    active_function_library = NULL;
    gc_collect(NULL, 0);
}

void interpreter_set_trace(int enabled) {
    trace_enabled = enabled ? 1 : 0;
}



static void exec_block(ASTNode* block);
static void exec_program(ASTNode* program);
static void load_library_by_name(const char* name);
static void exec_load_file(ASTNode* n);
static Value eval(Expr* e);
static void materialize_generator(Generator* g);
static int generator_next_value(Generator* g, Value* out);

static int function_param_index(ASTNode* decl, const char* param_name) {
    for (int i = 0; i < decl->function_decl.param_count; i++) {
        if (strcmp(decl->function_decl.params[i], param_name) == 0)
            return i;
    }
    return -1;
}

static Value call_function(FunctionDef* fn, Expr** args, char** arg_names, int arg_count) {
    ASTNode* decl = fn->node;
    int min_args = decl->function_decl.required_param_count;
    int max_args = decl->function_decl.param_count;
    if (max_args > MAX_CALL_SLOTS)
        runtime_error("Invalid function declaration", "Function has too many parameters for runtime call slots.", "");
    if (min_args < 0 || max_args < 0 || min_args > max_args)
        runtime_error("Invalid function declaration", "Function parameter counts are inconsistent.", "");

    if (arg_count < min_args || arg_count > max_args) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Function `%s` expects %d to %d argument(s), but got %d.",
            fn->name,
            min_args,
            max_args,
            arg_count
        );
        runtime_error("Invalid function call", msg, "");
    }

    Value arg_values[MAX_CALL_SLOTS];
    int assigned[MAX_CALL_SLOTS] = {0};
    Value param_values[MAX_CALL_SLOTS];
    int next_positional_param = 0;

    if (arg_count > MAX_CALL_SLOTS)
        runtime_error("Invalid function call", "Too many arguments.", "");
    for (int i = 0; i < arg_count; i++)
        arg_values[i] = eval(args[i]);

    for (int i = 0; i < arg_count; i++) {
        if (arg_names && arg_names[i]) {
            int idx = function_param_index(decl, arg_names[i]);
            if (idx < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Function `%s` has no parameter named `%s`.",
                    fn->name,
                    arg_names[i]
                );
                runtime_error("Invalid function call", msg, "");
            }

            if (assigned[idx]) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Parameter `%s` was provided more than once.",
                    arg_names[i]
                );
                runtime_error("Invalid function call", msg, "");
            }

            param_values[idx] = arg_values[i];
            assigned[idx] = 1;
            continue;
        }

        while (next_positional_param < decl->function_decl.param_count &&
               assigned[next_positional_param]) {
            next_positional_param++;
        }

        if (next_positional_param >= decl->function_decl.param_count)
            runtime_error("Invalid function call", "Too many positional arguments.", "");

        param_values[next_positional_param] = arg_values[i];
        assigned[next_positional_param] = 1;
        next_positional_param++;
    }

    for (int i = 0; i < decl->function_decl.param_count; i++) {
        if (assigned[i])
            continue;

        Expr* default_expr = decl->function_decl.param_defaults[i];
        if (!default_expr) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Missing required parameter `%s`.",
                decl->function_decl.params[i]
            );
            runtime_error("Invalid function call", msg, "");
        }
        param_values[i] = eval(default_expr);
    }

    if (fn->is_generator) {
        Generator* gen = gc_generator_new(decl->function_decl.param_count);
        gen->function_node = decl;
        gen->owner_library = fn->owner_library;
        for (int i = 0; i < decl->function_decl.param_count; i++)
            gen->args[i] = param_values[i];
        return value_generator(gen);
    }

    if (frame_depth >= MAX_FRAMES)
        runtime_error("Call stack overflow", "Too many nested function calls.", "");

    int base = var_count;
    frame_bases[frame_depth++] = base;
    {
        const char* call_name = fn->name ? fn->name
            : (decl->function_decl.name ? decl->function_decl.name : "function");
        debug_push_call(call_name, decl->line, decl->column);
    }

    for (int i = 0; i < decl->function_decl.param_count; i++)
        add_var(decl->function_decl.params[i], param_values[i]);

    int saved_in_function_call = in_function_call;
    int saved_has_value = function_has_value;
    int saved_returned = function_returned;
    Value saved_last_value = function_last_value;
    List* saved_yield_list = function_yield_list;
    int saved_yield_used = function_yield_used;
    const char* saved_active_function_library = active_function_library;

    in_function_call = 1;
    function_has_value = 0;
    function_returned = 0;
    function_yield_list = NULL;
    function_yield_used = 0;
    active_function_library = fn->owner_library;

    exec_block(decl->function_decl.body);

    if (function_yield_used && function_returned) {
        cleanup_locals_from(base);
        debug_pop_call();
        frame_depth--;
        in_function_call = saved_in_function_call;
        function_has_value = saved_has_value;
        function_returned = saved_returned;
        function_last_value = saved_last_value;
        function_yield_list = saved_yield_list;
        function_yield_used = saved_yield_used;
        active_function_library = saved_active_function_library;
        runtime_error(
            "Invalid function flow",
            "Cannot mix `yield` and `return` in the same function.",
            "Use either `yield` statements or `return`, but not both."
        );
    }

    if (!function_has_value && !function_yield_used) {
        cleanup_locals_from(base);
        debug_pop_call();
        frame_depth--;
        in_function_call = saved_in_function_call;
        function_has_value = saved_has_value;
        function_returned = saved_returned;
        function_last_value = saved_last_value;
        function_yield_list = saved_yield_list;
        function_yield_used = saved_yield_used;
        active_function_library = saved_active_function_library;
        runtime_error(
            "Missing function result",
            "Function body did not evaluate to a value.",
            "Use `return value` or end the function with an expression."
        );
    }

    Value result;
    if (function_yield_used) {
        if (!function_yield_list)
            function_yield_list = gc_list_new(0);
        result = value_list(function_yield_list);
    } else {
        result = function_last_value;
    }

    cleanup_locals_from(base);
    debug_pop_call();
    frame_depth--;

    in_function_call = saved_in_function_call;
    function_has_value = saved_has_value;
    function_returned = saved_returned;
    function_last_value = saved_last_value;
    function_yield_list = saved_yield_list;
    function_yield_used = saved_yield_used;
    active_function_library = saved_active_function_library;

    return result;
}

static void materialize_generator(Generator* g) {
    if (!g || g->initialized)
        return;

    ASTNode* decl = (ASTNode*)g->function_node;
    if (!decl)
        runtime_error("Invalid generator", "Generator has no function body.", "");
    if (g->arg_count != decl->function_decl.param_count)
        runtime_error("Invalid generator", "Generator argument shape does not match function declaration.", "");

    if (frame_depth >= MAX_FRAMES)
        runtime_error("Call stack overflow", "Too many nested function calls.", "");

    int base = var_count;
    frame_bases[frame_depth++] = base;

    for (int i = 0; i < g->arg_count; i++)
        add_var(decl->function_decl.params[i], g->args[i]);

    int saved_in_function_call = in_function_call;
    int saved_has_value = function_has_value;
    int saved_returned = function_returned;
    Value saved_last_value = function_last_value;
    List* saved_yield_list = function_yield_list;
    int saved_yield_used = function_yield_used;
    const char* saved_active_function_library = active_function_library;

    in_function_call = 1;
    function_has_value = 0;
    function_returned = 0;
    function_yield_list = NULL;
    function_yield_used = 0;
    active_function_library = g->owner_library;

    exec_block(decl->function_decl.body);

    if (function_returned)
        runtime_error(
            "Invalid generator flow",
            "Generator function cannot use `return`.",
            "Use `yield` to produce values."
        );

    if (!function_yield_list)
        function_yield_list = gc_list_new(0);

    g->cache = function_yield_list;
    g->initialized = 1;
    g->index = 0;

    cleanup_locals_from(base);
    debug_pop_call();
    frame_depth--;

    in_function_call = saved_in_function_call;
    function_has_value = saved_has_value;
    function_returned = saved_returned;
    function_last_value = saved_last_value;
    function_yield_list = saved_yield_list;
    function_yield_used = saved_yield_used;
    active_function_library = saved_active_function_library;
}

static int generator_next_value(Generator* g, Value* out) {
    if (!g)
        return 0;

    materialize_generator(g);
    if (!g->cache || g->index >= g->cache->count)
        return 0;

    if (out)
        *out = g->cache->items[g->index];
    g->index++;

    if (g->index >= g->cache->count) {
        /* Drop cached values once fully consumed to release references sooner. */
        g->cache->count = 0;
        g->index = 0;
        return 1;
    }

    if (g->index >= 64 && g->index * 2 >= g->cache->count) {
        int remaining = g->cache->count - g->index;
        memmove(g->cache->items, g->cache->items + g->index, sizeof(Value) * (size_t)remaining);
        g->cache->count = remaining;
        g->index = 0;
    }

    return 1;
}

static int type_field_index(const TypeDef* type, const char* field_name) {
    for (int i = 0; i < type->field_count; i++) {
        if (strcmp(type->field_names[i], field_name) == 0)
            return i;
    }
    return -1;
}

static Value call_type_constructor(TypeDef* type, Expr** args, char** arg_names, int arg_count) {
    if (arg_count != type->field_count) {
        char msg[256];
        char hint[256];
        snprintf(msg, sizeof(msg),
            "Type `%s` expects %d value(s), but got %d.",
            type->name,
            type->field_count,
            arg_count
        );
        snprintf(hint, sizeof(hint),
            "Use `%s(...)` with exactly %d value(s), matching the declared fields.",
            type->name,
            type->field_count
        );
        runtime_error("Invalid type construction", msg, hint);
    }

    if (type->field_count > MAX_CALL_SLOTS)
        runtime_error("Invalid type declaration", "Type has too many fields for runtime constructor slots.", "");

    Value assigned_values[MAX_CALL_SLOTS];
    int assigned[MAX_CALL_SLOTS] = {0};
    int next_positional_field = 0;

    if (arg_count > MAX_CALL_SLOTS)
        runtime_error("Invalid type construction", "Too many constructor arguments.", "");

    for (int i = 0; i < arg_count; i++) {
        Value arg_value = eval(args[i]);

        if (arg_names && arg_names[i]) {
            int idx = type_field_index(type, arg_names[i]);
            if (idx < 0) {
                char msg[256];
                char hint[256];
                snprintf(msg, sizeof(msg),
                    "Type `%s` has no field named `%s`.",
                    type->name,
                    arg_names[i]
                );
                snprintf(hint, sizeof(hint),
                    "Use field names declared in `create type %s(...)`.",
                    type->name
                );
                runtime_error("Invalid type construction", msg, hint);
            }

            if (assigned[idx]) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Field `%s` was provided more than once.",
                    arg_names[i]
                );
                runtime_error("Invalid type construction", msg, "Pass each field only once.");
            }

            assigned_values[idx] = arg_value;
            assigned[idx] = 1;
            continue;
        }

        while (next_positional_field < type->field_count && assigned[next_positional_field])
            next_positional_field++;

        if (next_positional_field >= type->field_count)
            runtime_error(
                "Invalid type construction",
                "Too many positional constructor values.",
                "Use fewer positional values or use named arguments."
            );

        assigned_values[next_positional_field] = arg_value;
        assigned[next_positional_field] = 1;
        next_positional_field++;
    }

    for (int i = 0; i < type->field_count; i++) {
        if (!assigned[i]) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Missing value for field `%s` when creating `%s`.",
                type->field_names[i],
                type->name
            );
            runtime_error("Invalid type construction", msg, "Provide all required fields.");
        }
    }

    Dict* d = gc_dict_new(type->field_count);
    for (int i = 0; i < type->field_count; i++)
        dict_set_entry_gc(d, gc_string_new(type->field_names[i]), assigned_values[i]);

    return value_dict(d);
}

static const LlvlFieldDef* llvl_find_field(const LlvlTypeDef* def, const char* field_name) {
    if (!def || !field_name)
        return NULL;
    for (int i = 0; i < def->field_count; i++) {
        if (def->fields[i].name && strcmp(def->fields[i].name, field_name) == 0)
            return &def->fields[i];
    }
    return NULL;
}

static Value llvl_read_field_value(const Value* target, const char* field_name) {
    if (!target || target->type != VALUE_BUFFER || !target->buffer_value)
        runtime_error("Invalid field access", "Target must be a struct/union buffer.", "");

    Buffer* buf = target->buffer_value;
    if (buf->elem_kind != BUFFER_ELEM_STRUCT && buf->elem_kind != BUFFER_ELEM_UNION)
        runtime_error("Invalid field access", "Buffer is not a struct/union allocation.", "");

    if (!buf->elem_type_name)
        runtime_error("Invalid field access", "Struct/union type name is missing.", "");

    LlvlTypeDef* def = llvl_find_type(buf->elem_type_name);
    if (!def) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown low-level type `%s`.", buf->elem_type_name);
        runtime_error("Invalid field access", msg, "");
    }

    const LlvlFieldDef* field = llvl_find_field(def, field_name);
    if (!field) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Field `%s` does not exist in `%s`.", field_name ? field_name : "<null>", def->name);
        runtime_error("Invalid field access", msg, "");
    }

    if (field->offset + field->size > buf->size)
        runtime_error("Invalid field access", "Field exceeds buffer size.", "");

    unsigned char* base = buf->data + field->offset;

    if (field->array_len > 0 || field->kind == LLVL_TYPE_STRUCT || field->kind == LLVL_TYPE_UNION) {
        return value_address(base);
    }

    BufferElemKind kind = llvl_kind_to_buffer_kind(field->kind);
    Value addr = value_address(base);
    Value out;
    rt_llvl_get_at_typed(&out, &addr, (int)kind);
    return out;
}

static void llvl_write_field_value(const Value* target, const char* field_name, Value value) {
    if (!target || target->type != VALUE_BUFFER || !target->buffer_value)
        runtime_error("Invalid field write", "Target must be a struct/union buffer.", "");

    Buffer* buf = target->buffer_value;
    if (buf->elem_kind != BUFFER_ELEM_STRUCT && buf->elem_kind != BUFFER_ELEM_UNION)
        runtime_error("Invalid field write", "Buffer is not a struct/union allocation.", "");

    if (!buf->elem_type_name)
        runtime_error("Invalid field write", "Struct/union type name is missing.", "");

    LlvlTypeDef* def = llvl_find_type(buf->elem_type_name);
    if (!def) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown low-level type `%s`.", buf->elem_type_name);
        runtime_error("Invalid field write", msg, "");
    }

    const LlvlFieldDef* field = llvl_find_field(def, field_name);
    if (!field) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Field `%s` does not exist in `%s`.", field_name ? field_name : "<null>", def->name);
        runtime_error("Invalid field write", msg, "");
    }

    if (field->array_len > 0 || field->kind == LLVL_TYPE_STRUCT || field->kind == LLVL_TYPE_UNION)
        runtime_error("Invalid field write", "Cannot assign to struct/array field directly.", "Assign to a scalar field instead.");

    if (field->offset + field->size > buf->size)
        runtime_error("Invalid field write", "Field exceeds buffer size.", "");

    unsigned char* base = buf->data + field->offset;
    BufferElemKind kind = llvl_kind_to_buffer_kind(field->kind);
    Value addr = value_address(base);
    rt_llvl_set_at_typed(&addr, &value, (int)kind);
}

static Value eval(Expr* e) {
    if (e && e->line > 0)
        runtime_error_set_location(e->line, e->column);

    switch (e->type) {

        case EXPR_LITERAL:
            return value_int(e->int_value);

        case EXPR_FLOAT_LITERAL:
            return value_float(e->float_value);

        case EXPR_BOOL_LITERAL:
            return value_bool(e->int_value);

        case EXPR_STRING_LITERAL:
            return value_string(e->string_value);
        
       case EXPR_CHAR_AT: {
    Value s = eval(e->char_string);
    Value i = eval(e->char_index);

    if (s.type != VALUE_STRING)
        runtime_error(
            "Invalid character access",
            "The target must be a string.",
            ""
        );

    if (i.type != VALUE_INT)
        runtime_error(
            "Invalid character index",
            "Index must be an integer.",
            ""
        );

    if (i.int_value < 0)
        runtime_error(
            "Index out of bounds",
            "Character index cannot be negative.",
            ""
        );

    const char* str = s.string_value;
    int index = i.int_value;

    int current = 0;
    const char* p = str;

    while (*p) {
        if (current == index)
            break;

        unsigned char c = (unsigned char)*p;

        if ((c & 0x80) == 0)       p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else if ((c & 0xF8) == 0xF0) p += 4;
        else
            runtime_error(
                "Invalid UTF-8",
                "Malformed UTF-8 string.",
                ""
            );

        current++;
    }

    if (!*p)
        runtime_error(
            "Index out of bounds",
            "Character index is outside the string.",
            ""
        );

    int len = 1;
    unsigned char c = (unsigned char)*p;

    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;

    char buf[5];
    memcpy(buf, p, len);
    buf[len] = '\0';

    return value_string(buf);
}

                case EXPR_INPUT: {
            
            if (e->input_prompt) {
                printf("%s", e->input_prompt);
                fflush(stdout);
            }

            char buffer[1024];

            if (!fgets(buffer, sizeof(buffer), stdin)) {
                runtime_error(
                    "Input error",
                    "Failed to read input.",
                    ""
                );
            }

            
            buffer[strcspn(buffer, "\n")] = '\0';

            return value_string(buffer);
        }

        case EXPR_FILE_READ: {
            Value path = eval(e->file_read_path);
            if (path.type != VALUE_STRING)
                runtime_error("Invalid file path", "read file expects a string path.", "");

            FILE* f = fopen(path.string_value, "rb");
            if (!f) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Could not open `%s` for reading.", path.string_value);
                runtime_error("File read failed", msg, "");
            }

            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                runtime_error("File read failed", "Could not seek to end of file.", "");
            }
            long size = ftell(f);
            rewind(f);

            if (size < 0) {
                fclose(f);
                runtime_error("File read failed", "Could not determine file size.", "");
            }
            if ((unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
                fclose(f);
                runtime_error("File read failed", "File is too large to read.", "");
            }

            char* buffer = malloc((size_t)size + 1);
            if (!buffer) {
                fclose(f);
                runtime_error("Out of memory", "Could not allocate file buffer.", "");
            }

            size_t read_count = fread(buffer, 1, (size_t)size, f);
            fclose(f);
            if (read_count != (size_t)size) {
                free(buffer);
                runtime_error("File read failed", "Could not read full file content.", "");
            }
            buffer[read_count] = '\0';
            if (read_count >= 3) {
                unsigned char b0 = (unsigned char)buffer[0];
                unsigned char b1 = (unsigned char)buffer[1];
                unsigned char b2 = (unsigned char)buffer[2];
                if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
                    memmove(buffer, buffer + 3, read_count - 3);
                    read_count -= 3;
                    buffer[read_count] = '\0';
                }
            }

            Value out = value_string(buffer);
            free(buffer);
            return out;
        }

        case EXPR_LLVL_VALUE_OF: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            Value out;
            rt_llvl_get_value(&out, &target);
            return out;
        }

        case EXPR_LLVL_BYTE_OF: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            Value index = eval(e->llvl_index);
            Value out;
            rt_llvl_get_byte(&out, &target, &index);
            return out;
        }

        case EXPR_LLVL_BIT_OF: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            Value index = eval(e->llvl_index);
            Value out;
            rt_llvl_get_bit(&out, &target, &index);
            return out;
        }

        case EXPR_LLVL_PLACE_OF: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            Value out;
            rt_llvl_place_of(&out, &target);
            return out;
        }

        case EXPR_LLVL_READ_PIN: {
            require_llvl("Low-level operation");
            Value pin = eval(e->llvl_target);
            Value out;
            rt_llvl_pin_read(&out, &pin);
            return out;
        }

        case EXPR_LLVL_PORT_READ: {
            require_llvl("Low-level operation");
            Value port = eval(e->llvl_target);
            Value out;
            rt_llvl_port_read(&out, &port);
            return out;
        }

        case EXPR_LLVL_OFFSET: {
            require_llvl("Low-level operation");
            Value base = eval(e->llvl_target);
            Value offset = eval(e->llvl_index);
            Value out;
            rt_llvl_offset(&out, &base, &offset);
            return out;
        }

        case EXPR_LLVL_ATOMIC_READ: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            Value out;
            rt_llvl_get_at(&out, &target);
            return out;
        }

        case EXPR_LLVL_FIELD: {
            require_llvl("Low-level operation");
            Value target = eval(e->llvl_target);
            return llvl_read_field_value(&target, e->name);
        }

        case EXPR_VARIABLE: {
            Var* v = find_var(e->name);
            if (!v) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "The variable `%s` was used before assignment.",
                    e->name
                );
                runtime_error(
                    "Undefined variable",
                    msg,
                    "Declare it using: set <name> to <value>"
                );
            }
            return v->value;
        }

        case EXPR_CALL: {
            Value native_http_result;
            if (try_eval_cli_args_call(e, &native_http_result))
                return native_http_result;
        if (try_eval_native_http_call(e, &native_http_result))
            return native_http_result;
        if (try_eval_native_io_call(e, &native_http_result))
            return native_http_result;
        if (try_eval_native_env_call(e, &native_http_result))
            return native_http_result;
        if (try_eval_native_process_call(e, &native_http_result))
            return native_http_result;
        if (try_eval_native_time_call(e, &native_http_result))
            return native_http_result;
        if (try_eval_native_zip_call(e, &native_http_result))
            return native_http_result;

            FunctionDef* fn = find_function(e->call_name);
            if (fn)
                return call_function(fn, e->call_args, e->call_arg_names, e->call_arg_count);

            TypeDef* type = find_type(e->call_name);
            if (type)
                return call_type_constructor(type, e->call_args, e->call_arg_names, e->call_arg_count);

            FunctionDef* hidden = find_function_any(e->call_name);
            if (hidden && hidden->owner_library && !function_is_accessible(hidden)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Function `%s` belongs to library `%s` and is not imported yet.",
                    e->call_name,
                    hidden->owner_library
                );
                runtime_error(
                    "Function not imported",
                    msg,
                    "Use: take <name> from <library>"
                );
            }

            {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "No function or type named `%s` exists.",
                    e->call_name
                );
                runtime_error(
                    "Undefined callable",
                    msg,
                    "Define it using: create function ... or create type ..."
                );
            }
            return value_int(0);
        }

case EXPR_LIST_COMPREHENSION: {
    Value source = eval(e->comp_iterable);
    List* out = gc_list_new(8);
    TempBinding binding;
    temp_binding_begin(e->comp_var_name, &binding);

    if (source.type == VALUE_GENERATOR) {
        Value item;
        while (generator_next_value(source.generator_value, &item)) {
            execution_tick();
            temp_binding_set(&binding, item);

            if (e->comp_filter) {
                Value cond = eval(e->comp_filter);
                if (!value_bool_or_error(cond, "Invalid comprehension filter"))
                    continue;
            }

            list_append(out, eval(e->comp_result));
        }
    } else {
        int item_count = value_collection_count(source);

        if (item_count < 0)
            runtime_error(
                "Invalid iterable",
                "List comprehensions require a list, dictionary, string, or generator.",
                ""
            );

        for (int i = 0; i < item_count; i++) {
            execution_tick();
            Value item = value_collection_item(source, i);
            temp_binding_set(&binding, item);

            if (e->comp_filter) {
                Value cond = eval(e->comp_filter);
                if (!value_bool_or_error(cond, "Invalid comprehension filter"))
                    continue;
            }

            list_append(out, eval(e->comp_result));
        }
    }

    temp_binding_end(&binding);
    return value_list(out);
}

case EXPR_LIST: {
    List* l = gc_list_new(e->list_count);
    l->count = e->list_count;

    for (int i = 0; i < l->count; i++) {
        l->items[i] = eval(e->list_items[i]);
    }

    return value_list(l);
}

case EXPR_ARRAY_LITERAL: {
    List* l = gc_list_new(e->element_count);
    l->count = e->element_count;

    for (int i = 0; i < l->count; i++)
        l->items[i] = eval(e->elements[i]);

    return value_list(l);
}

case EXPR_DICT: {
    Dict* d = gc_dict_new(e->dict_count);

    for (int i = 0; i < e->dict_count; i++) {

        Value key = eval(e->dict_keys[i]);
        Value val = eval(e->dict_values[i]);

        if (key.type != VALUE_STRING) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Dictionary keys must be strings, but got `%s`.",
                value_type_name(key.type)
            );
            runtime_error(
                "Invalid dictionary key",
                msg,
                "Use quoted keys like: { \"name\": value }"
            );
        }

        dict_set_entry_gc(d, key.string_value, val);
    }

    return value_dict(d);
}

case EXPR_DICT_GET: {

    Value dict_val = eval(e->dict_expr);
    Value key_val = eval(e->dict_key);

    if (dict_val.type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary access expected a dictionary, but got `%s`.",
            value_type_name(dict_val.type)
        );
        runtime_error(
            "Invalid dictionary access",
            msg,
            "Use: get item \"key\" from(<dictionary>)"
        );
    }

    if (key_val.type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary key must be a string, but got `%s`.",
            value_type_name(key_val.type)
        );
        runtime_error(
            "Invalid dictionary key",
            msg,
            "Use a quoted key, for example: \"name\""
        );
    }

    Dict* d = dict_val.dict_value;
    int idx = dict_find_index(d, key_val.string_value);
    if (idx >= 0)
        return d->entries[idx].value;

    {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Key `%s` does not exist in dictionary.",
            key_val.string_value
        );
        runtime_error(
            "Missing dictionary item",
            msg,
            "Check the key spelling or add the key before reading it."
        );
    }
    return value_int(0);
}

case EXPR_LIST_AT: {
    Value list = eval(e->list_expr);
    Value idx = eval(e->list_index);

    if (list.type != VALUE_LIST)
        runtime_error(
            "Invalid list access",
            "Target must be a list.",
            ""
        );

    if (idx.type != VALUE_INT)
        runtime_error(
            "Invalid list index",
            "Index must be an integer.",
            ""
        );

    if (idx.int_value < 0 || idx.int_value >= list.list_value->count)
        runtime_error(
            "Index out of bounds",
            "List index is outside the list.",
            ""
        );

    return list.list_value->items[idx.int_value];
}

case EXPR_ARRAY_INDEX: {
    Value list = eval(e->array_expr);
    Value idx = eval(e->index_expr);

    if (list.type != VALUE_LIST)
        runtime_error(
            "Invalid list access",
            "Target must be a list.",
            ""
        );

    if (idx.type != VALUE_INT)
        runtime_error(
            "Invalid list index",
            "Index must be an integer.",
            ""
        );

    if (idx.int_value < 0 || idx.int_value >= list.list_value->count)
        runtime_error(
            "Index out of bounds",
            "List index is outside the list.",
            ""
        );

    return list.list_value->items[idx.int_value];
}

case EXPR_INDEX_OF: {
    Value value = eval(e->index_value);
    Value array = eval(e->index_array);

    if (array.type != VALUE_LIST)
        runtime_error(
            "Invalid index search",
            "Target must be a list.",
            ""
        );

    List* list = array.list_value;

    for (int i = 0; i < list->count; i++) {

        Value item = list->items[i];

        if (item.type != value.type)
            continue;

        if (
            (item.type == VALUE_INT    && item.int_value   == value.int_value) ||
            (item.type == VALUE_FLOAT  && item.float_value == value.float_value) ||
            (item.type == VALUE_BOOL   && item.int_value   == value.int_value) ||
            (item.type == VALUE_STRING && strcmp(item.string_value, value.string_value) == 0)
        ) {
            return value_int(i);
        }
    }

    return value_int(-1);
}

case EXPR_LIST_ADD:
    runtime_error(
        "Invalid expression",
        "List add is a statement, not an expression.",
        ""
    );
    return value_int(0);




        case EXPR_BUILTIN: {
            Value v = eval(e->builtin_arg);

if (e->builtin_type == BUILTIN_NEXT) {
    if (v.type != VALUE_GENERATOR)
        runtime_error(
            "Invalid next usage",
            "next from(...) requires a generator.",
            "Use next from(generator_call(...))."
        );

    Value out;
    if (!generator_next_value(v.generator_value, &out))
        runtime_error(
            "Generator exhausted",
            "No more values are available from this generator.",
            "Check remaining values before pulling again."
        );
    return out;
}


if (e->builtin_type == BUILTIN_REVERSE) {

    if (v.type != VALUE_LIST)
        runtime_error(
            "Invalid reverse usage",
            "reverse requires a list.",
            ""
        );

    List* list = v.list_value;

    int i = 0;
    int j = list->count - 1;

    while (i < j) {
        Value temp = list->items[i];
        list->items[i] = list->items[j];
        list->items[j] = temp;

        i++;
        j--;
    }

    return v;
}



if (e->builtin_type == BUILTIN_SORT) {

    if (v.type != VALUE_LIST)
        runtime_error(
            "Invalid sort usage",
            "sort requires a list.",
            ""
        );

    List* list = v.list_value;

    int all_ints = 1;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type != VALUE_INT) {
            all_ints = 0;
            break;
        }
    }

    if (all_ints && list->count > 1) {
        qsort(list->items, (size_t)list->count, sizeof(Value), compare_int_values);
        return v;
    }

    for (int i = 0; i < list->count - 1; i++) {
        for (int j = 0; j < list->count - i - 1; j++) {

            Value a = list->items[j];
            Value b = list->items[j + 1];

            if (a.type == VALUE_INT && b.type == VALUE_INT) {

                if (a.int_value > b.int_value) {
                    Value temp = list->items[j];
                    list->items[j] = list->items[j + 1];
                    list->items[j + 1] = temp;
                }

            }
        }
    }

    return v;
}
          
         

    if (e->builtin_type == BUILTIN_LENGTH) {

        if (v.type == VALUE_STRING)
            return value_int(size_to_int_or_error(
                strlen(v.string_value),
                "String too large",
                "String length exceeds supported integer range."
            ));

        if (v.type == VALUE_LIST)
            return value_int(v.list_value->count);

        if (v.type == VALUE_GENERATOR) {
            materialize_generator(v.generator_value);
            return value_int(v.generator_value->cache->count - v.generator_value->index);
        }

        runtime_error(
            "Invalid length usage",
            "length of requires a string, list, or generator.",
            ""
        );
    }
            if (v.type != VALUE_STRING) {
                runtime_error(
                    "Invalid builtin usage",
                    "This operation requires a string.",
                    ""
                );
            }

            char* s = v.string_value;

            if (e->builtin_type == BUILTIN_LENGTH) {
                return value_int(size_to_int_or_error(
                    strlen(s),
                    "String too large",
                    "String length exceeds supported integer range."
                ));
            }

            
            size_t s_len = strlen(s);
            if (s_len > SIZE_MAX - 1)
                runtime_error("Out of memory", "String is too large to process.", "");
            char* buf = malloc(s_len + 1);
            if (!buf)
                runtime_error("Out of memory", "Could not allocate string buffer.", "");
            strcpy(buf, s);

            if (e->builtin_type == BUILTIN_UPPERCASE) {
                for (int i = 0; buf[i]; i++)
                    buf[i] = (char)toupper((unsigned char)buf[i]);
            }
            else if (e->builtin_type == BUILTIN_LOWERCASE) {
                for (int i = 0; buf[i]; i++)
                    buf[i] = (char)tolower((unsigned char)buf[i]);
            }
            else if (e->builtin_type == BUILTIN_TRIM) {
                char* start = buf;
                while (*start && isspace((unsigned char)*start))
                    start++;

                if (*start == '\0') {
                    Value result = value_string("");
                    free(buf);
                    return result;
                }

                char* end = start + strlen(start) - 1;
                while (end > start && isspace((unsigned char)*end)) {
                    *end = '\0';
                    end--;
                }

                Value result = value_string(start);
                free(buf);
                return result;
            }

            Value result = value_string(buf);
            free(buf);
            return result;
        }




case EXPR_CAST: {
    Value v = eval(e->cast_expr);

    switch (e->cast_type) {

case CAST_TO_INTEGER:
{
    if (v.type == VALUE_INT)
        return v;

    if (v.type == VALUE_FLOAT) {
        if (!isfinite(v.float_value) || v.float_value < (double)INT_MIN || v.float_value > (double)INT_MAX) {
            runtime_error(
                "Invalid cast",
                "Float value is outside integer range.",
                ""
            );
        }
        return value_int((int)v.float_value);
    }

    if (v.type == VALUE_STRING)
    {
        char *endptr;
        errno = 0;
        long result = strtol(v.string_value, &endptr, 10);

        if (errno == ERANGE || result < INT_MIN || result > INT_MAX)
        {
            runtime_error(
                "Invalid cast",
                "String integer is outside supported range.",
                ""
            );
        }

        if (*endptr != '\0')
        {
            runtime_error(
                "Invalid cast",
                "String is not a valid integer.",
                ""
            );
        }

        return value_int((int)result);
    }

    runtime_error(
        "Invalid cast",
        "Cannot cast this value to integer.",
        ""
    );
    return value_int(0);
}

case CAST_TO_FLOAT:
{
    if (v.type == VALUE_FLOAT)
        return v;

    if (v.type == VALUE_INT)
        return value_float((double)v.int_value);

    if (v.type == VALUE_STRING)
    {
        char *endptr;
        errno = 0;
        double result = strtod(v.string_value, &endptr);

        if (errno == ERANGE || !isfinite(result))
        {
            runtime_error(
                "Invalid cast",
                "String is outside supported float range.",
                ""
            );
        }

        if (*endptr != '\0')
        {
            runtime_error(
                "Invalid cast",
                "String is not a valid float.",
                ""
            );
        }

        return value_float(result);
    }

    runtime_error(
        "Invalid cast",
        "Cannot cast this value to float.",
        ""
    );
    return value_int(0);
}

        case CAST_TO_BOOLEAN:
            if (v.type == VALUE_BOOL)
                return v;

            if (v.type == VALUE_INT)
                return value_bool(v.int_value != 0);

            if (v.type == VALUE_FLOAT)
                return value_bool(v.float_value != 0.0);

            runtime_error(
                "Invalid cast",
                "Only numeric values can be cast to boolean.",
                ""
            );
            return value_int(0);

        case CAST_TO_STRING:
            return cast_to_string_value(v);
    }

    runtime_error("Invalid cast", "Unknown cast type.", "");
    return value_int(0);
}

        case EXPR_UNARY: {
            Value v = eval(e->expr);

            if (e->op == OP_NOT) {
                if (v.type != VALUE_BOOL)
                    runtime_error(
                        "Invalid logical operation",
                        "Logical not requires a boolean.",
                        ""
                    );
                return value_bool(!v.int_value);
            }

            if (e->op == OP_BIT_NOT) {
                if (v.type == VALUE_INT)
                    return value_int(~v.int_value);
                if (v.type == VALUE_BOOL)
                    return value_int(~(v.int_value ? 1 : 0));
                runtime_error(
                    "Invalid bitwise operation",
                    "Bitwise not requires an integer or boolean.",
                    ""
                );
            }

            if (e->op == OP_NEG) {
                if (v.type == VALUE_INT) {
                    if (v.int_value == INT_MIN)
                        runtime_error(
                            "Integer overflow",
                            "Negation overflow on minimum integer value.",
                            ""
                        );
                    return value_int(-v.int_value);
                }
                if (v.type == VALUE_FLOAT)
                    return value_float(-v.float_value);
            }

            runtime_error(
                "Invalid unary operation",
                "Unary operation requires a compatible value.",
                ""
            );
            return value_int(0);
        }

        case EXPR_BINARY: {
            Value l = eval(e->left);
            Value r = eval(e->right);




            
if (e->op == OP_CONTAINS) {

    if (l.type == VALUE_LIST) {
        List* list = l.list_value;
        for (int i = 0; i < list->count; i++) {
            if (value_equals(list->items[i], r))
                return value_bool(1);
        }
        return value_bool(0);
    }

    if (l.type == VALUE_DICT) {
        if (r.type != VALUE_STRING)
            runtime_error(
                "Invalid dictionary contains",
                "Dictionary keys must be strings.",
                ""
            );

        return value_bool(dict_find_index(l.dict_value, r.string_value) >= 0);
    }

    if (l.type == VALUE_STRING) {
        if (r.type != VALUE_STRING)
            runtime_error(
                "Invalid string contains",
                "String membership needs a string value.",
                ""
            );
        return value_bool(strstr(l.string_value, r.string_value) != NULL);
    }

    runtime_error(
        "Invalid contains usage",
        "Left side must be a list, dictionary, or string.",
        ""
    );
}

            
            if (e->op == OP_AND || e->op == OP_OR) {
                if (l.type != VALUE_BOOL || r.type != VALUE_BOOL)
                    runtime_error(
                        "Invalid logical operation",
                        "Logical operators require booleans.",
                        ""
                    );

                return value_bool(
                    e->op == OP_AND
                        ? (l.int_value && r.int_value)
                        : (l.int_value || r.int_value)
                );
            }

            if (e->op == OP_BIT_AND || e->op == OP_BIT_OR || e->op == OP_BIT_XOR ||
                e->op == OP_SHL || e->op == OP_SHR) {
                if (!((l.type == VALUE_INT || l.type == VALUE_BOOL) &&
                      (r.type == VALUE_INT || r.type == VALUE_BOOL))) {
                    runtime_error(
                        "Invalid bitwise operation",
                        "Bitwise operators require integers or booleans.",
                        ""
                    );
                }

                int a = (l.type == VALUE_BOOL) ? (l.int_value ? 1 : 0) : l.int_value;
                int b = (r.type == VALUE_BOOL) ? (r.int_value ? 1 : 0) : r.int_value;

                if (e->op == OP_SHL || e->op == OP_SHR) {
                    if (b < 0)
                        runtime_error("Invalid shift", "Shift amount must be non-negative.", "");
                    if (e->op == OP_SHL)
                        return value_int(a << b);
                    return value_int(a >> b);
                }

                if (e->op == OP_BIT_AND)
                    return value_int(a & b);
                if (e->op == OP_BIT_OR)
                    return value_int(a | b);
                return value_int(a ^ b);
            }

            
            if (e->op == OP_ADD &&
                (l.type == VALUE_STRING || r.type == VALUE_STRING)) {

                Value left_str = cast_to_string_value(l);
                Value right_str = cast_to_string_value(r);

                size_t left_len = strlen(left_str.string_value);
                size_t right_len = strlen(right_str.string_value);
                if (left_len > SIZE_MAX - right_len - 1)
                    runtime_error("Out of memory", "String concatenation is too large.", "");
                size_t len = left_len + right_len + 1;

                char* buf = malloc(len);
                if (!buf)
                    runtime_error("Out of memory", "Could not allocate concatenation buffer.", "");
                strcpy(buf, left_str.string_value);
                strcat(buf, right_str.string_value);

                Value v = value_string(buf);
                free(buf);
                return v;
            }

            
            if (e->op == OP_EQ || e->op == OP_NE ||
                e->op == OP_GT || e->op == OP_LT ||
                e->op == OP_GTE || e->op == OP_LTE) {

                if (l.type != r.type) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "Cannot compare `%s` with `%s`.",
                        value_type_name(l.type),
                        value_type_name(r.type)
                    );
                    runtime_error(
                        "Invalid comparison",
                        msg,
                        "Cast one side so both operands have the same type."
                    );
                }

                if (l.type == VALUE_INT) {
                    int a = l.int_value, b = r.int_value;
                    switch (e->op) {
                        case OP_EQ:  return value_bool(a == b);
                        case OP_NE:  return value_bool(a != b);
                        case OP_GT:  return value_bool(a >  b);
                        case OP_LT:  return value_bool(a <  b);
                        case OP_GTE: return value_bool(a >= b);
                        case OP_LTE: return value_bool(a <= b);
                        default: break;
                    }
                }

                if (l.type == VALUE_FLOAT) {
                    double a = l.float_value, b = r.float_value;
                    switch (e->op) {
                        case OP_EQ:  return value_bool(a == b);
                        case OP_NE:  return value_bool(a != b);
                        case OP_GT:  return value_bool(a >  b);
                        case OP_LT:  return value_bool(a <  b);
                        case OP_GTE: return value_bool(a >= b);
                        case OP_LTE: return value_bool(a <= b);
                        default: break;
                    }
                }

                if (l.type == VALUE_BOOL) {
                    if (e->op != OP_EQ && e->op != OP_NE)
                        runtime_error(
                            "Invalid boolean comparison",
                            "Booleans cannot be ordered.",
                            ""
                        );

                    return value_bool(
                        e->op == OP_EQ
                            ? l.int_value == r.int_value
                            : l.int_value != r.int_value
                    );
                }

                if (l.type == VALUE_STRING) {
                    if (e->op != OP_EQ && e->op != OP_NE)
                        runtime_error(
                            "Invalid string comparison",
                            "Strings cannot be ordered.",
                            ""
                        );

                    return value_bool(
                        e->op == OP_EQ
                            ? strcmp(l.string_value, r.string_value) == 0
                            : strcmp(l.string_value, r.string_value) != 0
                    );
                }
            }

            
            if (l.type != r.type ||
               (l.type != VALUE_INT && l.type != VALUE_FLOAT)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Arithmetic requires matching numeric types, got `%s` and `%s`.",
                    value_type_name(l.type),
                    value_type_name(r.type)
                );
                runtime_error(
                    "Invalid arithmetic",
                    msg,
                    "Use int with int or float with float (or cast explicitly)."
                );
            }

            if (l.type == VALUE_FLOAT) {
                double a = l.float_value, b = r.float_value;
                if (e->op == OP_DIV && b == 0)
                    runtime_error("Division by zero", "", "");
                switch (e->op) {
                    case OP_ADD: return value_float(a + b);
                    case OP_SUB: return value_float(a - b);
                    case OP_MUL: return value_float(a * b);
                    case OP_DIV: return value_float(a / b);
                    default: break;
                }
            }

            if (l.type == VALUE_INT) {
                int a = l.int_value, b = r.int_value;
                if (e->op == OP_DIV && b == 0)
                    runtime_error("Division by zero", "", "");
                if (e->op == OP_DIV && a == INT_MIN && b == -1)
                    runtime_error("Integer overflow", "Division overflow for minimum integer by -1.", "");
                switch (e->op) {
                    case OP_ADD: {
                        int out = 0;
                        if (!int_add_checked(a, b, &out))
                            runtime_error("Integer overflow", "Addition overflow.", "");
                        return value_int(out);
                    }
                    case OP_SUB: {
                        int out = 0;
                        if (!int_sub_checked(a, b, &out))
                            runtime_error("Integer overflow", "Subtraction overflow.", "");
                        return value_int(out);
                    }
                    case OP_MUL: {
                        int out = 0;
                        if (!int_mul_checked(a, b, &out))
                            runtime_error("Integer overflow", "Multiplication overflow.", "");
                        return value_int(out);
                    }
                    case OP_DIV: return value_int(a / b);
                    default: break;
                }
            }

            runtime_error("Invalid arithmetic", "Unknown operator.", "");
            return value_int(0);
        }
    }

    

    runtime_error("Interpreter error", "Unknown expression.", "");
    return value_int(0);
}



static void exec_node(ASTNode* n);

static int loop_exit = 0;
static int loop_next = 0;

static int ast_block_contains_yield(ASTNode* node) {
    if (!node)
        return 0;

    switch (node->type) {
        case AST_YIELD:
            return 1;
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++) {
                if (ast_block_contains_yield(node->body[i]))
                    return 1;
            }
            return 0;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++) {
                if (ast_block_contains_yield(node->branches[i].block))
                    return 1;
            }
            return ast_block_contains_yield(node->else_block);
        case AST_TRY:
            if (ast_block_contains_yield(node->try_stmt.try_block))
                return 1;
            return ast_block_contains_yield(node->try_stmt.otherwise_block);
        case AST_WHILE:
            return ast_block_contains_yield(node->while_stmt.body);
        case AST_REPEAT:
            return ast_block_contains_yield(node->repeat_stmt.body);
        case AST_FOR_EACH:
            return ast_block_contains_yield(node->for_each_stmt.body);
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                if (ast_block_contains_yield(node->match_stmt.branches[i].block))
                    return 1;
            }
            return ast_block_contains_yield(node->match_stmt.otherwise_block);
        default:
            return 0;
    }
}

static void register_function(ASTNode* node) {
    if (node->function_decl.param_count > MAX_CALL_SLOTS)
        runtime_error("Invalid function declaration", "Function has too many parameters.", "");
    if (node->function_decl.required_param_count > node->function_decl.param_count)
        runtime_error("Invalid function declaration", "Required parameter count is inconsistent.", "");

    if (find_function_any(node->function_decl.name)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Function `%s` is already defined.",
            node->function_decl.name
        );
        runtime_error("Duplicate function", msg, "");
    }

    ensure_function_capacity();
    ASTNode* clone = ast_clone(node);
    functions[function_count].name = clone ? clone->function_decl.name : NULL;
    functions[function_count].node = clone;
    functions[function_count].owner_library = current_loading_library;
    functions[function_count].imported = current_loading_library ? 0 : 1;
    functions[function_count].is_generator = ast_block_contains_yield(node->function_decl.body);
    functions[function_count].owns_node = 1;
    function_count++;
}

static void register_type(ASTNode* node) {
    if (type_count >= MAX_TYPES)
        runtime_error("Too many types", "Type declaration limit reached.", "");
    if (node->type_decl.field_count > MAX_CALL_SLOTS)
        runtime_error("Invalid type declaration", "Type has too many fields.", "");

    if (find_type(node->type_decl.name)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Type `%s` is already defined.",
            node->type_decl.name
        );
        runtime_error("Duplicate type", msg, "");
    }

    if (find_function_any(node->type_decl.name)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Type `%s` conflicts with an existing function name.",
            node->type_decl.name
        );
        runtime_error("Type name conflict", msg, "");
    }

    for (int i = 0; i < node->type_decl.field_count; i++) {
        for (int j = i + 1; j < node->type_decl.field_count; j++) {
            if (strcmp(node->type_decl.fields[i], node->type_decl.fields[j]) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Type `%s` repeats field `%s`.",
                    node->type_decl.name,
                    node->type_decl.fields[i]
                );
                runtime_error("Invalid type fields", msg, "");
            }
        }
    }

    types[type_count].name = heap_strdup_optional(node->type_decl.name);
    types[type_count].field_count = node->type_decl.field_count;
    if (node->type_decl.field_count > 0) {
        types[type_count].field_names = (char**)malloc_checked(sizeof(char*) * (size_t)node->type_decl.field_count);
        for (int i = 0; i < node->type_decl.field_count; i++)
            types[type_count].field_names[i] = heap_strdup_optional(node->type_decl.fields[i]);
    } else {
        types[type_count].field_names = NULL;
    }
    type_count++;
}

static int has_si_extension(const char* name) {
    size_t len = strlen(name);
    if (len < 3)
        return 0;
    return strcmp(name + len - 3, ".si") == 0;
}

static int is_absolute_module_path(const char* name) {
    if (!name || !name[0])
        return 0;
    if (name[0] == '/' || name[0] == '\\')
        return 1;
    if (isalpha((unsigned char)name[0]) && name[1] == ':')
        return 1;
    return 0;
}

static int is_bare_library_name(const char* name) {
    if (!name || !name[0])
        return 0;
    if (is_absolute_module_path(name))
        return 0;
    if (name[0] == '.')
        return 0;
    for (const char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            return 0;
    }
    return 1;
}

static void normalize_module_path(const char* input, char* out, size_t out_size) {
    if (!input || !out || out_size == 0)
        runtime_error("Module path error", "Missing module path data.", "");

    size_t len = strlen(input);
    if (len > SIZE_MAX - 1)
        runtime_error("Module path error", "Module path is too long.", "");
    char* buf = (char*)malloc(len + 1);
    if (!buf)
        runtime_error("Out of memory", "Could not allocate module path buffer.", "");

    for (size_t i = 0; i <= len; i++)
        buf[i] = (input[i] == '\\') ? '/' : input[i];

    if (len >= 3 && strcmp(buf + len - 3, ".si") == 0)
        buf[len - 3] = '\0';

    char prefix[16] = {0};
    char* cursor = buf;
    if (isalpha((unsigned char)cursor[0]) && cursor[1] == ':') {
        prefix[0] = cursor[0];
        prefix[1] = ':';
        prefix[2] = '\0';
        cursor += 2;
        if (*cursor == '/')
            cursor++;
    } else if (*cursor == '/') {
        prefix[0] = '/';
        prefix[1] = '\0';
        cursor++;
    }

    char** segments = NULL;
    size_t seg_count = 0;
    size_t seg_cap = 0;

    char* token = strtok(cursor, "/");
    while (token) {
        if (strcmp(token, ".") == 0) {
            token = strtok(NULL, "/");
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (seg_count > 0 && strcmp(segments[seg_count - 1], "..") != 0) {
                seg_count--;
            } else if (prefix[0] == '\0') {
                if (seg_count >= seg_cap) {
                    size_t next = seg_cap ? seg_cap * 2 : 64;
                    if (next < seg_cap || next > SIZE_MAX / sizeof(char*)) {
                        free(segments);
                        free(buf);
                        runtime_error("Out of memory", "Could not grow module path segments.", "");
                    }
                    char** grown = (char**)realloc(segments, sizeof(char*) * next);
                    if (!grown) {
                        free(segments);
                        free(buf);
                        runtime_error("Out of memory", "Could not grow module path segments.", "");
                    }
                    segments = grown;
                    seg_cap = next;
                }
                segments[seg_count++] = token;
            }
            token = strtok(NULL, "/");
            continue;
        }
        if (seg_count >= seg_cap) {
            size_t next = seg_cap ? seg_cap * 2 : 64;
            if (next < seg_cap || next > SIZE_MAX / sizeof(char*)) {
                free(segments);
                free(buf);
                runtime_error("Out of memory", "Could not grow module path segments.", "");
            }
            char** grown = (char**)realloc(segments, sizeof(char*) * next);
            if (!grown) {
                free(segments);
                free(buf);
                runtime_error("Out of memory", "Could not grow module path segments.", "");
            }
            segments = grown;
            seg_cap = next;
        }
        segments[seg_count++] = token;
        token = strtok(NULL, "/");
    }

    size_t pos = 0;
    if (prefix[0]) {
        size_t pfx_len = strlen(prefix);
        if (pfx_len >= out_size) {
            free(segments);
            free(buf);
            runtime_error("Module path too long", "Module path exceeds internal limit.", "");
        }
        memcpy(out + pos, prefix, pfx_len);
        pos += pfx_len;
        if (prefix[0] != '/' && pos + 1 < out_size)
            out[pos++] = '/';
    }

    for (size_t i = 0; i < seg_count; i++) {
        size_t part_len = strlen(segments[i]);
        if (pos + part_len + 2 >= out_size) {
            free(segments);
            free(buf);
            runtime_error("Module path too long", "Module path exceeds internal limit.", "");
        }
        memcpy(out + pos, segments[i], part_len);
        pos += part_len;
        if (i + 1 < seg_count)
            out[pos++] = '/';
    }
    out[pos] = '\0';

    free(segments);
    free(buf);
}

static void resolve_library_name(const char* request, char* out, size_t out_size) {
    char normalized_request[512];
    normalize_module_path(request, normalized_request, sizeof(normalized_request));

    if (is_absolute_module_path(normalized_request) || !current_loading_library) {
        snprintf(out, out_size, "%s", normalized_request);
        return;
    }

    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s", current_loading_library);
    char* slash = strrchr(base_dir, '/');
    if (slash)
        *slash = '\0';
    else
        base_dir[0] = '\0';

    if (base_dir[0] == '\0') {
        snprintf(out, out_size, "%s", normalized_request);
        return;
    }

    char joined[512];
    if (strlen(base_dir) + 1 + strlen(normalized_request) >= sizeof(joined))
        runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");
    joined[0] = '\0';
    strncat(joined, base_dir, sizeof(joined) - strlen(joined) - 1);
    strncat(joined, "/", sizeof(joined) - strlen(joined) - 1);
    strncat(joined, normalized_request, sizeof(joined) - strlen(joined) - 1);
    normalize_module_path(joined, out, out_size);
}

static void resolve_file_name(const char* request, char* out, size_t out_size) {
    char normalized_request[512];
    normalize_module_path(request, normalized_request, sizeof(normalized_request));

    if (is_absolute_module_path(normalized_request)) {
        snprintf(out, out_size, "%s", normalized_request);
        return;
    }

    const char* label = source_get_label();
    if (!label || !label[0] || label[0] == '<') {
        snprintf(out, out_size, "%s", normalized_request);
        return;
    }

    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s", label);
    for (int i = 0; base_dir[i]; i++) {
        if (base_dir[i] == '\\')
            base_dir[i] = '/';
    }

    char* slash = strrchr(base_dir, '/');
    if (slash)
        *slash = '\0';
    else
        base_dir[0] = '\0';

    if (base_dir[0] == '\0') {
        snprintf(out, out_size, "%s", normalized_request);
        return;
    }

    char joined[512];
    if (strlen(base_dir) + 1 + strlen(normalized_request) >= sizeof(joined))
        runtime_error("File path too long", "Resolved file path exceeds internal limit.", "");
    joined[0] = '\0';
    strncat(joined, base_dir, sizeof(joined) - strlen(joined) - 1);
    strncat(joined, "/", sizeof(joined) - strlen(joined) - 1);
    strncat(joined, normalized_request, sizeof(joined) - strlen(joined) - 1);
    normalize_module_path(joined, out, out_size);
}

static int build_rooted_path(char* out, size_t out_size, const char* root, const char* relative) {
    if (!out || out_size == 0 || !relative)
        return 0;
    if (!root || root[0] == '\0') {
        int wrote = snprintf(out, out_size, "%s", relative);
        return wrote > 0 && (size_t)wrote < out_size;
    }
    size_t root_len = strlen(root);
    int need_sep = root_len > 0 && root[root_len - 1] != '/' && root[root_len - 1] != '\\';
    int wrote = snprintf(out, out_size, "%s%s%s", root, need_sep ? "/" : "", relative);
    return wrote > 0 && (size_t)wrote < out_size;
}

static int join_path_simple(char* out, size_t out_size, const char* base, const char* child) {
    if (!out || out_size == 0)
        return 0;
    if (!base || base[0] == '\0') {
        int wrote = snprintf(out, out_size, "%s", child ? child : "");
        return wrote >= 0 && (size_t)wrote < out_size;
    }
    int need_sep = 1;
    size_t len = strlen(base);
    if (len > 0 && (base[len - 1] == '/' || base[len - 1] == '\\'))
        need_sep = 0;
    int wrote = snprintf(out, out_size, "%s%s%s", base, need_sep ? "/" : "", child ? child : "");
    return wrote >= 0 && (size_t)wrote < out_size;
}

#ifdef _WIN32
static int win_path_copy(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0)
        return 0;
    size_t len = strlen(src);
    if (len + 1 > dst_size)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        dst[i] = (c == '/') ? '\\' : c;
    }
    dst[len] = '\0';
    return 1;
}
#endif

static int find_library_recursive(const char* base_dir, const char* target_file, char* out, size_t out_size, int depth) {
    if (!base_dir || !target_file || !out || out_size == 0 || depth <= 0)
        return 0;
#ifdef _WIN32
    char pattern[512];
    if (!join_path_simple(pattern, sizeof(pattern), base_dir, "*"))
        return 0;
    char win_pattern[512];
    if (!win_path_copy(pattern, win_pattern, sizeof(win_pattern)))
        return 0;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.')
            continue;
        char child[512];
        if (!join_path_simple(child, sizeof(child), base_dir, name))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find_library_recursive(child, target_file, out, out_size, depth - 1)) {
                FindClose(h);
                return 1;
            }
        } else {
            if (strcmp(name, target_file) == 0) {
                snprintf(out, out_size, "%s", child);
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return 0;
#else
    DIR* dir = opendir(base_dir);
    if (!dir)
        return 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.')
            continue;
        char child[512];
        if (!join_path_simple(child, sizeof(child), base_dir, name))
            continue;
        struct stat st;
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (find_library_recursive(child, target_file, out, out_size, depth - 1)) {
                closedir(dir);
                return 1;
            }
        } else if (strcmp(name, target_file) == 0) {
            snprintf(out, out_size, "%s", child);
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
#endif
}

static void record_search_attempt(char attempts[][512], int* count, int max, const char* path) {
    if (!attempts || !count || max <= 0 || !path || !path[0])
        return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(attempts[i], path) == 0)
            return;
    }
    if (*count >= max)
        return;
    snprintf(attempts[*count], 512, "%s", path);
    (*count)++;
}

static void build_search_hint(char* out, size_t out_size, char attempts[][512], int count) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!attempts || count <= 0) return;
    size_t pos = 0;
    int wrote = snprintf(out, out_size, "Searched %d path%s:", count, count == 1 ? "" : "s");
    if (wrote < 0 || (size_t)wrote >= out_size)
        return;
    pos = (size_t)wrote;
    for (int i = 0; i < count; i++) {
        if (pos + 3 >= out_size)
            break;
        out[pos++] = '\n';
        out[pos++] = ' ';
        out[pos++] = ' ';
        out[pos] = '\0';
        size_t remaining = out_size - pos;
        int n = snprintf(out + pos, remaining, "%s", attempts[i]);
        if (n < 0 || (size_t)n >= remaining)
            break;
        pos += (size_t)n;
    }
}

static void build_library_file_path(const char* library_name, char* out, size_t out_size) {
    size_t name_len = strlen(library_name);

    if (has_si_extension(library_name)) {
        if (name_len + 1 > out_size) {
            runtime_error("Module path too long", "Module path exceeds internal limit.", "");
            if (out_size > 0)
                out[0] = '\0';
            return;
        }
        memcpy(out, library_name, name_len + 1);
        return;
    }

    if (name_len + 4 > out_size) {
        runtime_error("Module path too long", "Module path exceeds internal limit.", "");
        if (out_size > 0)
            out[0] = '\0';
        return;
    }
    memcpy(out, library_name, name_len);
    out[name_len] = '.';
    out[name_len + 1] = 's';
    out[name_len + 2] = 'i';
    out[name_len + 3] = '\0';
}

static const char* library_basename(const char* name) {
    const char* base = name;
    for (const char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

static int library_name_matches_declaration(const char* loaded_name, const char* declared_name) {
    if (strcmp(loaded_name, declared_name) == 0)
        return 1;

    const char* base = library_basename(loaded_name);
    if (strcmp(base, declared_name) == 0)
        return 1;

    if (has_si_extension(base)) {
        char without_ext[256];
        size_t len = strlen(base);
        if (len - 3 >= sizeof(without_ext))
            return 0;
        memcpy(without_ext, base, len - 3);
        without_ext[len - 3] = '\0';
        if (strcmp(without_ext, declared_name) == 0)
            return 1;
    }

    return 0;
}

static int is_library_loaded(const char* name) {
    for (int i = 0; i < loaded_library_count; i++) {
        if (strcmp(loaded_libraries[i], name) == 0)
            return 1;
    }
    return 0;
}

static int is_library_loading(const char* name) {
    for (int i = 0; i < loading_library_count; i++) {
        if (strcmp(loading_libraries[i], name) == 0)
            return 1;
    }
    return 0;
}

static int is_file_loading(const char* path) {
    for (int i = 0; i < loading_file_count; i++) {
        if (strcmp(loading_files[i], path) == 0)
            return 1;
    }
    return 0;
}

static void push_library_loading(const char* name) {
    if (loading_library_count >= MAX_LIBRARIES)
        runtime_error("Too many libraries", "Library loading stack limit reached.", "");
    loading_libraries[loading_library_count++] = sicht_strdup_checked(name);
}

static void pop_library_loading(void) {
    if (loading_library_count <= 0)
        return;
    loading_library_count--;
    free(loading_libraries[loading_library_count]);
    loading_libraries[loading_library_count] = NULL;
}

static void push_file_loading(const char* path) {
    if (loading_file_count >= MAX_LIBRARIES)
        runtime_error("Too many nested file loads", "File loading stack limit reached.", "");
    loading_files[loading_file_count++] = sicht_strdup_checked(path);
}

static void pop_file_loading(void) {
    if (loading_file_count <= 0)
        return;
    loading_file_count--;
    free(loading_files[loading_file_count]);
    loading_files[loading_file_count] = NULL;
}

static void mark_library_loaded(const char* name) {
    if (loaded_library_count >= MAX_LIBRARIES)
        runtime_error("Too many libraries", "Loaded library limit reached.", "");

    loaded_libraries[loaded_library_count++] = sicht_strdup_checked(name);
}

static int is_library_offered(const char* library_name, const char* symbol_name) {
    for (int i = 0; i < library_offer_count; i++) {
        if (strcmp(library_offers[i].library_name, library_name) != 0)
            continue;
        if (strcmp(library_offers[i].symbol_name, symbol_name) == 0)
            return 1;
    }
    return 0;
}

static void add_library_offer(const char* library_name, const char* symbol_name) {
    if (is_library_offered(library_name, symbol_name))
        return;

    if (library_offer_count >= MAX_LIBRARY_OFFERS)
        runtime_error("Too many offered symbols", "Library offer limit reached.", "");

    library_offers[library_offer_count].library_name = sicht_strdup_checked(library_name);
    library_offers[library_offer_count].symbol_name = sicht_strdup_checked(symbol_name);
    library_offer_count++;
}

static char* read_text_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    if ((unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
        fclose(file);
        runtime_error("File too large", "Could not load module file into memory.", "");
    }

    char* buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        runtime_error("Out of memory", "Could not allocate file buffer.", "");
    }

    size_t read_count = fread(buffer, 1, (size_t)size, file);
    if (read_count != (size_t)size) {
        fclose(file);
        free(buffer);
        runtime_error("File read failed", "Could not read full module file.", "");
    }
    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

static void validate_library_program(ASTNode* program, const char* name) {
    int found = 0;

    for (int i = 0; i < program->count; i++) {
        ASTNode* node = program->body[i];
        if (node->type != AST_CREATE_LIBRARY)
            continue;

        found = 1;
        if (!library_name_matches_declaration(name, node->library_decl.name)) {
            char msg[2048];
            snprintf(msg, sizeof(msg),
                "Library `%s` declares itself as `%s`, which does not match.",
                name,
                node->library_decl.name
            );
            runtime_error(
                "Invalid library declaration",
                msg,
                "Use `create library <name>` matching the loaded module path or its basename."
            );
        }
    }

    if (!found) {
        char msg[2048];
        char hint[2048];
        snprintf(msg, sizeof(msg),
            "Library `%s` must declare itself using `create library %s`.",
            name,
            name
        );
        snprintf(hint, sizeof(hint),
            "Add this near the top of `%s.si`: create library %s",
            name,
            name
        );
        runtime_error("Invalid library file", msg, hint);
    }
}

static void load_library_by_name(const char* name) {
    char resolved_name[512];
    resolve_library_name(name, resolved_name, sizeof(resolved_name));

    if (is_library_loading(resolved_name)) {
        char msg[1024];
        char chain[1024] = {0};
        for (int i = 0; i < loading_library_count; i++) {
            if (i > 0)
                strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, loading_libraries[i], sizeof(chain) - strlen(chain) - 1);
        }
        if (loading_library_count > 0)
            strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
        strncat(chain, resolved_name, sizeof(chain) - strlen(chain) - 1);

        snprintf(msg, sizeof(msg), "Circular import detected: ");
        strncat(msg, chain, sizeof(msg) - strlen(msg) - 1);
        runtime_error("Circular import", msg, "Break the cycle by moving shared code to a third module.");
    }

    if (is_library_loaded(resolved_name))
        return;

    const char* project_root = getenv("SICHT_PROJECT_ROOT");
    const char* lib_root = getenv("SICHT_LIB_ROOT");
    const char* runtime_root = getenv("SICHT_RUNTIME_ROOT");
    const char* root_prefixes[4];
    size_t prefix_count = 0;

    if (project_root && project_root[0])
        root_prefixes[prefix_count++] = project_root;
    if (lib_root && lib_root[0] && (!project_root || strcmp(lib_root, project_root) != 0))
        root_prefixes[prefix_count++] = lib_root;
    if (runtime_root && runtime_root[0] &&
        (!project_root || strcmp(runtime_root, project_root) != 0) &&
        (!lib_root || strcmp(runtime_root, lib_root) != 0))
        root_prefixes[prefix_count++] = runtime_root;
    root_prefixes[prefix_count++] = "";

    char path[512];
    char opened_path[512];
    opened_path[0] = '\0';

    char attempts[24][512];
    int attempt_count = 0;

    char* source = NULL;
    for (size_t p = 0; p < prefix_count && !source; p++) {
        char candidate_name[512];
        if (!build_rooted_path(candidate_name, sizeof(candidate_name), root_prefixes[p], resolved_name))
            runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");
        build_library_file_path(candidate_name, path, sizeof(path));
        record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), path);
        source = read_text_file(path);
        if (source)
            snprintf(opened_path, sizeof(opened_path), "%s", path);
    }

    if (!source && strncmp(resolved_name, "libs/", 5) == 0) {
        const char* rest = resolved_name + 5;
        const char* alt_roots[] = { "libs/stdlib", "libs/third_party" };
        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < sizeof(alt_roots) / sizeof(alt_roots[0]) && !source; i++) {
                char alt_name[512];
                if (snprintf(alt_name, sizeof(alt_name), "%s/%s", alt_roots[i], rest) >= (int)sizeof(alt_name))
                    runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");
                char candidate_name[512];
                if (!build_rooted_path(candidate_name, sizeof(candidate_name), root_prefixes[p], alt_name))
                    runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");
                build_library_file_path(candidate_name, path, sizeof(path));
                record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), path);
                source = read_text_file(path);
                if (source)
                    snprintf(opened_path, sizeof(opened_path), "%s", path);
            }
        }
    }

    if (!source && is_bare_library_name(resolved_name)) {
        const char* search_roots[] = { "libs/stdlib", "libs/third_party", "libs" };
        const size_t root_count = sizeof(search_roots) / sizeof(search_roots[0]);

        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < root_count && !source; i++) {
                char base_root[512];
                if (!build_rooted_path(base_root, sizeof(base_root), root_prefixes[p], search_roots[i]))
                    runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");

                char candidate_relative[512];
                if (snprintf(candidate_relative, sizeof(candidate_relative), "%s/%s", base_root, resolved_name) >= (int)sizeof(candidate_relative))
                    runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");

                char candidate_name[512];
                resolve_file_name(candidate_relative, candidate_name, sizeof(candidate_name));

                char candidate_path[512];
                build_library_file_path(candidate_name, candidate_path, sizeof(candidate_path));
                record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), candidate_path);
                source = read_text_file(candidate_path);
                if (source)
                    snprintf(opened_path, sizeof(opened_path), "%s", candidate_path);

                if (source)
                    continue;

                if (strcmp(candidate_name, candidate_relative) != 0) {
                    build_library_file_path(candidate_relative, candidate_path, sizeof(candidate_path));
                    record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), candidate_path);
                    source = read_text_file(candidate_path);
                    if (source)
                        snprintf(opened_path, sizeof(opened_path), "%s", candidate_path);
                }
            }
        }
    }

    if (!source && is_bare_library_name(resolved_name)) {
        const char* deep_roots[] = { "libs/stdlib", "libs/third_party" };
        const size_t deep_count = sizeof(deep_roots) / sizeof(deep_roots[0]);
        char target_file[512];
        if (has_si_extension(resolved_name)) {
            snprintf(target_file, sizeof(target_file), "%s", resolved_name);
        } else {
            snprintf(target_file, sizeof(target_file), "%s.si", resolved_name);
        }
        char found_path[512];
        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < deep_count && !source; i++) {
                char base_root[512];
                if (!build_rooted_path(base_root, sizeof(base_root), root_prefixes[p], deep_roots[i]))
                    runtime_error("Module path too long", "Resolved module path exceeds internal limit.", "");
                char hint_path[512];
                if (snprintf(hint_path, sizeof(hint_path), "%s/**/%s", base_root, target_file) < (int)sizeof(hint_path)) {
                    record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), hint_path);
                }
                if (find_library_recursive(base_root, target_file, found_path, sizeof(found_path), 8)) {
                    source = read_text_file(found_path);
                    if (source)
                        snprintf(opened_path, sizeof(opened_path), "%s", found_path);
                }
            }
        }
    }

    if (!source) {
        char msg[1024];
        char hint[256];
        char search_hint[2048];
        snprintf(msg, sizeof(msg), "Could not open library `%s`.", resolved_name);
        snprintf(hint, sizeof(hint), "Create the module file and load it with: load library \"%s\"", name);
        build_search_hint(search_hint, sizeof(search_hint), attempts, attempt_count);
        if (search_hint[0]) {
            char combined[2048];
            snprintf(combined, sizeof(combined), "%s\n\n%s", hint, search_hint);
            runtime_error("Library not found", msg, combined);
        } else {
            runtime_error("Library not found", msg, hint);
        }
    }

    push_library_loading(resolved_name);

    int token_count = 0;
    source_push_context();
    source_set_label(opened_path);
    source_load(source);
    Token* library_tokens = lex(source, &token_count);
    ASTNode* library_program = parse(library_tokens, token_count);

    validate_library_program(library_program, resolved_name);
    mark_library_loaded(resolved_name);
    const char* saved_loading_library = current_loading_library;
    current_loading_library = loaded_libraries[loaded_library_count - 1];
    exec_program(library_program);
    current_loading_library = saved_loading_library;
    pop_library_loading();
    source_pop_context();

    lex_free(library_tokens, token_count);
    free(source);
}

static void exec_load_file(ASTNode* n) {
    Value path_value = eval(n->file_load.path);
    if (path_value.type != VALUE_STRING)
        runtime_error("Invalid file path", "load file expects a string path.", "Use: load file \"script.si\"");

    const char* project_root = getenv("SICHT_PROJECT_ROOT");
    char normalized_request[512];
    char resolved_name[512];
    char primary_path[512];
    char fallback_name[512];
    char fallback_path[512];
    char final_path[512];
    char project_path[512];

    normalize_module_path(path_value.string_value, normalized_request, sizeof(normalized_request));
    resolve_file_name(path_value.string_value, resolved_name, sizeof(resolved_name));
    build_library_file_path(resolved_name, primary_path, sizeof(primary_path));

    if (is_file_loading(primary_path)) {
        char msg[1024];
        char chain[1024] = {0};
        for (int i = 0; i < loading_file_count; i++) {
            if (i > 0)
                strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, loading_files[i], sizeof(chain) - strlen(chain) - 1);
        }
        if (loading_file_count > 0)
            strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
        strncat(chain, primary_path, sizeof(chain) - strlen(chain) - 1);
        snprintf(msg, sizeof(msg), "Circular file load detected: ");
        strncat(msg, chain, sizeof(msg) - strlen(msg) - 1);
        runtime_error("Circular file load", msg, "Break the cycle between loaded files.");
    }

    char* source = read_text_file(primary_path);
    snprintf(final_path, sizeof(final_path), "%s", primary_path);

    if (!source && project_root && project_root[0] != '\0') {
        if (!build_rooted_path(project_path, sizeof(project_path), project_root, normalized_request))
            runtime_error("File path too long", "Resolved file path exceeds internal limit.", "");
        build_library_file_path(project_path, project_path, sizeof(project_path));
        if (is_file_loading(project_path)) {
            char msg[1024];
            char chain[1024] = {0};
            for (int i = 0; i < loading_file_count; i++) {
                if (i > 0)
                    strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
                strncat(chain, loading_files[i], sizeof(chain) - strlen(chain) - 1);
            }
            if (loading_file_count > 0)
                strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, project_path, sizeof(chain) - strlen(chain) - 1);
            snprintf(msg, sizeof(msg), "Circular file load detected: ");
            strncat(msg, chain, sizeof(msg) - strlen(msg) - 1);
            runtime_error("Circular file load", msg, "Break the cycle between loaded files.");
        }
        source = read_text_file(project_path);
        if (source)
            snprintf(final_path, sizeof(final_path), "%s", project_path);
    }

    if (!source) {
        normalize_module_path(path_value.string_value, fallback_name, sizeof(fallback_name));
        build_library_file_path(fallback_name, fallback_path, sizeof(fallback_path));
        if (strcmp(fallback_path, primary_path) != 0) {
            if (is_file_loading(fallback_path)) {
                char msg[1024];
                char chain[1024] = {0};
                for (int i = 0; i < loading_file_count; i++) {
                    if (i > 0)
                        strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
                    strncat(chain, loading_files[i], sizeof(chain) - strlen(chain) - 1);
                }
                if (loading_file_count > 0)
                    strncat(chain, " -> ", sizeof(chain) - strlen(chain) - 1);
                strncat(chain, fallback_path, sizeof(chain) - strlen(chain) - 1);
                snprintf(msg, sizeof(msg), "Circular file load detected: ");
                strncat(msg, chain, sizeof(msg) - strlen(msg) - 1);
                runtime_error("Circular file load", msg, "Break the cycle between loaded files.");
            }
            source = read_text_file(fallback_path);
            if (source)
                snprintf(final_path, sizeof(final_path), "%s", fallback_path);
        }
    }

    if (!source) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "Could not open file `%s`.", primary_path);
        runtime_error("File not found", msg, "Check the path passed to `load file`.");
    }

    push_file_loading(final_path);

    int token_count = 0;
    source_push_context();
    source_set_label(final_path);
    source_load(source);
    Token* file_tokens = lex(source, &token_count);
    ASTNode* file_program = parse(file_tokens, token_count);
    exec_program(file_program);
    pop_file_loading();
    source_pop_context();

    lex_free(file_tokens, token_count);
    free(source);
}

static void import_symbol_from_library(
    const char* symbol_name,
    const char* library_name,
    const char* alias_name
) {
    const char* target_name = alias_name ? alias_name : symbol_name;
    char resolved_name[512];
    resolve_library_name(library_name, resolved_name, sizeof(resolved_name));

    if (!is_library_loaded(resolved_name)) {
        char msg[1024];
        char hint[256];
        snprintf(msg, sizeof(msg),
            "Library `%s` is not loaded.",
            resolved_name
        );
        snprintf(hint, sizeof(hint),
            "Load it first: load library \"%s\"",
            library_name
        );
        runtime_error("Library not loaded", msg, hint);
    }

    if (!is_library_offered(resolved_name, symbol_name)) {
        char msg[1024];
        char hint[256];
        snprintf(msg, sizeof(msg),
            "Library `%s` did not offer `%s`.",
            resolved_name,
            symbol_name
        );
        snprintf(hint, sizeof(hint),
            "Inside `%s.si`, add: offer %s",
            library_name,
            symbol_name
        );
        runtime_error("Unavailable symbol", msg, hint);
    }

    FunctionDef* fn = find_function_in_library(resolved_name, symbol_name);
    if (fn) {
        if (!alias_name || strcmp(alias_name, symbol_name) == 0) {
            fn->imported = 1;
            return;
        }

        FunctionDef* existing = find_function_any(target_name);
        if (existing) {
            if (existing->node == fn->node)
                return;

            char msg[256];
            snprintf(msg, sizeof(msg),
                "Cannot import `%s` as `%s` because `%s` is already defined.",
                symbol_name,
                target_name,
                target_name
            );
            runtime_error("Import conflict", msg, "Use a different alias with: take <name> from <library> as <alias>.");
        }

        ASTNode* alias_node = fn->node;
        int alias_is_generator = fn->is_generator;
        ensure_function_capacity();
        functions[function_count].name = target_name;
        functions[function_count].node = alias_node;
        functions[function_count].owner_library = NULL;
        functions[function_count].imported = 1;
        functions[function_count].is_generator = alias_is_generator;
        function_count++;
        return;
    }

    Var* symbol_var = find_global_var(symbol_name);
    if (symbol_var) {
        if (!alias_name || strcmp(alias_name, symbol_name) == 0)
            return;

        if (find_function_any(target_name)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Cannot import value `%s` as `%s` because a function with that name exists.",
                symbol_name,
                target_name
            );
            runtime_error("Import conflict", msg, "Pick a different alias name.");
        }

        Var* alias_var = find_global_var(target_name);
        if (!alias_var)
            alias_var = add_var(target_name, symbol_var->value);
        alias_var->value = symbol_var->value;
        return;
    }

    {
        char msg[1024];
        snprintf(msg, sizeof(msg),
            "Library `%s` offered `%s`, but no matching function or value was found.",
            resolved_name,
            symbol_name
        );
        runtime_error("Invalid offer", msg, "Offer only names that are actually defined in the library.");
    }
}

static void import_all_from_library(const char* library_name) {
    char resolved_name[512];
    resolve_library_name(library_name, resolved_name, sizeof(resolved_name));

    if (!is_library_loaded(resolved_name)) {
        char msg[1024];
        char hint[256];
        snprintf(msg, sizeof(msg),
            "Library `%s` is not loaded.",
            resolved_name
        );
        snprintf(hint, sizeof(hint),
            "Load it first: load library \"%s\"",
            library_name
        );
        runtime_error("Library not loaded", msg, hint);
    }

    int found = 0;
    for (int i = 0; i < library_offer_count; i++) {
        if (strcmp(library_offers[i].library_name, resolved_name) != 0)
            continue;
        found = 1;
        import_symbol_from_library(library_offers[i].symbol_name, library_name, NULL);
    }

    if (!found) {
        char msg[1024];
        char hint[256];
        snprintf(msg, sizeof(msg),
            "Library `%s` did not offer any symbols.",
            resolved_name
        );
        snprintf(hint, sizeof(hint),
            "Inside `%s.si`, add: offer <name>",
            library_name
        );
        runtime_error("No offered symbols", msg, hint);
    }
}

static void exec_block(ASTNode* block) {
    for (int i = 0; i < block->count; i++) {
        if (function_returned)
            return;

        exec_node(block->body[i]);

        gc_poll_budget--;
        if (gc_poll_budget <= 0) {
            gc_poll_budget = 32;
        }

        if (gc_poll_budget == 32 && gc_needs_collect()) {
            int root_count = 0;
            int root_capacity = var_count + 2;
            if (root_capacity < 2)
                root_capacity = 2;
            ensure_gc_roots_capacity(root_capacity);
            for (int r = 0; r < var_count; r++)
                gc_roots_buffer[root_count++] = vars[r].value;
            if (function_has_value)
                gc_roots_buffer[root_count++] = function_last_value;
            if (function_yield_list)
                gc_roots_buffer[root_count++] = value_list(function_yield_list);
            gc_collect(gc_roots_buffer, root_count);
        }

        if (loop_exit || loop_next || function_returned)
            return;
    }
}



static void exec_if(ASTNode* n) {
    for (int i = 0; i < n->branch_count; i++) {
        Value cond = eval(n->branches[i].condition);

        if (cond.type != VALUE_BOOL)
            runtime_error(
                "Invalid if condition",
                "Condition must be boolean.",
                ""
            );

        if (cond.int_value) {
            exec_block(n->branches[i].block);
            return;
        }
    }

    if (n->else_block)
        exec_block(n->else_block);
}



static void exec_while(ASTNode* n) {
    WhileStmt* w = &n->while_stmt;
    int has_limit = 0;
    int max_iterations = 0;
    int iterations = 0;

    if (w->repeat_limit) {
        Value limit = eval(w->repeat_limit);

        if (limit.type != VALUE_INT)
            runtime_error(
                "Invalid while repeat limit",
                "Repeat limit must be an integer.",
                ""
            );

        if (limit.int_value < 0)
            runtime_error(
                "Invalid while repeat limit",
                "Repeat limit cannot be negative.",
                ""
            );

        has_limit = 1;
        max_iterations = limit.int_value;
    }

    while (1) {
        execution_tick();

        if (has_limit && iterations >= max_iterations)
            break;

        Value c = eval(w->condition);

        if (c.type != VALUE_BOOL)
            runtime_error(
                "Invalid while condition",
                "Condition must be boolean.",
                ""
            );

        if (!c.int_value)
            break;

        exec_block(w->body);
        iterations++;
        if (function_returned)
            return;

        if (loop_exit) {
            loop_exit = 0;
            break;
        }

        if (loop_next) {
            loop_next = 0;
            continue;
        }
    }
}

static void exec_repeat(ASTNode* n) {
    RepeatStmt* r = &n->repeat_stmt;

    Value v = eval(r->times);

    if (v.type != VALUE_INT)
        runtime_error(
            "Invalid repeat count",
            "Repeat count must be an integer.",
            ""
        );

    if (v.int_value < 0)
        runtime_error(
            "Invalid repeat count",
            "Repeat count cannot be negative.",
            ""
        );

    for (int i = 0; i < v.int_value; i++) {
        execution_tick();
        exec_block(r->body);
        if (function_returned)
            return;

        if (loop_exit) {
            loop_exit = 0;
            break;
        }

        if (loop_next) {
            loop_next = 0;
            continue;
        }
    }
}

static void exec_for_each(ASTNode* n) {
    Value source = eval(n->for_each_stmt.iterable);

    TempBinding binding;
    temp_binding_begin(n->for_each_stmt.item_name, &binding);

    if (source.type == VALUE_GENERATOR) {
        Value item;
        while (generator_next_value(source.generator_value, &item)) {
            execution_tick();
            temp_binding_set(&binding, item);
            exec_block(n->for_each_stmt.body);
            if (function_returned) {
                temp_binding_end(&binding);
                return;
            }

            if (loop_exit) {
                loop_exit = 0;
                break;
            }

            if (loop_next) {
                loop_next = 0;
                continue;
            }
        }
    } else {
        int count = value_collection_count(source);

        if (count < 0)
            runtime_error(
                "Invalid iterable",
                "for each requires a list, dictionary, string, or generator.",
                ""
            );

        for (int i = 0; i < count; i++) {
            execution_tick();
            temp_binding_set(&binding, value_collection_item(source, i));
            exec_block(n->for_each_stmt.body);
            if (function_returned) {
                temp_binding_end(&binding);
                return;
            }

            if (loop_exit) {
                loop_exit = 0;
                break;
            }

            if (loop_next) {
                loop_next = 0;
                continue;
            }
        }
    }

    temp_binding_end(&binding);
}

static void exec_match(ASTNode* n) {
    Value target = eval(n->match_stmt.target);

    for (int i = 0; i < n->match_stmt.branch_count; i++) {
        Value candidate = eval(n->match_stmt.branches[i].value);
        if (value_equals(target, candidate)) {
            exec_block(n->match_stmt.branches[i].block);
            return;
        }
    }

    if (n->match_stmt.otherwise_block)
        exec_block(n->match_stmt.otherwise_block);
}

typedef struct {
    int var_count;
    int frame_depth;
    int in_function_call;
    int function_has_value;
    int function_returned;
    Value function_last_value;
    List* function_yield_list;
    int function_yield_used;
    int function_yield_count;
    int loading_library_count;
    int loading_file_count;
    int loop_exit;
    int loop_next;
} TrySnapshot;

static void capture_try_snapshot(TrySnapshot* snap) {
    snap->var_count = var_count;
    snap->frame_depth = frame_depth;
    snap->in_function_call = in_function_call;
    snap->function_has_value = function_has_value;
    snap->function_returned = function_returned;
    snap->function_last_value = function_last_value;
    snap->function_yield_list = function_yield_list;
    snap->function_yield_used = function_yield_used;
    snap->function_yield_count = function_yield_list ? function_yield_list->count : 0;
    snap->loading_library_count = loading_library_count;
    snap->loading_file_count = loading_file_count;
    snap->loop_exit = loop_exit;
    snap->loop_next = loop_next;
}

static void restore_try_snapshot(const TrySnapshot* snap) {
    for (int i = snap->var_count; i < var_count; i++) {
        free(vars[i].name);
        vars[i].name = NULL;
    }
    var_count = snap->var_count;
    frame_depth = snap->frame_depth;
    in_function_call = snap->in_function_call;
    function_has_value = snap->function_has_value;
    function_returned = snap->function_returned;
    function_last_value = snap->function_last_value;
    function_yield_list = snap->function_yield_list;
    function_yield_used = snap->function_yield_used;
    if (function_yield_list)
        function_yield_list->count = snap->function_yield_count;
    for (int i = snap->loading_library_count; i < loading_library_count; i++) {
        free(loading_libraries[i]);
        loading_libraries[i] = NULL;
    }
    loading_library_count = snap->loading_library_count;
    for (int i = snap->loading_file_count; i < loading_file_count; i++) {
        free(loading_files[i]);
        loading_files[i] = NULL;
    }
    loading_file_count = snap->loading_file_count;
    loop_exit = snap->loop_exit;
    loop_next = snap->loop_next;
}

static void exec_try(ASTNode* n) {
    TrySnapshot snapshot;
    capture_try_snapshot(&snapshot);

    jmp_buf env;
    if (runtime_try_push(&env) < 0)
        runtime_error("Too many nested try blocks", "try nesting limit reached.", "");

    if (setjmp(env) == 0) {
        exec_block(n->try_stmt.try_block);
        runtime_try_pop();
        return;
    }

    runtime_try_pop();
    restore_try_snapshot(&snapshot);

    if (n->try_stmt.otherwise_block)
        exec_block(n->try_stmt.otherwise_block);
}

static void exec_file_write(ASTNode* n, int append_mode) {
    Value path = eval(n->file_io_stmt.path);
    Value content = eval(n->file_io_stmt.content);

    if (path.type != VALUE_STRING)
        runtime_error("Invalid file path", "File path must be a string.", "");
    if (content.type != VALUE_STRING)
        runtime_error("Invalid file content", "File content must be a string.", "");

    FILE* f = fopen(path.string_value, append_mode ? "ab" : "wb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not open `%s` for writing.", path.string_value);
        runtime_error("File write failed", msg, "");
    }

    size_t len = strlen(content.string_value);
    size_t written = fwrite(content.string_value, 1, len, f);
    fclose(f);

    if (written != len)
        runtime_error("File write failed", "Could not write full content to file.", "");
}


static void exec_node(ASTNode* n) {
    if (function_returned)
        return;
    runtime_error_set_location(n->line, n->column);
    execution_tick();

    if (debugger_is_enabled()) {
        const char* label = source_get_label();
        char stack_json[8192];
        char locals_json[8192];
        build_debug_stack_json(label ? label : "", n->line, n->column, stack_json, sizeof(stack_json));
        build_debug_locals_json(locals_json, sizeof(locals_json));
        debugger_on_statement(label ? label : "", n->line, n->column, stack_json, locals_json);
    }

    if (trace_enabled) {
        const char* label = source_get_label();
        if (label && label[0])
            printf("[trace] %s: %s", label, ast_type_name(n->type));
        else
            printf("[trace] %s", ast_type_name(n->type));
        if (n->type == AST_SET && n->name)
            printf(" name=%s", n->name);
        else if (n->type == AST_LIST_ADD && n->list_add.list_name)
            printf(" list=%s", n->list_add.list_name);
        else if (n->type == AST_LIST_REMOVE && n->list_remove.list_name)
            printf(" list=%s", n->list_remove.list_name);
        else if (n->type == AST_DICT_ADD && n->dict_add.dict_name)
            printf(" dict=%s", n->dict_add.dict_name);
        else if (n->type == AST_DICT_REMOVE && n->dict_remove.dict_name)
            printf(" dict=%s", n->dict_remove.dict_name);
        else if (n->type == AST_DICT_CLEAR && n->dict_clear.dict_name)
            printf(" dict=%s", n->dict_clear.dict_name);
        else if (n->type == AST_FUNCTION && n->function_decl.name)
            printf(" name=%s", n->function_decl.name);
        else if (n->type == AST_FOR_EACH && n->for_each_stmt.item_name)
            printf(" item=%s", n->for_each_stmt.item_name);
        else if (n->type == AST_LOAD_LIBRARY && n->library_load.name)
            printf(" name=%s", n->library_load.name);
        else if (n->type == AST_LOAD_FILE)
            printf(" file-load");
        else if (n->type == AST_LIBRARY_OFFER && n->library_offer.name)
            printf(" name=%s", n->library_offer.name);
        else if (n->type == AST_LIBRARY_TAKE && n->library_take.name)
            printf(" name=%s from=%s alias=%s",
                n->library_take.name,
                n->library_take.library_name,
                n->library_take.alias_name ? n->library_take.alias_name : "-");
        else if (n->type == AST_LIBRARY_TAKE_ALL && n->library_take_all.library_name)
            printf(" all from=%s", n->library_take_all.library_name);
        else if (n->type == AST_TYPE_DECL && n->type_decl.name)
            printf(" name=%s fields=%d", n->type_decl.name, n->type_decl.field_count);
        else if ((n->type == AST_FILE_WRITE || n->type == AST_FILE_APPEND))
            printf(" file-op");
        else if (n->type == AST_RETURN)
            printf(" return");
        printf("\n");
    }

    switch (n->type) {

        case AST_SET: {
            Value v = eval(n->expr);
            Var* var = NULL;
            if (frame_depth > 0) {
                var = find_local_var(n->name);
                if (!var)
                    var = add_var(n->name, v);
            } else {
                var = find_var(n->name);
                if (!var)
                    var = add_var(n->name, v);
            }

            var->value = v;
            break;
        }

        case AST_PRINT_EXPR: {
            Value v = eval(n->expr);
            print_value_inline(v);
            printf("\n");
            fflush(stdout);
            break;
        }



        case AST_PRINT_STRING: {
            for (int i = 0; i < n->part_count; i++) {
                StringPart* sp = &n->parts[i];
                if (sp->type == STR_TEXT)
                    printf("%s", sp->text);
                else {
                    Value v = eval(sp->expr);
                    print_value_inline(v);
                }
            }
            printf("\n");
            fflush(stdout);
            break;
        }

        case AST_LLVL_BLOCK:
            llvl_enter();
            exec_block(n);
            llvl_exit();
            break;

        case AST_LLVL_SAVE: {
            require_llvl("Low-level operation");
            Value size_val = eval(n->llvl_save.size);
            Value out;

            if (n->llvl_save.has_type) {
                size_t elem_size = llvl_type_size_from_kind(n->llvl_save.elem_kind, n->llvl_save.elem_type_name);
                size_t count = llvl_size_from_value(size_val, "Invalid buffer count");
                if (elem_size > 0 && count > SIZE_MAX / elem_size)
                    runtime_error("Invalid buffer size", "Typed buffer is too large.", "");
                size_t bytes = elem_size * count;
                Value alloc_size = bytes > (size_t)INT_MAX ? value_float((double)bytes) : value_int((int)bytes);
                rt_llvl_alloc(&out, &alloc_size);
                rt_llvl_set_buffer_meta(&out, elem_size, (int)llvl_kind_to_buffer_kind(n->llvl_save.elem_kind), n->llvl_save.elem_type_name);
            } else {
                rt_llvl_alloc(&out, &size_val);
                rt_llvl_set_buffer_meta(&out, 1, BUFFER_ELEM_RAW, NULL);
            }

            Var* var = NULL;
            if (frame_depth > 0) {
                var = find_local_var(n->llvl_save.name);
                if (!var)
                    var = add_var(n->llvl_save.name, out);
            } else {
                var = find_var(n->llvl_save.name);
                if (!var)
                    var = add_var(n->llvl_save.name, out);
            }
            var->value = out;
            break;
        }

        case AST_LLVL_REMOVE: {
            require_llvl("Low-level operation");
            Var* var = find_var(n->llvl_remove.name);
            if (!var)
                runtime_error("Undefined variable", "Buffer variable does not exist.", "");
            rt_llvl_free(&var->value);
            break;
        }

        case AST_LLVL_RESIZE: {
            require_llvl("Low-level operation");
            Var* var = find_var(n->llvl_resize.name);
            if (!var)
                runtime_error("Undefined variable", "Buffer variable does not exist.", "");
            Value size_val = eval(n->llvl_resize.size);

            if (n->llvl_resize.has_type) {
                size_t elem_size = llvl_type_size_from_kind(n->llvl_resize.elem_kind, n->llvl_resize.elem_type_name);
                size_t count = llvl_size_from_value(size_val, "Invalid resize count");
                if (elem_size > 0 && count > SIZE_MAX / elem_size)
                    runtime_error("Invalid resize size", "Typed buffer is too large.", "");
                size_t bytes = elem_size * count;
                size_val = bytes > (size_t)INT_MAX ? value_float((double)bytes) : value_int((int)bytes);
                rt_llvl_set_buffer_meta(&var->value, elem_size, (int)llvl_kind_to_buffer_kind(n->llvl_resize.elem_kind), n->llvl_resize.elem_type_name);
            }

            if (n->llvl_resize.op == LLVL_RESIZE_ANY) {
                rt_llvl_resize_any(&var->value, &size_val);
            } else {
                int is_grow = n->llvl_resize.op == LLVL_RESIZE_GROW ? 1 : 0;
                rt_llvl_resize(&var->value, &size_val, is_grow);
            }
            break;
        }

        case AST_LLVL_COPY: {
            require_llvl("Low-level operation");
            Var* src = find_var(n->llvl_copy.src);
            if (!src)
                runtime_error("Undefined variable", "Source buffer does not exist.", "");
            if (n->llvl_copy.has_size) {
                Var* dst = find_var(n->llvl_copy.dest);
                if (!dst)
                    runtime_error("Undefined variable", "Destination buffer does not exist.", "");
                Value size = eval(n->llvl_copy.size);
                rt_llvl_copy_bytes(&src->value, &dst->value, &size);
            } else {
                Value out;
                rt_llvl_copy(&out, &src->value);
                Var* dst = NULL;
                if (frame_depth > 0) {
                    dst = find_local_var(n->llvl_copy.dest);
                    if (!dst)
                        dst = add_var(n->llvl_copy.dest, out);
                } else {
                    dst = find_var(n->llvl_copy.dest);
                    if (!dst)
                        dst = add_var(n->llvl_copy.dest, out);
                }
                dst->value = out;
            }
            break;
        }

        case AST_LLVL_MOVE: {
            require_llvl("Low-level operation");
            Var* src = find_var(n->llvl_move.src);
            if (!src)
                runtime_error("Undefined variable", "Source buffer does not exist.", "");
            Var* dst = NULL;
            if (frame_depth > 0) {
                dst = find_local_var(n->llvl_move.dest);
                if (!dst)
                    dst = add_var(n->llvl_move.dest, value_buffer(NULL));
            } else {
                dst = find_var(n->llvl_move.dest);
                if (!dst)
                    dst = add_var(n->llvl_move.dest, value_buffer(NULL));
            }
            rt_llvl_move(&dst->value, &src->value);
            break;
        }

        case AST_LLVL_SET_VALUE: {
            require_llvl("Low-level operation");
            Var* var = find_var(n->llvl_set_value.name);
            if (!var)
                runtime_error("Undefined variable", "Buffer variable does not exist.", "");
            Value value = eval(n->llvl_set_value.value);
            rt_llvl_set_value(&var->value, &value);
            break;
        }

        case AST_LLVL_SET_BYTE: {
            require_llvl("Low-level operation");
            Var* var = find_var(n->llvl_set_byte.name);
            if (!var)
                runtime_error("Undefined variable", "Buffer variable does not exist.", "");
            Value index = eval(n->llvl_set_byte.index);
            Value value = eval(n->llvl_set_byte.value);
            rt_llvl_set_byte(&var->value, &index, &value);
            break;
        }

        case AST_LLVL_BIT_OP: {
            require_llvl("Low-level operation");
            Var* var = find_var(n->llvl_bit_op.name);
            if (!var)
                runtime_error("Undefined variable", "Buffer variable does not exist.", "");
            Value index = eval(n->llvl_bit_op.index);
            int op = 0;
            if (n->llvl_bit_op.op == LLVL_BIT_CLEAR)
                op = 1;
            else if (n->llvl_bit_op.op == LLVL_BIT_FLIP)
                op = 2;
            rt_llvl_bit_op(&var->value, &index, op);
            break;
        }

        case AST_LLVL_SET_AT: {
            require_llvl("Low-level operation");
            Value addr = eval(n->llvl_set_at.address);
            Value value = eval(n->llvl_set_at.value);
            rt_llvl_set_at(&addr, &value);
            break;
        }

        case AST_LLVL_ATOMIC_OP: {
            require_llvl("Low-level operation");
            Value addr = eval(n->llvl_atomic_op.address);
            if (n->llvl_atomic_op.op == LLVL_ATOMIC_WRITE) {
                Value value = eval(n->llvl_atomic_op.value);
                rt_llvl_set_at(&addr, &value);
                break;
            }

            Value current;
            rt_llvl_get_at(&current, &addr);
            Value delta = eval(n->llvl_atomic_op.value);
            Value out;
            if (n->llvl_atomic_op.op == LLVL_ATOMIC_ADD) {
                rt_add(&out, &current, &delta);
            } else {
                rt_sub(&out, &current, &delta);
            }
            rt_llvl_set_at(&addr, &out);
            break;
        }

        case AST_LLVL_MARK_VOLATILE: {
            require_llvl("Low-level operation");
            Value target = eval(n->llvl_mark_volatile.target);
            if (target.type != VALUE_ADDRESS && target.type != VALUE_BUFFER)
                runtime_error(
                    "Invalid volatile target",
                    "Volatile marker expects a buffer or address.",
                    ""
                );
            break;
        }

        case AST_LLVL_SET_CHECK: {
            require_llvl("Low-level operation");
            if (n->llvl_set_check.kind == LLVL_CHECK_BOUNDS) {
                rt_llvl_set_bounds_check(n->llvl_set_check.enabled);
            } else {
                rt_llvl_set_pointer_checks(n->llvl_set_check.enabled);
            }
            break;
        }

        case AST_LLVL_PORT_WRITE: {
            require_llvl("Low-level operation");
            Value port = eval(n->llvl_port_write.port);
            Value value = eval(n->llvl_port_write.value);
            rt_llvl_port_write(&port, &value);
            break;
        }

        case AST_LLVL_REGISTER_INTERRUPT: {
            require_llvl("Low-level operation");
            Value interrupt_id = eval(n->llvl_register_interrupt.interrupt_id);
            rt_llvl_register_interrupt(&interrupt_id, n->llvl_register_interrupt.handler_name);
            break;
        }

        case AST_LLVL_SET_FIELD: {
            require_llvl("Low-level operation");
            Value target = eval(n->llvl_set_field.target);
            Value value = eval(n->llvl_set_field.value);
            llvl_write_field_value(&target, n->llvl_set_field.field_name, value);
            break;
        }

        case AST_LLVL_PIN_WRITE: {
            require_llvl("Low-level operation");
            Value value = eval(n->llvl_pin_write.value);
            Value pin = eval(n->llvl_pin_write.pin);
            rt_llvl_pin_write(&value, &pin);
            break;
        }

        case AST_LLVL_WAIT: {
            require_llvl("Low-level operation");
            Value duration = eval(n->llvl_wait.duration);
            rt_llvl_wait_ms(&duration);
            break;
        }

        case AST_LLVL_STRUCT_DECL:
            llvl_register_struct_decl(n, 0);
            break;

        case AST_LLVL_UNION_DECL:
            llvl_register_struct_decl(n, 1);
            break;

        case AST_LLVL_ENUM_DECL:
            llvl_register_enum_decl(n);
            break;

        case AST_LLVL_BITFIELD_DECL:
            llvl_register_bitfield_decl(n);
            break;

case AST_SET_ELEMENT: {

    Var* var = find_var(n->set_element.name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "List variable does not exist.",
            ""
        );

    if (var->value.type != VALUE_LIST)
        runtime_error(
            "Invalid list assignment",
            "Target variable is not a list.",
            ""
        );

    Value index_val = eval(n->set_element.index);
    Value value = eval(n->set_element.value);

    if (index_val.type != VALUE_INT)
        runtime_error(
            "Invalid index",
            "List index must be an integer.",
            ""
        );

    int idx = index_val.int_value;

    if (idx < 0 || idx >= var->value.list_value->count)
        runtime_error(
            "Index out of bounds",
            "List index outside range.",
            ""
        );

    var->value.list_value->items[idx] = value;

    break;
}

case AST_LIST_REMOVE_ELEMENT: {

    Var* var = find_var(n->list_remove_element.list_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "List variable does not exist.",
            ""
        );

    if (var->value.type != VALUE_LIST)
        runtime_error(
            "Invalid list operation",
            "Target variable is not a list.",
            ""
        );

    Value idx_val = eval(n->list_remove_element.index);

    if (idx_val.type != VALUE_INT)
        runtime_error(
            "Invalid index",
            "List index must be an integer.",
            ""
        );

    int idx = idx_val.int_value;
    List* list = var->value.list_value;

    if (idx < 0 || idx >= list->count)
        runtime_error(
            "Index out of bounds",
            "List index outside range.",
            ""
        );

    for (int i = idx; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];

    list->count--;

    break;
}


case AST_LIST_ADD: {

    Var* var = find_var(n->list_add.list_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "List variable does not exist.",
            ""
        );

    if (var->value.type != VALUE_LIST)
        runtime_error(
            "Invalid list operation",
            "Target variable is not a list.",
            ""
        );

    Value v = eval(n->list_add.value);

    List* list = var->value.list_value;
    gc_list_reserve(list, list->count + 1);

    list->items[list->count] = v;
    list->count++;

    break;
}

case AST_LIST_REMOVE: {

    Var* var = find_var(n->list_remove.list_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "List variable does not exist.",
            ""
        );

    if (var->value.type != VALUE_LIST)
        runtime_error(
            "Invalid list operation",
            "Target variable is not a list.",
            ""
        );

    Value target = eval(n->list_remove.value);

    List* list = var->value.list_value;

    int found = -1;

    for (int i = 0; i < list->count; i++) {

        Value item = list->items[i];

        if (item.type != target.type)
            continue;

        if (
            (item.type == VALUE_INT    && item.int_value   == target.int_value)   ||
            (item.type == VALUE_FLOAT  && item.float_value == target.float_value) ||
            (item.type == VALUE_BOOL   && item.int_value   == target.int_value)   ||
            (item.type == VALUE_STRING && strcmp(item.string_value, target.string_value) == 0)
        ) {
            found = i;
            break;
        }
    }

    if (found == -1)
        return;   

    for (int i = found; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];

    list->count--;

    break;
}       


case AST_DICT_ADD: {

    Var* var = find_var(n->dict_add.dict_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "Dictionary variable does not exist.",
            "Create it first, for example: set book to {}"
        );

    if (var->value.type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Variable `%s` is `%s`, not a dictionary.",
            n->dict_add.dict_name,
            value_type_name(var->value.type)
        );
        runtime_error(
            "Invalid dictionary operation",
            msg,
            "Use a dictionary variable, for example: set book to { \"name\": 1 }"
        );
    }

    Value key = eval(n->dict_add.key);
    Value value = eval(n->dict_add.value);

    if (key.type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            value_type_name(key.type)
        );
        runtime_error(
            "Invalid dictionary key",
            msg,
            "Use quoted keys like: add item \"name\": value to(dict)"
        );
    }

    Dict* d = var->value.dict_value;
    dict_set_entry_gc(d, key.string_value, value);

    break;
}

case AST_DICT_REMOVE: {
    Var* var = find_var(n->dict_remove.dict_name);
    if (!var)
        runtime_error("Undefined variable", "Dictionary variable does not exist.", "Create it first, for example: set book to {}");
    if (var->value.type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Variable `%s` is `%s`, not a dictionary.",
            n->dict_remove.dict_name,
            value_type_name(var->value.type)
        );
        runtime_error("Invalid dictionary operation", msg, "Use a dictionary variable with remove item.");
    }

    Value key = eval(n->dict_remove.key);
    if (key.type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            value_type_name(key.type)
        );
        runtime_error(
            "Invalid dictionary key",
            msg,
            "Use quoted keys like: remove item \"name\" from(dict)"
        );
    }

    Dict* dict = var->value.dict_value;
    if (!dict_remove_entry(dict, key.string_value)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Key `%s` does not exist in dictionary.", key.string_value);
        runtime_error("Missing dictionary item", msg, "Check the key spelling before removing.");
    }
    break;
}
case AST_DICT_CLEAR: {
    Var* var = find_var(n->dict_clear.dict_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "Dictionary variable does not exist.",
            "Create it first, for example: set book to {}"
        );

    if (var->value.type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Variable `%s` is `%s`, not a dictionary.",
            n->dict_clear.dict_name,
            value_type_name(var->value.type)
        );
        runtime_error(
            "Invalid dictionary operation",
            msg,
            "Use clear(dict_name) only on dictionary variables."
        );
    }

    Dict* dict = var->value.dict_value;
    dict_clear_entries(dict);
    break;
}

case AST_DICT_CONTAINS_ITEM: {
    Var* var = find_var(n->dict_contains.dict_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "Dictionary variable does not exist.",
            "Create it first, for example: set book to {}"
        );

    if (var->value.type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Variable `%s` is `%s`, not a dictionary.",
            n->dict_contains.dict_name,
            value_type_name(var->value.type)
        );
        runtime_error(
            "Invalid dictionary operation",
            msg,
            "Use dictionary membership checks only with dictionaries."
        );
    }

    Value key = eval(n->dict_contains.key);
    if (key.type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            value_type_name(key.type)
        );
        runtime_error(
            "Invalid dictionary key",
            msg,
            "Use quoted keys like: \"name\" in(dict)"
        );
    }

    Dict* dict = var->value.dict_value;
    (void)dict_find_index(dict, key.string_value);

    break;
}



case AST_LIST_CLEAR: {

    Var* var = find_var(n->list_clear.list_name);

    if (!var)
        runtime_error(
            "Undefined variable",
            "List variable does not exist.",
            ""
        );

    if (var->value.type == VALUE_LIST) {
        var->value.list_value->count = 0;
        break;
    }
    if (var->value.type == VALUE_DICT) {
        dict_clear_entries(var->value.dict_value);
        break;
    }
    runtime_error(
        "Invalid clear operation",
        "Target variable is not a list or dictionary.",
        ""
    );

    break;
}


        case AST_BLOCK:
            exec_block(n);
            break;
        case AST_IF:
            exec_if(n);
            break;
        case AST_TRY:
            exec_try(n);
            break;

        case AST_WHILE:
            exec_while(n);
            break;

        case AST_REPEAT:
            exec_repeat(n);
            break;
        case AST_FOR_EACH:
            exec_for_each(n);
            break;
        case AST_MATCH:
            exec_match(n);
            break;
        case AST_FUNCTION:
            
            break;
        case AST_YIELD: {
            if (!in_function_call)
                runtime_error(
                    "Invalid yield",
                    "`yield` can only be used inside a function.",
                    "Use `return` in regular function flow, or place `yield` inside a function body."
                );
            if (function_returned)
                runtime_error(
                    "Invalid function flow",
                    "Cannot use `yield` after `return` in the same function.",
                    "Use either `yield` statements or `return`, but not both."
                );

            if (!function_yield_list)
                function_yield_list = gc_list_new(4);

            if (generator_yield_limit > 0 &&
                (long long)function_yield_list->count >= generator_yield_limit) {
                runtime_error(
                    "Generator yield limit exceeded",
                    "This generator produced more values than the configured limit.",
                    "Raise SICHT_MAX_GENERATOR_ITEMS or make the generator terminate sooner."
                );
            }

            Value yielded = eval(n->expr);
            gc_list_reserve(function_yield_list, function_yield_list->count + 1);
            function_yield_list->items[function_yield_list->count++] = yielded;
            function_yield_used = 1;
            function_has_value = 1;
            break;
        }
        case AST_RETURN:
            if (!in_function_call)
                runtime_error("Invalid return", "return can only be used inside a function.", "");
            if (function_yield_used)
                runtime_error(
                    "Invalid function flow",
                    "Cannot use `return` after `yield` in the same function.",
                    "Use either `yield` statements or `return`, but not both."
                );
            function_last_value = eval(n->return_stmt.value);
            function_has_value = 1;
            function_returned = 1;
            break;
        case AST_CREATE_LIBRARY:
            
            break;
        case AST_TYPE_DECL:
            
            break;
        case AST_LIBRARY_OFFER:
            if (!current_loading_library) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "`offer %s` can only be used in a library file.",
                    n->library_offer.name
                );
                runtime_error(
                    "Invalid offer",
                    msg,
                    "Move this into a file that contains `create library <name>`."
                );
            }
            add_library_offer(current_loading_library, n->library_offer.name);
            break;
        case AST_LOAD_LIBRARY:
            load_library_by_name(n->library_load.name);
            break;
        case AST_LOAD_FILE:
            exec_load_file(n);
            break;
        case AST_LIBRARY_TAKE:
            import_symbol_from_library(
                n->library_take.name,
                n->library_take.library_name,
                n->library_take.alias_name
            );
            break;
        case AST_LIBRARY_TAKE_ALL:
            import_all_from_library(n->library_take_all.library_name);
            break;
        case AST_FILE_WRITE:
            exec_file_write(n, 0);
            break;
        case AST_FILE_APPEND:
            exec_file_write(n, 1);
            break;
        case AST_EXIT:
    loop_exit = 1;
    break;

case AST_NEXT:
    loop_next = 1;
    break;
        case AST_EXPR_STMT: {
            Value out = eval(n->expr);
            if (in_function_call) {
                function_last_value = out;
                function_has_value = 1;
            }
            break;
}

        default:
            break;
    }
}

static void exec_program(ASTNode* program) {
    for (int i = 0; i < program->count; i++) {
        runtime_error_set_location(program->body[i]->line, program->body[i]->column);
        if (program->body[i]->type == AST_FUNCTION)
            register_function(program->body[i]);
        if (program->body[i]->type == AST_TYPE_DECL)
            register_type(program->body[i]);
    }

    if (program->llvl_mode)
        llvl_enter();
    exec_block(program);
    if (program->llvl_mode)
        llvl_exit();
}

void execute(ASTNode* program) {
    interpreter_reset();
    execute_incremental(program);
}

void execute_incremental(ASTNode* program) {
    loop_exit = 0;
    loop_next = 0;
    exec_program(program);
}

static int starts_with(const char* value, const char* prefix) {
    if (!prefix || prefix[0] == '\0')
        return 1;
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

static int append_unique_symbol(const char** out, int count, int max_out, const char* symbol) {
    if (!symbol || symbol[0] == '\0')
        return count;

    for (int i = 0; i < count; i++) {
        if (strcmp(out[i], symbol) == 0)
            return count;
    }

    if (count < max_out)
        out[count++] = symbol;
    return count;
}

void interpreter_dump_vars(FILE* out) {
    if (!out)
        return;

    if (var_count == 0) {
        fprintf(out, "(no variables)\n");
        return;
    }

    for (int i = 0; i < var_count; i++) {
        fprintf(out, "%s : %s\n",
            vars[i].name,
            value_type_name(vars[i].value.type)
        );
    }
}

void interpreter_dump_functions(FILE* out) {
    if (!out)
        return;

    int shown = 0;
    for (int i = 0; i < function_count; i++) {
        FunctionDef* fn = &functions[i];
        if (!function_is_accessible(fn))
            continue;

        int min_args = fn->node->function_decl.required_param_count;
        int max_args = fn->node->function_decl.param_count;
        fprintf(out, "%s(%d..%d)%s\n",
            fn->name,
            min_args,
            max_args,
            fn->is_generator ? " [generator]" : ""
        );
        shown++;
    }

    if (shown == 0)
        fprintf(out, "(no functions)\n");
}

int interpreter_collect_symbols(const char* prefix, const char** out, int max_out) {
    if (!out || max_out <= 0)
        return 0;

    int count = 0;

    for (int i = 0; i < var_count; i++) {
        if (starts_with(vars[i].name, prefix))
            count = append_unique_symbol(out, count, max_out, vars[i].name);
    }

    for (int i = 0; i < function_count; i++) {
        FunctionDef* fn = &functions[i];
        if (!function_is_accessible(fn))
            continue;
        if (starts_with(fn->name, prefix))
            count = append_unique_symbol(out, count, max_out, fn->name);
    }

    for (int i = 0; i < library_offer_count; i++) {
        const char* symbol = library_offers[i].symbol_name;
        if (starts_with(symbol, prefix))
            count = append_unique_symbol(out, count, max_out, symbol);
    }

    return count;
}

const char* interpreter_eval_expr_type(Expr* expr) {
    if (!expr)
        return "unknown";
    Value out = eval(expr);
    return value_type_name(out.type);
}

static void print_value_inline(Value v) {
    switch (v.type) {
        case VALUE_STRING:
            printf("%s", v.string_value);
            return;
        case VALUE_BOOL:
            printf(v.int_value ? "true" : "false");
            return;
        case VALUE_FLOAT:
            printf("%g", v.float_value);
            return;
        case VALUE_INT:
            printf("%d", v.int_value);
            return;
        case VALUE_BUFFER: {
            size_t size = v.buffer_value ? v.buffer_value->size : 0;
            printf("<buffer %zu bytes>", size);
            return;
        }
        case VALUE_ADDRESS:
            printf("<address %p>", v.address_value);
            return;
        case VALUE_LIST: {
            printf("[");
            for (int i = 0; i < v.list_value->count; i++) {
                print_value_inline(v.list_value->items[i]);
                if (i < v.list_value->count - 1)
                    printf(", ");
            }
            printf("]");
            return;
        }
        case VALUE_DICT: {
            printf("{");
            for (int i = 0; i < v.dict_value->count; i++) {
                DictEntry* e = &v.dict_value->entries[i];
                printf("\"%s\": ", e->key);
                print_value_inline(e->value);
                if (i < v.dict_value->count - 1)
                    printf(", ");
            }
            printf("}");
            return;
        }
        case VALUE_GENERATOR: {
            Generator* g = v.generator_value;
            if (!g) {
                printf("<generator:null>");
                return;
            }
            printf("<generator index=%d", g->index);
            if (g->initialized && g->cache)
                printf(" remaining=%d", g->cache->count - g->index);
            printf(">");
            return;
        }
    }
}


