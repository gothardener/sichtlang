#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
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
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
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

#include "native_rt.h"
#include "gc.h"
#include "value.h"
#include "expr.h"
#include "runtime_error.h"

static const char* rt_type_name(ValueType type) {
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
    }
    return "unknown";
}

static void rt_null_value_error(void) {
    runtime_error("Runtime failure", "Null value pointer in native runtime.", "Report this as a compiler bug.");
}

static int rt_truthy_value(Value v);
static Value rt_cast_string_value(Value v);

#define MAX_DEEP_EQUALS_DEPTH 64

static Value** rt_gc_roots = NULL;
static int rt_gc_root_count = 0;
static int rt_gc_root_capacity = 0;
static Value* rt_gc_root_values = NULL;
static int rt_gc_root_values_capacity = 0;
static int rt_gc_globals_registered = 0;
static int rt_gc_trace_cached = -1;
static int rt_gc_disable_cached = -1;
static int rt_rt_trace_cached = -1;
static long long rt_step_limit = -1;
static long long rt_step_count = 0;
static long long rt_generator_limit = -1;

static char** rt_cli_args = NULL;
static int rt_cli_argc = 0;

static int rt_env_flag_cached(const char* name, int* cache) {
    if (!name || !cache)
        return 0;
    if (*cache >= 0)
        return *cache;
    const char* env = getenv(name);
    *cache = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return *cache;
}

static int rt_trace_enabled(void) {
    return rt_env_flag_cached("SICHT_RT_TRACE", &rt_rt_trace_cached);
}

static void rt_trace(const char* label, const void* ptr, int a, int b) {
    if (!rt_trace_enabled())
        return;
    fprintf(stderr, "[rt] %s ptr=%p a=%d b=%d\n", label, ptr, a, b);
}

static long long rt_read_step_limit(void) {
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

static long long rt_read_generator_limit(void) {
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

void rt_step_tick(void) {
    if (rt_step_limit < 0)
        rt_step_limit = rt_read_step_limit();
    if (rt_step_limit <= 0)
        return;
    rt_step_count++;
    if (rt_step_count > rt_step_limit) {
        runtime_error(
            "Execution step limit exceeded",
            "Program exceeded configured step budget.",
            "Raise SICHT_MAX_STEPS or simplify the loop/control flow."
        );
    }
}

static void rt_step_tick_bulk(long long count) {
    if (count <= 0)
        return;
    if (rt_step_limit < 0)
        rt_step_limit = rt_read_step_limit();
    if (rt_step_limit <= 0)
        return;
    if (count > LLONG_MAX - rt_step_count) {
        runtime_error(
            "Execution step limit exceeded",
            "Program exceeded configured step budget.",
            "Raise SICHT_MAX_STEPS or simplify the loop/control flow."
        );
    }
    if (rt_step_count > rt_step_limit - count) {
        runtime_error(
            "Execution step limit exceeded",
            "Program exceeded configured step budget.",
            "Raise SICHT_MAX_STEPS or simplify the loop/control flow."
        );
    }
    rt_step_count += count;
}

static void rt_gc_ensure_root_capacity(int needed) {
    if (needed <= rt_gc_root_capacity)
        return;
    int new_cap = rt_gc_root_capacity > 0 ? rt_gc_root_capacity : 64;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2)
            runtime_error("GC root limit", "Root table exceeded supported size.", "");
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(Value*))
        runtime_error("GC root limit", "Root table exceeded supported size.", "");
    Value** grown = (Value**)realloc(rt_gc_roots, sizeof(Value*) * (size_t)new_cap);
    if (!grown)
        runtime_error("Out of memory", "Could not grow GC root table.", "");
    rt_gc_roots = grown;
    rt_gc_root_capacity = new_cap;
}

static void rt_gc_ensure_root_values_capacity(int needed) {
    if (needed <= rt_gc_root_values_capacity)
        return;
    int new_cap = rt_gc_root_values_capacity > 0 ? rt_gc_root_values_capacity : 64;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2)
            runtime_error("GC root limit", "Root snapshot exceeded supported size.", "");
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(Value))
        runtime_error("GC root limit", "Root snapshot exceeded supported size.", "");
    Value* grown = (Value*)realloc(rt_gc_root_values, sizeof(Value) * (size_t)new_cap);
    if (!grown)
        runtime_error("Out of memory", "Could not grow GC root snapshot.", "");
    rt_gc_root_values = grown;
    rt_gc_root_values_capacity = new_cap;
}

void rt_gc_register_globals(Value* globals, int count) {
    if (rt_gc_globals_registered)
        return;
    if (!globals || count <= 0) {
        rt_gc_globals_registered = 1;
        return;
    }
    rt_gc_ensure_root_capacity(rt_gc_root_count + count);
    for (int i = 0; i < count; i++)
        rt_gc_roots[rt_gc_root_count++] = &globals[i];
    rt_gc_globals_registered = 1;
}

void rt_gc_push_root(Value* value) {
    if (!value)
        return;
    rt_gc_ensure_root_capacity(rt_gc_root_count + 1);
    rt_gc_roots[rt_gc_root_count++] = value;
}

void rt_gc_pop_roots(int count) {
    if (count <= 0)
        return;
    if (count >= rt_gc_root_count) {
        rt_gc_root_count = 0;
        return;
    }
    rt_gc_root_count -= count;
}

void rt_gc_maybe_collect(void) {
    if (rt_env_flag_cached("SICHT_GC_DISABLE", &rt_gc_disable_cached))
        return;
    if (!gc_needs_collect())
        return;
    if (rt_env_flag_cached("SICHT_GC_TRACE", &rt_gc_trace_cached)) {
        fprintf(stderr, "[gc] roots=%d live=%zu next=%zu\n",
            rt_gc_root_count,
            gc_live_count(),
            gc_next_threshold_value());
    }
    int count = rt_gc_root_count;
    if (count <= 0) {
        gc_collect(NULL, 0);
        return;
    }
    rt_gc_ensure_root_values_capacity(count);
    for (int i = 0; i < count; i++) {
        Value* ptr = rt_gc_roots[i];
        rt_gc_root_values[i] = ptr ? *ptr : value_int(0);
    }
    gc_collect(rt_gc_root_values, count);
}

static int rt_should_flush_stdout(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    const char* env = getenv("SICHT_FLUSH_STDOUT");
    cached = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return cached;
}

static char* rt_strdup_checked(const char* s) {
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1)
        runtime_error("Out of memory", "Could not duplicate string.", "");
    len += 1;
    char* out = malloc(len);
    if (!out)
        runtime_error("Out of memory", "Could not duplicate string.", "");
    memcpy(out, s, len);
    return out;
}

void rt_make_int(Value* out, int v) {
    if (!out)
        return;
    *out = value_int(v);
}

void rt_make_float(Value* out, double v) {
    if (!out)
        return;
    *out = value_float(v);
}

void rt_make_bool(Value* out, int v) {
    if (!out)
        return;
    *out = value_bool(v);
}

void rt_make_string(Value* out, const char* s) {
    if (!out)
        return;
    *out = value_string(s ? s : "");
}

static Value rt_compare_values(Value a, Value b, int op) {
    if (a.type != b.type) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Cannot compare `%s` with `%s`.",
            rt_type_name(a.type),
            rt_type_name(b.type)
        );
        runtime_error("Invalid comparison", msg, "Cast one side so both operands have the same type.");
    }

    if (a.type == VALUE_INT) {
        int x = a.int_value;
        int y = b.int_value;
        switch (op) {
            case 0: return value_bool(x == y);
            case 1: return value_bool(x != y);
            case 2: return value_bool(x > y);
            case 3: return value_bool(x < y);
            case 4: return value_bool(x >= y);
            case 5: return value_bool(x <= y);
        }
    }

    if (a.type == VALUE_FLOAT) {
        double x = a.float_value;
        double y = b.float_value;
        switch (op) {
            case 0: return value_bool(x == y);
            case 1: return value_bool(x != y);
            case 2: return value_bool(x > y);
            case 3: return value_bool(x < y);
            case 4: return value_bool(x >= y);
            case 5: return value_bool(x <= y);
        }
    }

    if (a.type == VALUE_BOOL) {
        if (op != 0 && op != 1)
            runtime_error("Invalid boolean comparison", "Booleans only support equality comparisons.", "");
        return value_bool(op == 0 ? (a.int_value == b.int_value) : (a.int_value != b.int_value));
    }

    if (a.type == VALUE_STRING) {
        int cmp = strcmp(a.string_value, b.string_value);
        if (op != 0 && op != 1)
            runtime_error("Invalid string comparison", "Strings only support equality comparisons.", "");
        return value_bool(op == 0 ? (cmp == 0) : (cmp != 0));
    }

    if (a.type == VALUE_BUFFER) {
        if (op != 0 && op != 1)
            runtime_error("Invalid buffer comparison", "Buffers only support equality comparisons.", "");
        return value_bool(op == 0 ? (a.buffer_value == b.buffer_value) : (a.buffer_value != b.buffer_value));
    }

    if (a.type == VALUE_ADDRESS) {
        if (op != 0 && op != 1)
            runtime_error("Invalid address comparison", "Addresses only support equality comparisons.", "");
        return value_bool(op == 0 ? (a.address_value == b.address_value) : (a.address_value != b.address_value));
    }

    runtime_error("Invalid comparison", "Comparison is not supported for this type.", "");
    return value_bool(0);
}

static int rt_int_add_checked(int a, int b, int* out) {
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b))
        return 0;
    *out = a + b;
    return 1;
}

static int rt_int_sub_checked(int a, int b, int* out) {
    if ((b > 0 && a < INT_MIN + b) || (b < 0 && a > INT_MAX + b))
        return 0;
    *out = a - b;
    return 1;
}

static int rt_int_mul_checked(int a, int b, int* out) {
    long long v = (long long)a * (long long)b;
    if (v < INT_MIN || v > INT_MAX)
        return 0;
    *out = (int)v;
    return 1;
}

static Value rt_add_value(Value a, Value b) {
    if (a.type == VALUE_STRING || b.type == VALUE_STRING) {
        Value left_str = rt_cast_string_value(a);
        Value right_str = rt_cast_string_value(b);
        size_t left_len = strlen(left_str.string_value);
        size_t right_len = strlen(right_str.string_value);
        if (left_len > SIZE_MAX - right_len - 1)
            runtime_error("Out of memory", "String concatenation is too large.", "");
        size_t len = left_len + right_len + 1;
        char* joined = malloc(len);
        if (!joined)
            runtime_error("Out of memory", "Could not allocate concatenation buffer.", "");
        strcpy(joined, left_str.string_value);
        strcat(joined, right_str.string_value);
        Value out = value_string(joined);
        free(joined);
        return out;
    }

    if (a.type != b.type || (a.type != VALUE_INT && a.type != VALUE_FLOAT)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Arithmetic requires matching numeric types, got `%s` and `%s`.",
            rt_type_name(a.type),
            rt_type_name(b.type)
        );
        runtime_error("Invalid arithmetic", msg, "Use int with int or float with float (or cast explicitly).");
    }

    if (a.type == VALUE_FLOAT) {
        return value_float(a.float_value + b.float_value);
    }

    {
        int out = 0;
        if (!rt_int_add_checked(a.int_value, b.int_value, &out))
            runtime_error("Integer overflow", "Addition overflow.", "");
        return value_int(out);
    }
}

static Value rt_sub_value(Value a, Value b) {
    if (a.type != b.type || (a.type != VALUE_INT && a.type != VALUE_FLOAT)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Arithmetic requires matching numeric types, got `%s` and `%s`.",
            rt_type_name(a.type),
            rt_type_name(b.type)
        );
        runtime_error("Invalid arithmetic", msg, "Use int with int or float with float (or cast explicitly).");
    }

    if (a.type == VALUE_FLOAT)
        return value_float(a.float_value - b.float_value);

    {
        int out = 0;
        if (!rt_int_sub_checked(a.int_value, b.int_value, &out))
            runtime_error("Integer overflow", "Subtraction overflow.", "");
        return value_int(out);
    }
}

static Value rt_mul_value(Value a, Value b) {
    if (a.type != b.type || (a.type != VALUE_INT && a.type != VALUE_FLOAT)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Arithmetic requires matching numeric types, got `%s` and `%s`.",
            rt_type_name(a.type),
            rt_type_name(b.type)
        );
        runtime_error("Invalid arithmetic", msg, "Use int with int or float with float (or cast explicitly).");
    }

    if (a.type == VALUE_FLOAT)
        return value_float(a.float_value * b.float_value);

    {
        int out = 0;
        if (!rt_int_mul_checked(a.int_value, b.int_value, &out))
            runtime_error("Integer overflow", "Multiplication overflow.", "");
        return value_int(out);
    }
}

static Value rt_div_value(Value a, Value b) {
    if (a.type != b.type || (a.type != VALUE_INT && a.type != VALUE_FLOAT)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Arithmetic requires matching numeric types, got `%s` and `%s`.",
            rt_type_name(a.type),
            rt_type_name(b.type)
        );
        runtime_error("Invalid arithmetic", msg, "Use int with int or float with float (or cast explicitly).");
    }

    if (a.type == VALUE_FLOAT) {
        if (b.float_value == 0.0)
            runtime_error("Division by zero", "", "");
        return value_float(a.float_value / b.float_value);
    }

    if (b.int_value == 0)
        runtime_error("Division by zero", "", "");
    if (a.int_value == INT_MIN && b.int_value == -1)
        runtime_error("Integer overflow", "Division overflow for minimum integer by -1.", "");
    return value_int(a.int_value / b.int_value);
}

static Value rt_eq_value(Value a, Value b) { return rt_compare_values(a, b, 0); }
static Value rt_ne_value(Value a, Value b) { return rt_compare_values(a, b, 1); }
static Value rt_gt_value(Value a, Value b) { return rt_compare_values(a, b, 2); }
static Value rt_lt_value(Value a, Value b) { return rt_compare_values(a, b, 3); }
static Value rt_gte_value(Value a, Value b) { return rt_compare_values(a, b, 4); }
static Value rt_lte_value(Value a, Value b) { return rt_compare_values(a, b, 5); }

static Value rt_and_value(Value a, Value b) {
    return value_bool(rt_truthy_value(a) && rt_truthy_value(b));
}

static Value rt_or_value(Value a, Value b) {
    return value_bool(rt_truthy_value(a) || rt_truthy_value(b));
}

static Value rt_not_value(Value a) {
    return value_bool(!rt_truthy_value(a));
}

static Value rt_neg_value(Value a) {
    if (a.type == VALUE_INT) {
        if (a.int_value == INT_MIN)
            runtime_error("Integer overflow", "Negation overflow on minimum integer value.", "");
        return value_int(-a.int_value);
    }
    if (a.type == VALUE_FLOAT)
        return value_float(-a.float_value);
    runtime_error("Invalid arithmetic", "Unary negation requires a numeric value.", "");
    return value_int(0);
}

static Value rt_cast_int_value(Value v) {
    if (v.type == VALUE_INT)
        return v;
    if (v.type == VALUE_FLOAT) {
        if (!isfinite(v.float_value) || v.float_value < (double)INT_MIN || v.float_value > (double)INT_MAX) {
            runtime_error("Invalid cast", "Float value is outside integer range.", "");
        }
        return value_int((int)v.float_value);
    }
    if (v.type == VALUE_STRING) {
        char* endptr;
        errno = 0;
        long result = strtol(v.string_value, &endptr, 10);
        if (errno == ERANGE || result < INT_MIN || result > INT_MAX)
            runtime_error("Invalid cast", "String integer is outside supported range.", "");
        if (*endptr != '\0')
            runtime_error("Invalid cast", "String is not a valid integer.", "");
        return value_int((int)result);
    }
    runtime_error("Invalid cast", "Cannot cast this value to integer.", "");
    return value_int(0);
}

static Value rt_cast_float_value(Value v) {
    if (v.type == VALUE_FLOAT)
        return v;
    if (v.type == VALUE_INT)
        return value_float((double)v.int_value);
    if (v.type == VALUE_STRING) {
        char* endptr;
        errno = 0;
        double result = strtod(v.string_value, &endptr);
        if (errno == ERANGE || !isfinite(result))
            runtime_error("Invalid cast", "String is outside supported float range.", "");
        if (*endptr != '\0')
            runtime_error("Invalid cast", "String is not a valid float.", "");
        return value_float(result);
    }
    runtime_error("Invalid cast", "Cannot cast this value to float.", "");
    return value_float(0.0);
}

static Value rt_cast_bool_value(Value v) {
    if (v.type == VALUE_BOOL)
        return v;
    if (v.type == VALUE_INT)
        return value_bool(v.int_value != 0);
    if (v.type == VALUE_FLOAT)
        return value_bool(v.float_value != 0.0);
    runtime_error("Invalid cast", "Only numeric values can be cast to boolean.", "");
    return value_bool(0);
}

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} RtStrBuf;

static void rt_sb_init(RtStrBuf* sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char*)malloc(sb->cap);
    if (!sb->buf)
        runtime_error("Out of memory", "Could not allocate string buffer.", "");
    sb->buf[0] = '\0';
}

static void rt_sb_ensure(RtStrBuf* sb, size_t needed) {
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

static void rt_sb_append(RtStrBuf* sb, const char* text) {
    if (!text)
        return;
    size_t text_len = strlen(text);
    if (text_len == 0)
        return;
    rt_sb_ensure(sb, sb->len + text_len + 1);
    memcpy(sb->buf + sb->len, text, text_len);
    sb->len += text_len;
    sb->buf[sb->len] = '\0';
}

static void rt_sb_append_char(RtStrBuf* sb, char ch) {
    rt_sb_ensure(sb, sb->len + 2);
    sb->buf[sb->len++] = ch;
    sb->buf[sb->len] = '\0';
}

static void rt_sb_append_value(RtStrBuf* sb, Value v, int depth) {
    const int max_depth = 16;
    if (depth > max_depth) {
        rt_sb_append(sb, "<...>");
        return;
    }

    switch (v.type) {
        case VALUE_STRING:
            rt_sb_append(sb, v.string_value ? v.string_value : "");
            return;
        case VALUE_BOOL:
            rt_sb_append(sb, v.int_value ? "true" : "false");
            return;
        case VALUE_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", v.float_value);
            rt_sb_append(sb, buf);
            return;
        }
        case VALUE_INT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d", v.int_value);
            rt_sb_append(sb, buf);
            return;
        }
        case VALUE_BUFFER: {
            char buf[64];
            size_t size = v.buffer_value ? v.buffer_value->size : 0;
            snprintf(buf, sizeof(buf), "<buffer %zu bytes>", size);
            rt_sb_append(sb, buf);
            return;
        }
        case VALUE_ADDRESS: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<address %p>", v.address_value);
            rt_sb_append(sb, buf);
            return;
        }
        case VALUE_LIST: {
            List* list = v.list_value;
            rt_sb_append_char(sb, '[');
            if (list) {
                for (int i = 0; i < list->count; i++) {
                    rt_sb_append_value(sb, list->items[i], depth + 1);
                    if (i < list->count - 1)
                        rt_sb_append(sb, ", ");
                }
            }
            rt_sb_append_char(sb, ']');
            return;
        }
        case VALUE_DICT: {
            Dict* dict = v.dict_value;
            rt_sb_append_char(sb, '{');
            if (dict) {
                for (int i = 0; i < dict->count; i++) {
                    DictEntry* e = &dict->entries[i];
                    rt_sb_append_char(sb, '"');
                    rt_sb_append(sb, e->key ? e->key : "");
                    rt_sb_append(sb, "\": ");
                    rt_sb_append_value(sb, e->value, depth + 1);
                    if (i < dict->count - 1)
                        rt_sb_append(sb, ", ");
                }
            }
            rt_sb_append_char(sb, '}');
            return;
        }
        case VALUE_GENERATOR: {
            Generator* g = v.generator_value;
            if (!g) {
                rt_sb_append(sb, "<generator:null>");
                return;
            }
            char buf[128];
            if (g->initialized && g->cache)
                snprintf(buf, sizeof(buf), "<generator index=%d remaining=%d>", g->index, g->cache->count - g->index);
            else
                snprintf(buf, sizeof(buf), "<generator index=%d>", g->index);
            rt_sb_append(sb, buf);
            return;
        }
    }
}

static Value rt_stringify_value(Value v) {
    RtStrBuf sb;
    rt_sb_init(&sb);
    rt_sb_append_value(&sb, v, 0);
    Value out = value_string(sb.buf);
    free(sb.buf);
    return out;
}

static Value rt_cast_string_value(Value v) {
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
    return rt_stringify_value(v);
}

static int rt_get_int_value(Value v) {
    if (v.type != VALUE_INT)
        runtime_error("Invalid loop bound", "Loop bound must be an integer.", "Cast to int before using repeat.");
    return v.int_value;
}

static int rt_truthy_value(Value v) {
    if (v.type != VALUE_BOOL)
        runtime_error("Invalid condition", "Condition must be boolean.", "Cast to bool before using it in conditions.");
    return v.int_value != 0;
}

static void rt_print_inline_value_depth(Value v, int depth);

static void rt_print_inline_value(Value v) {
    rt_print_inline_value_depth(v, 0);
}

static void rt_print_inline_value_depth(Value v, int depth) {
    const int max_depth = 16;
    if (depth > max_depth) {
        printf("<...>");
        return;
    }

    switch (v.type) {
        case VALUE_STRING:
            if (v.string_value) {
                fwrite(v.string_value, 1, strlen(v.string_value), stdout);
            }
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
        case VALUE_BUFFER:
            printf("<buffer %zu bytes>", v.buffer_value ? v.buffer_value->size : 0);
            return;
        case VALUE_ADDRESS:
            printf("<address %p>", v.address_value);
            return;
        case VALUE_LIST: {
            List* list = v.list_value;
            if (!list) {
                printf("[]");
                return;
            }
            printf("[");
            for (int i = 0; i < list->count; i++) {
                rt_print_inline_value_depth(list->items[i], depth + 1);
                if (i < list->count - 1)
                    printf(", ");
            }
            printf("]");
            return;
        }
        case VALUE_DICT: {
            Dict* dict = v.dict_value;
            if (!dict) {
                printf("{}");
                return;
            }
            printf("{");
            for (int i = 0; i < dict->count; i++) {
                DictEntry* e = &dict->entries[i];
                printf("\"%s\": ", e->key ? e->key : "");
                rt_print_inline_value_depth(e->value, depth + 1);
                if (i < dict->count - 1)
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

static int rt_compare_int_values(const void* a, const void* b) {
    const Value* va = (const Value*)a;
    const Value* vb = (const Value*)b;
    if (va->int_value < vb->int_value)
        return -1;
    if (va->int_value > vb->int_value)
        return 1;
    return 0;
}

static int rt_size_to_int_or_error(size_t n, const char* title, const char* message) {
    if (n > (size_t)INT_MAX)
        runtime_error(title, message, "");
    return (int)n;
}

typedef void (*RtGenFn)(Value* out_list, Value* args, int arg_count);

static void rt_materialize_generator(Generator* g) {
    if (!g || g->initialized)
        return;
    if (!g->function_node)
        runtime_error("Invalid generator", "Generator has no function body.", "");

    RtGenFn fn = (RtGenFn)g->function_node;
    Value list_val;
    list_val.type = VALUE_LIST;
    list_val.int_value = 0;
    list_val.float_value = 0.0;
    list_val.string_value = NULL;
    list_val.dict_value = NULL;
    list_val.generator_value = NULL;
    list_val.list_value = gc_list_new(0);
    list_val.list_value->count = 0;

    fn(&list_val, g->args, g->arg_count);
    g->cache = list_val.list_value;
    g->initialized = 1;
    g->index = 0;
}

static int rt_value_equals_internal(Value a, Value b, int depth) {
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
                if (!rt_value_equals_internal(a.list_value->items[i], b.list_value->items[i], depth + 1))
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
                int idx = gc_dict_find_index(b.dict_value, key);
                if (idx < 0)
                    return 0;
                if (!rt_value_equals_internal(a.dict_value->entries[i].value, b.dict_value->entries[idx].value, depth + 1))
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

static int rt_value_equals(Value a, Value b) {
    return rt_value_equals_internal(a, b, 0);
}

void rt_add(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_add_value(*a, *b);
}

void rt_sub(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_sub_value(*a, *b);
}

void rt_mul(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_mul_value(*a, *b);
}

void rt_div(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_div_value(*a, *b);
}

void rt_eq(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_eq_value(*a, *b);
}

void rt_ne(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_ne_value(*a, *b);
}

void rt_gt(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_gt_value(*a, *b);
}

void rt_lt(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_lt_value(*a, *b);
}

void rt_gte(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_gte_value(*a, *b);
}

void rt_lte(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_lte_value(*a, *b);
}

void rt_and(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_and_value(*a, *b);
}

void rt_or(Value* out, const Value* a, const Value* b) {
    if (!out || !a || !b)
        rt_null_value_error();
    *out = rt_or_value(*a, *b);
}

void rt_not(Value* out, const Value* a) {
    if (!out || !a)
        rt_null_value_error();
    *out = rt_not_value(*a);
}

void rt_neg(Value* out, const Value* a) {
    if (!out || !a)
        rt_null_value_error();
    *out = rt_neg_value(*a);
}

void rt_cast_int(Value* out, const Value* v) {
    if (!out || !v)
        rt_null_value_error();
    *out = rt_cast_int_value(*v);
}

void rt_cast_float(Value* out, const Value* v) {
    if (!out || !v)
        rt_null_value_error();
    *out = rt_cast_float_value(*v);
}

void rt_cast_bool(Value* out, const Value* v) {
    if (!out || !v)
        rt_null_value_error();
    *out = rt_cast_bool_value(*v);
}

void rt_cast_string(Value* out, const Value* v) {
    if (!out || !v)
        rt_null_value_error();
    *out = rt_cast_string_value(*v);
}

int rt_get_int(const Value* v) {
    if (!v)
        rt_null_value_error();
    return rt_get_int_value(*v);
}

int rt_truthy(const Value* v) {
    if (!v)
        rt_null_value_error();
    return rt_truthy_value(*v);
}

void rt_print_inline(const Value* v) {
    if (!v)
        rt_null_value_error();
    rt_print_inline_value(*v);
}

void rt_print_int(int v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    if (n > 0)
        fwrite(buf, 1, (size_t)n, stdout);
}

void rt_print_float(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n > 0)
        fwrite(buf, 1, (size_t)n, stdout);
}

void rt_print_text(const char* s) {
    if (!s)
        return;
    fwrite(s, 1, strlen(s), stdout);
}

void rt_print_text_repeat(const char* s, int count) {
    if (!s || count <= 0)
        return;
    size_t len = strlen(s);
    if (len == 0)
        return;

    if (len == 1) {
        char buf[8192];
        memset(buf, s[0], sizeof(buf));
        while (count > 0) {
            int chunk = count < (int)sizeof(buf) ? count : (int)sizeof(buf);
            fwrite(buf, 1, (size_t)chunk, stdout);
            count -= chunk;
        }
        return;
    }

    size_t reps_per_chunk = 8192 / len;
    if (reps_per_chunk == 0)
        reps_per_chunk = 1;
    size_t chunk_len = reps_per_chunk * len;

    char stack_buf[4096];
    char* buf = stack_buf;
    if (chunk_len > sizeof(stack_buf)) {
        buf = (char*)malloc(chunk_len);
        if (!buf) {
            for (int i = 0; i < count; i++)
                fwrite(s, 1, len, stdout);
            return;
        }
    }

    for (size_t i = 0; i < reps_per_chunk; i++)
        memcpy(buf + (i * len), s, len);

    while (count >= (int)reps_per_chunk) {
        fwrite(buf, 1, chunk_len, stdout);
        count -= (int)reps_per_chunk;
    }

    for (int i = 0; i < count; i++)
        fwrite(s, 1, len, stdout);

    if (buf != stack_buf)
        free(buf);
}

void rt_print_text_repeat_checked(const char* s, int count) {
    if (count < 0)
        runtime_error("Invalid repeat count", "Repeat count cannot be negative.", "");
    if (!s || count == 0)
        return;

    long long ticks = (long long)count;
    if (ticks > 0) {
        if (ticks > LLONG_MAX / 2) {
            runtime_error(
                "Execution step limit exceeded",
                "Program exceeded configured step budget.",
                "Raise SICHT_MAX_STEPS or simplify the loop/control flow."
            );
        }
        ticks *= 2;
    }
    rt_step_tick_bulk(ticks);

    int flush = rt_should_flush_stdout();
    if (flush) {
        size_t len = strlen(s);
        if (len == 0)
            return;
        for (int i = 0; i < count; i++) {
            fwrite(s, 1, len, stdout);
            fflush(stdout);
        }
        return;
    }

    rt_print_text_repeat(s, count);
}

void rt_print_newline(void) {
    fputc('\n', stdout);
    if (rt_should_flush_stdout())
        fflush(stdout);
}

void rt_list_new(Value* out, int count) {
    if (!out)
        rt_null_value_error();
    List* list = gc_list_new(count);
    list->count = count;
    for (int i = 0; i < count; i++)
        list->items[i] = value_int(0);
    rt_trace("list_new", list, list->count, list->capacity);
    *out = value_list(list);
}

void rt_list_set(Value* list_val, const Value* index_val, const Value* value) {
    if (!list_val || !index_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list assignment", "Target variable is not a list.", "");
    if (index_val->type != VALUE_INT)
        runtime_error("Invalid index", "List index must be an integer.", "");
    int idx = index_val->int_value;
    List* list = list_val->list_value;
    if (idx < 0 || idx >= list->count)
        runtime_error("Index out of bounds", "List index outside range.", "");
    list->items[idx] = *value;
}

void rt_list_set_i32(Value* list_val, int index, const Value* value) {
    if (!list_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list assignment", "Target variable is not a list.", "");
    List* list = list_val->list_value;
    if (index < 0 || index >= list->count)
        runtime_error("Index out of bounds", "List index outside range.", "");
    list->items[index] = *value;
}

void rt_list_get(Value* out, const Value* list_val, const Value* index_val) {
    if (!out || !list_val || !index_val)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list access", "Target must be a list.", "");
    if (index_val->type != VALUE_INT)
        runtime_error("Invalid list index", "Index must be an integer.", "");
    int idx = index_val->int_value;
    List* list = list_val->list_value;
    if (idx < 0 || idx >= list->count)
        runtime_error("Index out of bounds", "List index is outside the list.", "");
    *out = list->items[idx];
}

void rt_list_get_i32(Value* out, const Value* list_val, int index) {
    if (!out || !list_val)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list access", "Target must be a list.", "");
    List* list = list_val->list_value;
    if (index < 0 || index >= list->count)
        runtime_error("Index out of bounds", "List index is outside the list.", "");
    *out = list->items[index];
}

void rt_list_add(Value* list_val, const Value* value) {
    if (!list_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list operation", "Target variable is not a list.", "");
    List* list = list_val->list_value;
    gc_list_reserve(list, list->count + 1);
    list->items[list->count++] = *value;
    rt_trace("list_add", list, list->count, list->capacity);
}

void rt_gen_yield(Value* list_val, const Value* value) {
    if (!list_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid yield", "Generator output target is not a list.", "");
    if (rt_generator_limit < 0)
        rt_generator_limit = rt_read_generator_limit();
    if (rt_generator_limit > 0) {
        List* list = list_val->list_value;
        if (list && (long long)list->count >= rt_generator_limit) {
            runtime_error(
                "Generator yield limit exceeded",
                "This generator produced more values than the configured limit.",
                "Raise SICHT_MAX_GENERATOR_ITEMS or make the generator terminate sooner."
            );
        }
    }
    rt_list_add(list_val, value);
}

void rt_list_remove(Value* list_val, const Value* value) {
    if (!list_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list operation", "Target variable is not a list.", "");
    List* list = list_val->list_value;
    int found = -1;
    for (int i = 0; i < list->count; i++) {
        if (rt_value_equals(list->items[i], *value)) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return;
    for (int i = found; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];
    list->count--;
}

void rt_list_remove_element(Value* list_val, const Value* index_val) {
    if (!list_val || !index_val)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list operation", "Target variable is not a list.", "");
    if (index_val->type != VALUE_INT)
        runtime_error("Invalid index", "List index must be an integer.", "");
    int idx = index_val->int_value;
    List* list = list_val->list_value;
    if (idx < 0 || idx >= list->count)
        runtime_error("Index out of bounds", "List index outside range.", "");
    rt_trace("list_remove_element", list, idx, list->count);
    for (int i = idx; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];
    list->count--;
}

void rt_list_remove_element_i32(Value* list_val, int index) {
    if (!list_val)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list operation", "Target variable is not a list.", "");
    List* list = list_val->list_value;
    if (index < 0 || index >= list->count)
        runtime_error("Index out of bounds", "List index outside range.", "");
    rt_trace("list_remove_element_i32", list, index, list->count);
    for (int i = index; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];
    list->count--;
}

void rt_list_clear(Value* list_val) {
    if (!list_val)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid list operation", "Target variable is not a list.", "");
    list_val->list_value->count = 0;
}

void rt_list_index_of(Value* out, const Value* list_val, const Value* value) {
    if (!out || !list_val || !value)
        rt_null_value_error();
    if (list_val->type != VALUE_LIST)
        runtime_error("Invalid index search", "Target must be a list.", "");
    List* list = list_val->list_value;
    for (int i = 0; i < list->count; i++) {
        if (rt_value_equals(list->items[i], *value)) {
            *out = value_int(i);
            return;
        }
    }
    *out = value_int(-1);
}

void rt_dict_new(Value* out, int count) {
    if (!out)
        rt_null_value_error();
    Dict* dict = gc_dict_new(count);
    rt_trace("dict_new", dict, dict->count, dict->capacity);
    *out = value_dict(dict);
}

void rt_dict_set(Value* dict_val, const Value* key, const Value* value) {
    if (!dict_val || !key || !value)
        rt_null_value_error();
    if (dict_val->type != VALUE_DICT)
        runtime_error("Invalid dictionary operation", "Target variable is not a dictionary.", "");
    if (key->type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            rt_type_name(key->type)
        );
        runtime_error("Invalid dictionary key", msg, "Use quoted keys like: \"name\"");
    }
    gc_dict_set(dict_val->dict_value, key->string_value, *value);
    rt_trace("dict_set", dict_val->dict_value, dict_val->dict_value->count, dict_val->dict_value->capacity);
}

void rt_dict_get(Value* out, const Value* dict_val, const Value* key) {
    if (!out || !dict_val || !key)
        rt_null_value_error();
    if (dict_val->type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary access expected a dictionary, but got `%s`.",
            rt_type_name(dict_val->type)
        );
        runtime_error("Invalid dictionary access", msg, "Use: get item \"key\" from(<dictionary>)");
    }
    if (key->type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary key must be a string, but got `%s`.",
            rt_type_name(key->type)
        );
        runtime_error("Invalid dictionary key", msg, "Use a quoted key, for example: \"name\"");
    }
    Dict* dict = dict_val->dict_value;
    int idx = gc_dict_find_index(dict, key->string_value);
    if (idx >= 0) {
        *out = dict->entries[idx].value;
        return;
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Key `%s` does not exist in dictionary.",
            key->string_value
        );
        runtime_error("Missing dictionary item", msg, "Check the key spelling or add the key before reading it.");
    }
    *out = value_int(0);
}

void rt_dict_remove(Value* dict_val, const Value* key) {
    if (!dict_val || !key)
        rt_null_value_error();
    if (dict_val->type != VALUE_DICT)
        runtime_error("Invalid dictionary operation", "Target variable is not a dictionary.", "");
    if (key->type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            rt_type_name(key->type)
        );
        runtime_error("Invalid dictionary key", msg, "Use quoted keys like: \"name\"");
    }
    if (!gc_dict_remove(dict_val->dict_value, key->string_value)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Key `%s` does not exist in dictionary.", key->string_value);
        runtime_error("Missing dictionary item", msg, "Check the key spelling before removing.");
    }
}

void rt_dict_clear(Value* dict_val) {
    if (!dict_val)
        rt_null_value_error();
    if (dict_val->type != VALUE_DICT)
        runtime_error("Invalid dictionary operation", "Target variable is not a dictionary.", "");
    gc_dict_clear(dict_val->dict_value);
}

void rt_dict_contains_item(Value* dict_val, const Value* key) {
    if (!dict_val || !key)
        rt_null_value_error();
    if (dict_val->type != VALUE_DICT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Variable is `%s`, not a dictionary.",
            rt_type_name(dict_val->type)
        );
        runtime_error("Invalid dictionary operation", msg, "Use dictionary membership checks only with dictionaries.");
    }
    if (key->type != VALUE_STRING) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Dictionary keys must be strings, but got `%s`.",
            rt_type_name(key->type)
        );
        runtime_error("Invalid dictionary key", msg, "Use quoted keys like: \"name\" in(dict)");
    }
    (void)gc_dict_find_index(dict_val->dict_value, key->string_value);
}

void rt_contains(Value* out, const Value* left, const Value* right) {
    if (!out || !left || !right)
        rt_null_value_error();
    if (left->type == VALUE_LIST) {
        List* list = left->list_value;
        for (int i = 0; i < list->count; i++) {
            if (rt_value_equals(list->items[i], *right)) {
                *out = value_bool(1);
                return;
            }
        }
        *out = value_bool(0);
        return;
    }
    if (left->type == VALUE_DICT) {
        if (right->type != VALUE_STRING)
            runtime_error("Invalid dictionary contains", "Dictionary keys must be strings.", "");
        *out = value_bool(gc_dict_find_index(left->dict_value, right->string_value) >= 0);
        return;
    }
    if (left->type == VALUE_STRING) {
        if (right->type != VALUE_STRING)
            runtime_error("Invalid string contains", "String membership needs a string value.", "");
        *out = value_bool(strstr(left->string_value, right->string_value) != NULL);
        return;
    }
    runtime_error("Invalid contains usage", "Left side must be a list, dictionary, or string.", "");
}

void rt_input(Value* out, const char* prompt) {
    if (!out)
        rt_null_value_error();
    if (prompt && prompt[0]) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    char buffer[1024];
    if (!fgets(buffer, sizeof(buffer), stdin)) {
        runtime_error("Input error", "Failed to read input.", "");
    }
    buffer[strcspn(buffer, "\n")] = '\0';
    *out = value_string(buffer);
}

void rt_builtin(Value* out, int builtin_type, const Value* arg) {
    if (!out || !arg)
        rt_null_value_error();

    Value v = *arg;

    if (builtin_type == BUILTIN_NEXT) {
        if (v.type != VALUE_GENERATOR)
            runtime_error(
                "Invalid next usage",
                "next from(...) requires a generator.",
                "Use next from(generator_call(...))."
            );
        if (!rt_generator_next(out, &v))
            runtime_error(
                "Generator exhausted",
                "No more values are available from this generator.",
                "Check remaining values before pulling again."
            );
        return;
    }

    if (builtin_type == BUILTIN_REVERSE) {
        if (v.type != VALUE_LIST)
            runtime_error("Invalid reverse usage", "reverse requires a list.", "");
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
        *out = v;
        return;
    }

    if (builtin_type == BUILTIN_SORT) {
        if (v.type != VALUE_LIST)
            runtime_error("Invalid sort usage", "sort requires a list.", "");
        List* list = v.list_value;
        int all_ints = 1;
        for (int i = 0; i < list->count; i++) {
            if (list->items[i].type != VALUE_INT) {
                all_ints = 0;
                break;
            }
        }
        if (all_ints && list->count > 1) {
            qsort(list->items, (size_t)list->count, sizeof(Value), rt_compare_int_values);
            *out = v;
            return;
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
        *out = v;
        return;
    }

    if (builtin_type == BUILTIN_LENGTH) {
        if (v.type == VALUE_STRING) {
            *out = value_int(rt_size_to_int_or_error(
                strlen(v.string_value),
                "String too large",
                "String length exceeds supported integer range."
            ));
            return;
        }
        if (v.type == VALUE_LIST) {
            *out = value_int(v.list_value->count);
            return;
        }
        if (v.type == VALUE_GENERATOR) {
            rt_materialize_generator(v.generator_value);
            if (!v.generator_value->cache) {
                *out = value_int(0);
                return;
            }
            *out = value_int(v.generator_value->cache->count - v.generator_value->index);
            return;
        }
        runtime_error("Invalid length usage", "length of requires a string, list, or generator.", "");
    }

    if (v.type != VALUE_STRING)
        runtime_error("Invalid builtin usage", "This operation requires a string.", "");

    const char* s = v.string_value ? v.string_value : "";
    if (builtin_type == BUILTIN_LENGTH) {
        *out = value_int(rt_size_to_int_or_error(
            strlen(s),
            "String too large",
            "String length exceeds supported integer range."
        ));
        return;
    }

    size_t s_len = strlen(s);
    if (s_len > SIZE_MAX - 1)
        runtime_error("Out of memory", "String is too large to process.", "");
    char* buf = malloc(s_len + 1);
    if (!buf)
        runtime_error("Out of memory", "Could not allocate string buffer.", "");
    strcpy(buf, s);

    if (builtin_type == BUILTIN_UPPERCASE) {
        for (int i = 0; buf[i]; i++)
            buf[i] = (char)toupper((unsigned char)buf[i]);
    } else if (builtin_type == BUILTIN_LOWERCASE) {
        for (int i = 0; buf[i]; i++)
            buf[i] = (char)tolower((unsigned char)buf[i]);
    } else if (builtin_type == BUILTIN_TRIM) {
        char* start = buf;
        while (*start && isspace((unsigned char)*start))
            start++;
        if (*start == '\0') {
            Value result = value_string("");
            free(buf);
            *out = result;
            return;
        }
        char* end = start + strlen(start) - 1;
        while (end > start && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }
        Value result = value_string(start);
        free(buf);
        *out = result;
        return;
    }

    Value result = value_string(buf);
    free(buf);
    *out = result;
}

void rt_char_at(Value* out, const Value* str_val, const Value* index_val) {
    if (!out || !str_val || !index_val)
        rt_null_value_error();
    if (str_val->type != VALUE_STRING)
        runtime_error("Invalid character access", "The target must be a string.", "");
    if (index_val->type != VALUE_INT)
        runtime_error("Invalid character index", "Index must be an integer.", "");
    if (index_val->int_value < 0)
        runtime_error("Index out of bounds", "Character index cannot be negative.", "");

    const char* str = str_val->string_value;
    int index = index_val->int_value;

    int current = 0;
    const char* p = str;
    while (*p) {
        if (current == index)
            break;
        unsigned char c = (unsigned char)*p;
        if ((c & 0x80) == 0) p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else if ((c & 0xF8) == 0xF0) p += 4;
        else runtime_error("Invalid UTF-8", "Malformed UTF-8 string.", "");
        current++;
    }

    if (!*p)
        runtime_error("Index out of bounds", "Character index is outside the string.", "");

    int len = 1;
    unsigned char c = (unsigned char)*p;
    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;

    char buf[5];
    memcpy(buf, p, (size_t)len);
    buf[len] = '\0';
    *out = value_string(buf);
}

void rt_file_read(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid file path", "read file expects a string path.", "");

    FILE* f = fopen(path_val->string_value, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not open `%s` for reading.", path_val->string_value);
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
    Value outv = value_string(buffer);
    free(buffer);
    *out = outv;
}

void rt_file_write(const Value* path_val, const Value* content_val, int append_mode) {
    if (!path_val || !content_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid file path", "File path must be a string.", "");
    if (content_val->type != VALUE_STRING)
        runtime_error("Invalid file content", "File content must be a string.", "");

    FILE* f = fopen(path_val->string_value, append_mode ? "ab" : "wb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not open `%s` for writing.", path_val->string_value);
        runtime_error("File write failed", msg, "");
    }
    size_t len = strlen(content_val->string_value);
    size_t written = fwrite(content_val->string_value, 1, len, f);
    fclose(f);
    if (written != len)
        runtime_error("File write failed", "Could not write full file content.", "");
}

void rt_cli_set_args(int argc, const char** argv) {
    if (rt_cli_args) {
        for (int i = 0; i < rt_cli_argc; i++)
            free(rt_cli_args[i]);
        free(rt_cli_args);
    }
    rt_cli_args = NULL;
    rt_cli_argc = 0;

    if (!argv || argc <= 0)
        return;
    if (argc > INT_MAX)
        runtime_error("Arguments too large", "Argument count exceeds supported size.", "");

    rt_cli_args = (char**)calloc((size_t)argc, sizeof(char*));
    if (!rt_cli_args)
        runtime_error("Out of memory", "Could not allocate argument list.", "");
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i] ? argv[i] : "";
        rt_cli_args[i] = rt_strdup_checked(arg);
    }
    rt_cli_argc = argc;
}

void rt_cli_set_args_from_argv(int argc, const char** argv) {
    if (!argv || argc <= 1) {
        rt_cli_set_args(0, NULL);
        return;
    }
    rt_cli_set_args(argc - 1, argv + 1);
}

void rt_native_cli_args(Value* out) {
    if (!out)
        rt_null_value_error();
    List* list = gc_list_new(rt_cli_argc);
    for (int i = 0; i < rt_cli_argc; i++) {
        Value v = value_string(rt_cli_args[i] ? rt_cli_args[i] : "");
        gc_list_reserve(list, list->count + 1);
        list->items[list->count++] = v;
    }
    *out = value_list(list);
}

static int rt_path_is_root(const char* path) {
    if (!path || !path[0])
        return 1;
    if (strcmp(path, "/") == 0 || strcmp(path, "\\") == 0)
        return 1;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        if (path[2] == '\0')
            return 1;
        if ((path[2] == '/' || path[2] == '\\') && path[3] == '\0')
            return 1;
    }
    return 0;
}

#ifdef _WIN32
static char* rt_win_path_copy(const char* path) {
    if (!path)
        path = "";
    size_t len = strlen(path);
    if (len > SIZE_MAX - 1)
        runtime_error("Path too long", "Windows path exceeds supported size.", "");
    char* out = (char*)malloc(len + 1);
    if (!out)
        runtime_error("Out of memory", "Could not allocate path buffer.", "");
    for (size_t i = 0; i <= len; i++) {
        char c = path[i];
        out[i] = (c == '/') ? '\\' : c;
    }
    return out;
}
#endif

static int rt_join_path(char* out, size_t out_size, const char* left, const char* right) {
    if (!out || out_size == 0 || !right)
        return 0;
    if (!left || !left[0]) {
        int wrote = snprintf(out, out_size, "%s", right);
        return wrote > 0 && (size_t)wrote < out_size;
    }
#ifdef _WIN32
    char sep = '\\';
#else
    char sep = '/';
#endif
    size_t left_len = strlen(left);
    int need_sep = left_len > 0 && left[left_len - 1] != '/' && left[left_len - 1] != '\\';
    int wrote = snprintf(out, out_size, "%s%s%s", left, need_sep ? (char[]){sep, '\0'} : "", right);
    return wrote > 0 && (size_t)wrote < out_size;
}

static int rt_make_dir_single(const char* path, char* err, size_t err_size) {
    if (!path || !path[0]) {
        snprintf(err, err_size, "Empty directory path.");
        return 0;
    }
#ifdef _WIN32
    char* win_path = rt_win_path_copy(path);
    BOOL ok = CreateDirectoryA(win_path, NULL);
    if (!ok) {
        DWORD code = GetLastError();
        free(win_path);
        if (code == ERROR_ALREADY_EXISTS)
            return 1;
        snprintf(err, err_size, "Could not create directory.");
        return 0;
    }
    free(win_path);
    return 1;
#else
    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno == EEXIST)
        return 1;
    snprintf(err, err_size, "Could not create directory.");
    return 0;
#endif
}

static int rt_make_dir_recursive(const char* path, char* err, size_t err_size) {
    if (!path || !path[0]) {
        snprintf(err, err_size, "Empty directory path.");
        return 0;
    }
    size_t len = strlen(path);
    if (len > SIZE_MAX - 1) {
        snprintf(err, err_size, "Directory path too long.");
        return 0;
    }
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        snprintf(err, err_size, "Out of memory while creating directory.");
        return 0;
    }
    memcpy(buf, path, len + 1);
#ifdef _WIN32
    for (size_t i = 0; i <= len; i++) {
        if (buf[i] == '/')
            buf[i] = '\\';
    }
    char sep = '\\';
#else
    char sep = '/';
#endif

    size_t start = 0;
    if (isalpha((unsigned char)buf[0]) && buf[1] == ':') {
        start = 2;
        if (buf[start] == sep)
            start++;
    } else if (buf[0] == sep) {
        start = 1;
    }

    for (size_t i = start; i <= len; i++) {
        if (buf[i] == sep || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (buf[0] && !(buf[1] == ':' && buf[2] == '\0')) {
                if (!rt_make_dir_single(buf, err, err_size)) {
                    free(buf);
                    return 0;
                }
            }
            buf[i] = saved;
        }
    }
    free(buf);
    return 1;
}

static int rt_path_is_directory(const char* path) {
    if (!path || !path[0])
        return 0;
#ifdef _WIN32
    char* win_path = rt_win_path_copy(path);
    DWORD attr = GetFileAttributesA(win_path);
    free(win_path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
#endif
}

static int rt_remove_tree(const char* path, char* err, size_t err_size) {
    if (!path || !path[0]) {
        snprintf(err, err_size, "Empty directory path.");
        return 0;
    }
    if (rt_path_is_root(path)) {
        snprintf(err, err_size, "Refusing to remove root directory.");
        return 0;
    }
    if (!rt_path_is_directory(path)) {
        snprintf(err, err_size, "Directory does not exist.");
        return 0;
    }
#ifdef _WIN32
    char pattern[1024];
    if (!rt_join_path(pattern, sizeof(pattern), path, "*")) {
        snprintf(err, err_size, "Directory path too long.");
        return 0;
    }
    char* win_pattern = rt_win_path_copy(pattern);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    free(win_pattern);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, err_size, "Could not open directory.");
        return 0;
    }
    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (!rt_join_path(child, sizeof(child), path, name)) {
            FindClose(h);
            snprintf(err, err_size, "Directory entry path too long.");
            return 0;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!rt_remove_tree(child, err, err_size)) {
                FindClose(h);
                return 0;
            }
        } else {
            char* win_child = rt_win_path_copy(child);
            if (!DeleteFileA(win_child)) {
                free(win_child);
                FindClose(h);
                snprintf(err, err_size, "Could not remove file.");
                return 0;
            }
            free(win_child);
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);

    char* win_path = rt_win_path_copy(path);
    if (!RemoveDirectoryA(win_path)) {
        free(win_path);
        snprintf(err, err_size, "Could not remove directory.");
        return 0;
    }
    free(win_path);
    return 1;
#else
    DIR* dir = opendir(path);
    if (!dir) {
        snprintf(err, err_size, "Could not open directory.");
        return 0;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (!rt_join_path(child, sizeof(child), path, name)) {
            closedir(dir);
            snprintf(err, err_size, "Directory entry path too long.");
            return 0;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            closedir(dir);
            snprintf(err, err_size, "Could not stat directory entry.");
            return 0;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!rt_remove_tree(child, err, err_size)) {
                closedir(dir);
                return 0;
            }
        } else {
            if (unlink(child) != 0) {
                closedir(dir);
                snprintf(err, err_size, "Could not remove file.");
                return 0;
            }
        }
    }
    closedir(dir);
    if (rmdir(path) != 0) {
        snprintf(err, err_size, "Could not remove directory.");
        return 0;
    }
    return 1;
#endif
}

void rt_native_io_directory_exists(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Directory path must be a string.", "");
    *out = value_bool(rt_path_is_directory(path_val->string_value));
}

void rt_native_io_create_directory(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Directory path must be a string.", "");
    char err[256];
    if (!rt_make_dir_recursive(path_val->string_value, err, sizeof(err)))
        runtime_error("Directory create failed", err, "");
    *out = value_bool(1);
}

void rt_native_io_list_directories(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Directory path must be a string.", "");
    const char* base = path_val->string_value;
    if (!rt_path_is_directory(base))
        runtime_error("Directory not found", "Directory does not exist.", "");

    List* list = gc_list_new(0);
#ifdef _WIN32
    char pattern[1024];
    if (!rt_join_path(pattern, sizeof(pattern), base, "*"))
        runtime_error("Directory list failed", "Directory path too long.", "");
    char* win_pattern = rt_win_path_copy(pattern);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    free(win_pattern);
    if (h == INVALID_HANDLE_VALUE)
        runtime_error("Directory list failed", "Could not open directory.", "");

    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char child[1024];
            if (!rt_join_path(child, sizeof(child), base, name)) {
                FindClose(h);
                runtime_error("Directory list failed", "Directory entry path too long.", "");
            }
            Value v = value_string(child);
            gc_list_reserve(list, list->count + 1);
            list->items[list->count++] = v;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR* dir = opendir(base);
    if (!dir)
        runtime_error("Directory list failed", "Could not open directory.", "");
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (!rt_join_path(child, sizeof(child), base, name)) {
            closedir(dir);
            runtime_error("Directory list failed", "Directory entry path too long.", "");
        }
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            Value v = value_string(child);
            gc_list_reserve(list, list->count + 1);
            list->items[list->count++] = v;
        }
    }
    closedir(dir);
#endif
    *out = value_list(list);
}

void rt_native_io_list_files(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Directory path must be a string.", "");
    const char* base = path_val->string_value;
    if (!rt_path_is_directory(base))
        runtime_error("Directory not found", "Directory does not exist.", "");

    List* list = gc_list_new(0);
#ifdef _WIN32
    char pattern[1024];
    if (!rt_join_path(pattern, sizeof(pattern), base, "*"))
        runtime_error("Directory list failed", "Directory path too long.", "");
    char* win_pattern = rt_win_path_copy(pattern);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    free(win_pattern);
    if (h == INVALID_HANDLE_VALUE)
        runtime_error("Directory list failed", "Could not open directory.", "");

    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char child[1024];
            if (!rt_join_path(child, sizeof(child), base, name)) {
                FindClose(h);
                runtime_error("Directory list failed", "Directory entry path too long.", "");
            }
            Value v = value_string(child);
            gc_list_reserve(list, list->count + 1);
            list->items[list->count++] = v;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR* dir = opendir(base);
    if (!dir)
        runtime_error("Directory list failed", "Could not open directory.", "");
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (!rt_join_path(child, sizeof(child), base, name)) {
            closedir(dir);
            runtime_error("Directory list failed", "Directory entry path too long.", "");
        }
        struct stat st;
        if (stat(child, &st) == 0 && !S_ISDIR(st.st_mode)) {
            Value v = value_string(child);
            gc_list_reserve(list, list->count + 1);
            list->items[list->count++] = v;
        }
    }
    closedir(dir);
#endif
    *out = value_list(list);
}

static int rt_copy_file_simple(const char* src, const char* dst, char* err, size_t err_size) {
    if (!src || !dst || !err || err_size == 0)
        return 0;
    err[0] = '\0';

    FILE* in = fopen(src, "rb");
    if (!in) {
        snprintf(err, err_size, "Could not open source file.");
        return 0;
    }
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        snprintf(err, err_size, "Could not open destination file.");
        return 0;
    }

    char buf[8192];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fclose(in);
            fclose(out);
            snprintf(err, err_size, "Could not write destination file.");
            return 0;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        snprintf(err, err_size, "Could not read source file.");
        return 0;
    }

    fclose(in);
    fclose(out);
    return 1;
}

static int rt_copy_tree(const char* src, const char* dst, char* err, size_t err_size) {
    if (!src || !dst || !err || err_size == 0)
        return 0;
    if (!rt_path_is_directory(src)) {
        snprintf(err, err_size, "Source directory does not exist.");
        return 0;
    }
    if (!rt_make_dir_recursive(dst, err, err_size))
        return 0;

#ifdef _WIN32
    char pattern[1024];
    if (!rt_join_path(pattern, sizeof(pattern), src, "*")) {
        snprintf(err, err_size, "Directory path too long.");
        return 0;
    }
    char* win_pattern = rt_win_path_copy(pattern);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    free(win_pattern);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, err_size, "Could not open directory.");
        return 0;
    }

    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child_src[1024];
        char child_dst[1024];
        if (!rt_join_path(child_src, sizeof(child_src), src, name) ||
            !rt_join_path(child_dst, sizeof(child_dst), dst, name)) {
            FindClose(h);
            snprintf(err, err_size, "Directory entry path too long.");
            return 0;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!rt_copy_tree(child_src, child_dst, err, err_size)) {
                FindClose(h);
                return 0;
            }
        } else {
            if (!rt_copy_file_simple(child_src, child_dst, err, err_size)) {
                FindClose(h);
                return 0;
            }
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR* dir = opendir(src);
    if (!dir) {
        snprintf(err, err_size, "Could not open directory.");
        return 0;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child_src[1024];
        char child_dst[1024];
        if (!rt_join_path(child_src, sizeof(child_src), src, name) ||
            !rt_join_path(child_dst, sizeof(child_dst), dst, name)) {
            closedir(dir);
            snprintf(err, err_size, "Directory entry path too long.");
            return 0;
        }
        struct stat st;
        if (stat(child_src, &st) != 0) {
            closedir(dir);
            snprintf(err, err_size, "Could not stat directory entry.");
            return 0;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!rt_copy_tree(child_src, child_dst, err, err_size)) {
                closedir(dir);
                return 0;
            }
        } else {
            if (!rt_copy_file_simple(child_src, child_dst, err, err_size)) {
                closedir(dir);
                return 0;
            }
        }
    }
    closedir(dir);
#endif
    return 1;
}

void rt_native_io_copy_file(Value* out, const Value* src_val, const Value* dst_val) {
    if (!out || !src_val || !dst_val)
        rt_null_value_error();
    if (src_val->type != VALUE_STRING || dst_val->type != VALUE_STRING)
        runtime_error("Invalid file path", "Source and destination must be strings.", "");
    char err[256];
    if (!rt_copy_file_simple(src_val->string_value, dst_val->string_value, err, sizeof(err)))
        runtime_error("File copy failed", err, "");
    *out = value_bool(1);
}

void rt_native_io_copy_directory(Value* out, const Value* src_val, const Value* dst_val) {
    if (!out || !src_val || !dst_val)
        rt_null_value_error();
    if (src_val->type != VALUE_STRING || dst_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Source and destination must be strings.", "");
    char err[256];
    if (!rt_copy_tree(src_val->string_value, dst_val->string_value, err, sizeof(err)))
        runtime_error("Directory copy failed", err, "");
    *out = value_bool(1);
}

void rt_native_io_remove_directory(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid directory path", "Directory path must be a string.", "");
    char err[256];
    if (!rt_remove_tree(path_val->string_value, err, sizeof(err)))
        runtime_error("Directory remove failed", err, "");
    *out = value_bool(1);
}

void rt_native_io_delete_file(Value* out, const Value* path_val) {
    if (!out || !path_val)
        rt_null_value_error();
    if (path_val->type != VALUE_STRING)
        runtime_error("Invalid file path", "File path must be a string.", "");
#ifdef _WIN32
    char* win_path = rt_win_path_copy(path_val->string_value);
    if (!DeleteFileA(win_path)) {
        DWORD code = GetLastError();
        free(win_path);
        if (code == ERROR_FILE_NOT_FOUND) {
            *out = value_bool(0);
            return;
        }
        runtime_error("File delete failed", "Could not delete file.", "");
    }
    free(win_path);
#else
    if (unlink(path_val->string_value) != 0) {
        if (errno == ENOENT) {
            *out = value_bool(0);
            return;
        }
        runtime_error("File delete failed", "Could not delete file.", "");
    }
#endif
    *out = value_bool(1);
}

static char* rt_find_last_substring(char* haystack, const char* needle) {
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

static int run_http_libcurl_request(const char* method, const char* url, const char* body, const Value* headers, char** out_body, int* out_status, char* err, size_t err_size) {
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
    if (headers && headers->type == VALUE_DICT) {
        Dict* dict = headers->dict_value;
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

    if (!buf.data) {
        buf.data = rt_strdup_checked("");
    }
    *out_body = buf.data;
    return 1;
}

static int run_http_libcurl_download(const char* url, const Value* headers, const char* output_path, int* out_status, char* err, size_t err_size) {
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
    if (headers && headers->type == VALUE_DICT) {
        Dict* dict = headers->dict_value;
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

static int winhttp_add_headers(HINTERNET request, const Value* headers, char* err, size_t err_size) {
    if (!headers || headers->type != VALUE_DICT)
        return 1;
    Dict* dict = headers->dict_value;
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

static int run_http_winhttp_request(const char* method, const char* url, const char* body, const Value* headers, char** out_body, int* out_status, char* err, size_t err_size) {
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

static int run_http_winhttp_download(const char* url, const Value* headers, const char* output_path, int* out_status, char* err, size_t err_size) {
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

static Value make_http_result(int ok, int status, const char* body, const char* error_message) {
    const char* err = (error_message && error_message[0]) ? error_message : "";
    if (status == 0 && err[0] == '\0')
        err = "HTTP request failed.";
    Dict* out = gc_dict_new(4);
    gc_dict_set_key(out, "ok", value_bool(ok ? 1 : 0), 0);
    gc_dict_set_key(out, "status", value_int(status), 0);
    gc_dict_set_key(out, "body", value_string(body ? body : ""), 0);
    gc_dict_set_key(out, "error", value_string(err), 0);
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
    list->items[list->count++] = rt_strdup_checked(value);
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

static int append_curl_headers(const Value* headers, ArgList* args, char* err, size_t err_size) {
    if (!args)
        return 0;
    if (!headers || headers->type != VALUE_DICT)
        return 1;

    Dict* dict = headers->dict_value;
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
    return rt_strdup_checked(temp_file);
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
    return rt_strdup_checked(tmpl);
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

static int run_process_simple(const ArgList* args, char* err, size_t err_size) {
    if (!args || args->count <= 0 || !err || err_size == 0)
        return 0;
    err[0] = '\0';

#ifdef _WIN32
    size_t cmd_cap = 1024;
    size_t cmd_len = 0;
    char* cmd = malloc(cmd_cap);
    if (!cmd) {
        snprintf(err, err_size, "Out of memory while building command.");
        return 0;
    }
    cmd[0] = '\0';

    for (int i = 0; i < args->count; i++) {
        const char* arg = args->items[i] ? args->items[i] : "";
        size_t needed = 0;
        if (!size_add_checked(cmd_len, 3, &needed) ||
            !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                "Out of memory while building command.",
                "Command too long.")) {
            free(cmd);
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
                        "Out of memory while building command.",
                        "Command too long.")) {
                    free(cmd);
                    return 0;
                }
                cmd[cmd_len++] = '\\';
                continue;
            }
            if (*p == '"') {
                for (int j = 0; j < backslashes; j++) {
                    if (!size_add_checked(cmd_len, 1, &needed) ||
                        !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                            "Out of memory while building command.",
                            "Command too long.")) {
                        free(cmd);
                        return 0;
                    }
                    cmd[cmd_len++] = '\\';
                }
                backslashes = 0;
                if (!size_add_checked(cmd_len, 2, &needed) ||
                    !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                        "Out of memory while building command.",
                        "Command too long.")) {
                    free(cmd);
                    return 0;
                }
                cmd[cmd_len++] = '\\';
                cmd[cmd_len++] = '"';
                continue;
            }
            backslashes = 0;
            if (!size_add_checked(cmd_len, 1, &needed) ||
                !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                    "Out of memory while building command.",
                    "Command too long.")) {
                free(cmd);
                return 0;
            }
            cmd[cmd_len++] = *p;
        }
        for (int j = 0; j < backslashes; j++) {
            if (!size_add_checked(cmd_len, 1, &needed) ||
                !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                    "Out of memory while building command.",
                    "Command too long.")) {
                free(cmd);
                return 0;
            }
            cmd[cmd_len++] = '\\';
        }
        if (!size_add_checked(cmd_len, 2, &needed) ||
            !ensure_buffer_capacity(&cmd, &cmd_cap, needed, err, err_size,
                "Out of memory while building command.",
                "Command too long.")) {
            free(cmd);
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

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) {
        snprintf(err, err_size, "Could not launch process.");
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != 0) {
        snprintf(err, err_size, "Process exited with code %lu.", (unsigned long)exit_code);
        return 0;
    }
    return 1;
#else
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_size, "Could not fork process.");
        return 0;
    }
    if (pid == 0) {
        char** argv = calloc((size_t)args->count + 1, sizeof(char*));
        if (!argv)
            _exit(127);
        for (int i = 0; i < args->count; i++)
            argv[i] = args->items[i];
        argv[args->count] = NULL;
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err, err_size, "Could not wait for process.");
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, err_size, "Process exited with error.");
        return 0;
    }
    return 1;
#endif
}

static char* rt_escape_powershell_single(const char* text) {
    if (!text)
        text = "";
    size_t len = strlen(text);
    size_t extra = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\'')
            extra++;
    }
    if (len > SIZE_MAX - extra - 1)
        runtime_error("Out of memory", "String too long to escape.", "");
    char* out = (char*)malloc(len + extra + 1);
    if (!out)
        runtime_error("Out of memory", "Could not allocate escape buffer.", "");
    size_t pos = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\'') {
            out[pos++] = '\'';
            out[pos++] = '\'';
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';
    return out;
}

static int rt_zip_extract_internal(const char* zip_path, const char* dest_dir, char* err, size_t err_size) {
    if (!zip_path || !zip_path[0] || !dest_dir || !dest_dir[0]) {
        snprintf(err, err_size, "Zip path or destination missing.");
        return 0;
    }
    ArgList args;
    arglist_init(&args);
#ifdef _WIN32
    char* zip_esc = rt_escape_powershell_single(zip_path);
    char* dest_esc = rt_escape_powershell_single(dest_dir);
    size_t cmd_len = strlen(zip_esc) + strlen(dest_esc) + 128;
    char* cmd = (char*)malloc(cmd_len);
    if (!cmd) {
        free(zip_esc);
        free(dest_esc);
        snprintf(err, err_size, "Out of memory while building zip command.");
        return 0;
    }
    snprintf(cmd, cmd_len, "Expand-Archive -Path '%s' -DestinationPath '%s' -Force", zip_esc, dest_esc);
    free(zip_esc);
    free(dest_esc);
    arglist_push(&args, "powershell");
    arglist_push(&args, "-NoProfile");
    arglist_push(&args, "-ExecutionPolicy");
    arglist_push(&args, "Bypass");
    arglist_push(&args, "-Command");
    arglist_push(&args, cmd);
    free(cmd);
#else
    arglist_push(&args, "unzip");
    arglist_push(&args, "-o");
    arglist_push(&args, zip_path);
    arglist_push(&args, "-d");
    arglist_push(&args, dest_dir);
#endif
    int ok = run_process_simple(&args, err, err_size);
    arglist_free(&args);
    return ok;
}

static void read_stderr_snippet(const char* path, char* err, size_t err_size);

static int run_http_curl_process_request(const char* method, const char* url, const char* body, const Value* headers, char** out_body, int* out_status, char* err, size_t err_size) {
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
            stderr_file = rt_strdup_checked(temp_file);
    }
#else
    char tmpl[] = "/tmp/sicht_http_err_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        close(fd);
        stderr_file = rt_strdup_checked(tmpl);
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
    char* at = rt_find_last_substring(buffer, marker);
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

static int run_http_curl_request(const char* method, const char* url, const char* body, const Value* headers, char** out_body, int* out_status, char* err, size_t err_size) {
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

static int run_http_curl_process_download(const char* url, const char* output_path, const Value* headers, int* out_status, char* err, size_t err_size) {
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
            stderr_file = rt_strdup_checked(temp_file);
    }
#else
    char tmpl[] = "/tmp/sicht_http_err_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        close(fd);
        stderr_file = rt_strdup_checked(tmpl);
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
    char* at = rt_find_last_substring(buffer, marker);
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

static int run_http_curl_download(const char* url, const char* output_path, const Value* headers, int* out_status, char* err, size_t err_size) {
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

void rt_native_http_get(Value* out, const Value* url, const Value* headers) {
    if (!out || !url)
        rt_null_value_error();
    if (url->type != VALUE_STRING)
        runtime_error("Invalid native HTTP call", "HTTP url must be a string.", "");
    if (headers && headers->type != VALUE_DICT)
        runtime_error("Invalid native HTTP call", "HTTP headers must be a dictionary of string values.", "");

    char* response_body = NULL;
    int status = 0;
    char err[256];
    if (!run_http_curl_request("GET", url->string_value, "", headers, &response_body, &status, err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return;
    }
    *out = make_http_result(status >= 200 && status < 300, status, response_body ? response_body : "", "");
    free(response_body);
}

void rt_native_http_post(Value* out, const Value* url, const Value* body, const Value* headers) {
    if (!out || !url || !body)
        rt_null_value_error();
    if (url->type != VALUE_STRING || body->type != VALUE_STRING)
        runtime_error("Invalid native HTTP call", "HTTP url/body must be strings.", "");
    if (headers && headers->type != VALUE_DICT)
        runtime_error("Invalid native HTTP call", "HTTP headers must be a dictionary of string values.", "");

    char* response_body = NULL;
    int status = 0;
    char err[256];
    if (!run_http_curl_request("POST", url->string_value, body->string_value, headers, &response_body, &status, err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return;
    }
    *out = make_http_result(status >= 200 && status < 300, status, response_body ? response_body : "", "");
    free(response_body);
}

void rt_native_http_request(Value* out, const Value* method, const Value* url, const Value* body, const Value* headers) {
    if (!out || !method || !url)
        rt_null_value_error();
    if (method->type != VALUE_STRING || url->type != VALUE_STRING)
        runtime_error("Invalid native HTTP call", "HTTP method/url must be strings.", "");

    const Value* body_value = body;
    const Value* headers_value = headers;
    Value empty_body = value_string("");

    if (headers_value && headers_value->type != VALUE_DICT)
        runtime_error("Invalid native HTTP call", "HTTP headers must be a dictionary of string values.", "");

    if (!headers_value && body_value && body_value->type == VALUE_DICT) {
        headers_value = body_value;
        body_value = NULL;
    }

    if (body_value && body_value->type != VALUE_STRING)
        runtime_error("Invalid native HTTP call", "HTTP body must be a string.", "");

    char method_buf[16];
    char err[256];
    if (!normalize_http_method(method->string_value, method_buf, sizeof(method_buf), err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return;
    }

    const char* body_text = body_value ? body_value->string_value : empty_body.string_value;
    char* response_body = NULL;
    int status = 0;
    if (!run_http_curl_request(method_buf, url->string_value, body_text, headers_value, &response_body, &status, err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return;
    }
    *out = make_http_result(status >= 200 && status < 300, status, response_body ? response_body : "", "");
    free(response_body);
}

void rt_native_http_download(Value* out, const Value* url, const Value* path, const Value* headers) {
    if (!out || !url || !path)
        rt_null_value_error();
    if (url->type != VALUE_STRING || path->type != VALUE_STRING)
        runtime_error("Invalid native HTTP call", "HTTP url/path must be strings.", "");
    if (headers && headers->type != VALUE_DICT)
        runtime_error("Invalid native HTTP call", "HTTP headers must be a dictionary of string values.", "");

    int status = 0;
    char err[256];
    if (!run_http_curl_download(url->string_value, path->string_value, headers, &status, err, sizeof(err))) {
        *out = make_http_result(0, 0, "", err);
        return;
    }
    *out = make_http_result(status >= 200 && status < 300, status, "", err);
}

void rt_native_time_ms(Value* out) {
    if (!out)
        rt_null_value_error();
#ifdef _WIN32
    ULONGLONG ms = GetTickCount64();
    *out = value_float((double)ms);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        *out = value_float(0.0);
        return;
    }
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    *out = value_float(ms);
#endif
}

void rt_native_env_get(Value* out, const Value* name_val) {
    if (!out || !name_val)
        rt_null_value_error();
    if (name_val->type != VALUE_STRING)
        runtime_error("Invalid environment query", "Environment variable name must be a string.", "");
    const char* value = getenv(name_val->string_value);
    *out = value_string(value ? value : "");
}

void rt_native_zip_extract(Value* out, const Value* zip_path, const Value* dest_dir) {
    if (!out || !zip_path || !dest_dir)
        rt_null_value_error();
    if (zip_path->type != VALUE_STRING || dest_dir->type != VALUE_STRING)
        runtime_error("Invalid zip extract", "Zip path and destination must be strings.", "");
    char err[256];
    if (!rt_zip_extract_internal(zip_path->string_value, dest_dir->string_value, err, sizeof(err)))
        runtime_error("Zip extract failed", err, "Ensure unzip or PowerShell is available.");
    *out = value_bool(1);
}

void rt_native_process_run(Value* out, const Value* command_val) {
    if (!out || !command_val)
        rt_null_value_error();
    if (command_val->type != VALUE_STRING)
        runtime_error("Invalid process run", "Command must be a string.", "");
    int rc = system(command_val->string_value);
    *out = value_bool(rc == 0);
}

void rt_native_process_id(Value* out) {
    if (!out)
        rt_null_value_error();
#ifdef _WIN32
    *out = value_int((int)GetCurrentProcessId());
#else
    *out = value_int((int)getpid());
#endif
}

void rt_make_generator(Value* out, void* fn_ptr, const Value* args, int arg_count) {
    if (!out)
        rt_null_value_error();
    if (!fn_ptr)
        runtime_error("Invalid generator", "Generator has no function body.", "");
    Generator* gen = gc_generator_new(arg_count);
    gen->function_node = fn_ptr;
    gen->arg_count = arg_count;
    gen->initialized = 0;
    gen->index = 0;
    gen->cache = NULL;
    if (arg_count > 0 && args) {
        for (int i = 0; i < arg_count; i++)
            gen->args[i] = args[i];
    }
    *out = value_generator(gen);
}

int rt_generator_next(Value* out, const Value* gen_val) {
    if (!gen_val || gen_val->type != VALUE_GENERATOR)
        return 0;
    Generator* g = gen_val->generator_value;
    if (!g)
        return 0;
    rt_materialize_generator(g);
    if (!g->cache || g->index >= g->cache->count)
        return 0;
    if (out)
        *out = g->cache->items[g->index];
    g->index++;
    if (g->index >= g->cache->count) {
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

int rt_collection_count(const Value* source) {
    if (!source)
        rt_null_value_error();
    if (source->type == VALUE_LIST)
        return source->list_value->count;
    if (source->type == VALUE_DICT)
        return source->dict_value->count;
    if (source->type == VALUE_STRING)
        return rt_size_to_int_or_error(
            strlen(source->string_value),
            "String too large",
            "String length exceeds supported integer range."
        );
    if (source->type == VALUE_GENERATOR) {
        rt_materialize_generator(source->generator_value);
        if (!source->generator_value->cache)
            return 0;
        return source->generator_value->cache->count - source->generator_value->index;
    }
    runtime_error("Invalid iterable", "Target must be a list, dictionary, string, or generator.", "");
    return -1;
}

void rt_collection_item(Value* out, const Value* source, int index) {
    if (!out || !source)
        rt_null_value_error();
    if (source->type == VALUE_LIST) {
        *out = source->list_value->items[index];
        return;
    }
    if (source->type == VALUE_DICT) {
        *out = value_string(source->dict_value->entries[index].key);
        return;
    }
    if (source->type == VALUE_STRING) {
        char buf[2];
        buf[0] = source->string_value[index];
        buf[1] = '\0';
        *out = value_string(buf);
        return;
    }
    if (source->type == VALUE_GENERATOR) {
        Generator* g = source->generator_value;
        rt_materialize_generator(g);
        *out = g->cache->items[g->index + index];
        return;
    }
    runtime_error("Invalid iterable", "Target must be a list, dictionary, string, or generator.", "");
}

void rt_llvl_set_gc_paused(int paused) {
    gc_set_paused(paused ? 1 : 0);
}

void rt_llvl_require(void) {
    if (gc_is_paused())
        return;
    runtime_error(
        "Low-level operation",
        "This operation is only available inside llvl blocks.",
        "Wrap the code with: llvl ... end llvl"
    );
}

typedef struct {
    Buffer* buffer;
    unsigned char* base;
    size_t size;
} LlvlBufferEntry;

static LlvlBufferEntry* llvl_buffers = NULL;
static int llvl_buffer_count = 0;
static int llvl_buffer_capacity = 0;
static int llvl_bounds_check_enabled = 1;
static int llvl_pointer_check_enabled = 0;

void rt_llvl_set_bounds_check(int enabled) {
    llvl_bounds_check_enabled = enabled ? 1 : 0;
}

void rt_llvl_set_pointer_checks(int enabled) {
    llvl_pointer_check_enabled = enabled ? 1 : 0;
}

static void llvl_track_buffer(Buffer* buf) {
    if (!buf)
        return;
    for (int i = 0; i < llvl_buffer_count; i++) {
        if (llvl_buffers[i].buffer == buf) {
            llvl_buffers[i].base = buf->data;
            llvl_buffers[i].size = buf->size;
            return;
        }
    }

    if (llvl_buffer_count >= llvl_buffer_capacity) {
        int new_cap = llvl_buffer_capacity > 0 ? llvl_buffer_capacity * 2 : 16;
        if (new_cap < llvl_buffer_capacity)
            runtime_error("Low-level tracking", "Buffer registry overflow.", "");
        if ((size_t)new_cap > SIZE_MAX / sizeof(LlvlBufferEntry))
            runtime_error("Low-level tracking", "Buffer registry overflow.", "");
        LlvlBufferEntry* grown = (LlvlBufferEntry*)realloc(llvl_buffers, (size_t)new_cap * sizeof(LlvlBufferEntry));
        if (!grown)
            runtime_error("Out of memory", "Could not grow buffer registry.", "");
        memset(grown + llvl_buffer_capacity, 0, (size_t)(new_cap - llvl_buffer_capacity) * sizeof(LlvlBufferEntry));
        llvl_buffers = grown;
        llvl_buffer_capacity = new_cap;
    }

    llvl_buffers[llvl_buffer_count].buffer = buf;
    llvl_buffers[llvl_buffer_count].base = buf->data;
    llvl_buffers[llvl_buffer_count].size = buf->size;
    llvl_buffer_count++;
}

static void llvl_untrack_buffer(Buffer* buf) {
    if (!buf)
        return;
    for (int i = 0; i < llvl_buffer_count; i++) {
        if (llvl_buffers[i].buffer == buf) {
            llvl_buffers[i] = llvl_buffers[llvl_buffer_count - 1];
            llvl_buffer_count--;
            return;
        }
    }
}

static const LlvlBufferEntry* llvl_find_buffer_for_ptr(const unsigned char* ptr) {
    if (!ptr)
        return NULL;
    for (int i = 0; i < llvl_buffer_count; i++) {
        const LlvlBufferEntry* entry = &llvl_buffers[i];
        if (!entry->base || entry->size == 0)
            continue;
        if (ptr >= entry->base && ptr < entry->base + entry->size)
            return entry;
    }
    return NULL;
}

static void llvl_validate_address(const unsigned char* ptr, size_t needed, const char* title) {
    if (!llvl_pointer_check_enabled)
        return;
    if (!ptr)
        runtime_error(title, "Null pointer.", "");
    const LlvlBufferEntry* entry = llvl_find_buffer_for_ptr(ptr);
    if (!entry)
        runtime_error(title, "Pointer is not within a tracked buffer.", "");
    if (llvl_bounds_check_enabled && needed > 0) {
        size_t offset = (size_t)(ptr - entry->base);
        if (offset + needed > entry->size)
            runtime_error(title, "Pointer exceeds buffer bounds.", "");
    }
}

static Buffer* rt_require_buffer(Value* v, const char* title) {
    if (!v)
        rt_null_value_error();
    if (v->type != VALUE_BUFFER || !v->buffer_value) {
        runtime_error(
            title,
            "Target must be a valid buffer.",
            "Use: save <size> for buffer"
        );
    }
    return v->buffer_value;
}

static size_t rt_size_from_value(const Value* v, const char* title) {
    if (!v)
        rt_null_value_error();
    double d = 0.0;
    if (v->type == VALUE_INT) {
        d = (double)v->int_value;
    } else if (v->type == VALUE_FLOAT) {
        d = v->float_value;
    } else if (v->type == VALUE_BOOL) {
        d = v->int_value ? 1.0 : 0.0;
    } else {
        runtime_error(title, "Size must be an integer or float.", "Cast the size to integer or float.");
    }
    if (!(d >= 0.0))
        runtime_error(title, "Size must be non-negative.", "");
    if (d > (double)SIZE_MAX)
        runtime_error(title, "Size is too large.", "");
    return (size_t)d;
}

static int rt_int_from_value(const Value* v, const char* title) {
    if (!v)
        rt_null_value_error();
    if (v->type == VALUE_INT)
        return v->int_value;
    if (v->type == VALUE_BOOL)
        return v->int_value ? 1 : 0;
    if (v->type == VALUE_FLOAT)
        return (int)v->float_value;
    runtime_error(title, "Value must be an integer, float, or boolean.", "");
    return 0;
}

void rt_llvl_alloc(Value* out, const Value* size_val) {
    if (!out)
        rt_null_value_error();
    size_t size = rt_size_from_value(size_val, "Invalid buffer size");
    Buffer* buf = (Buffer*)malloc(sizeof(Buffer));
    if (!buf)
        runtime_error("Out of memory", "Could not allocate buffer metadata.", "");
    buf->size = size;
    buf->data = NULL;
    if (size > 0) {
        buf->data = (unsigned char*)calloc(1, size);
        if (!buf->data)
            runtime_error("Out of memory", "Could not allocate buffer data.", "");
    }
    buf->elem_size = 1;
    buf->elem_kind = BUFFER_ELEM_RAW;
    buf->elem_type_name = NULL;
    *out = value_buffer(buf);
    llvl_track_buffer(buf);
}

void rt_llvl_free(Value* buffer_val) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid buffer");
    if (!buf)
        return;
    llvl_untrack_buffer(buf);
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    free(buf);
    buffer_val->buffer_value = NULL;
    buffer_val->type = VALUE_BUFFER;
}

void rt_llvl_resize(Value* buffer_val, const Value* size_val, int is_grow) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid buffer resize");
    size_t new_size = rt_size_from_value(size_val, "Invalid buffer size");
    if (is_grow && new_size < buf->size)
        runtime_error("Invalid grow size", "New size must be >= current size.", "");
    if (!is_grow && new_size > buf->size)
        runtime_error("Invalid shrink size", "New size must be <= current size.", "");

    if (new_size == buf->size)
        return;

    unsigned char* new_data = NULL;
    if (new_size > 0) {
        new_data = (unsigned char*)realloc(buf->data, new_size);
        if (!new_data)
            runtime_error("Out of memory", "Could not resize buffer.", "");
        if (new_size > buf->size)
            memset(new_data + buf->size, 0, new_size - buf->size);
    } else {
        free(buf->data);
    }

    buf->data = new_data;
    buf->size = new_size;
    llvl_track_buffer(buf);
}

void rt_llvl_resize_any(Value* buffer_val, const Value* size_val) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid buffer resize");
    size_t new_size = rt_size_from_value(size_val, "Invalid buffer size");

    if (new_size == buf->size)
        return;

    unsigned char* new_data = NULL;
    if (new_size > 0) {
        new_data = (unsigned char*)realloc(buf->data, new_size);
        if (!new_data)
            runtime_error("Out of memory", "Could not resize buffer.", "");
        if (new_size > buf->size)
            memset(new_data + buf->size, 0, new_size - buf->size);
    } else {
        free(buf->data);
    }

    buf->data = new_data;
    buf->size = new_size;
    llvl_track_buffer(buf);
}

void rt_llvl_set_buffer_meta(Value* buffer_val, size_t elem_size, int elem_kind, const char* elem_type_name) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid buffer metadata");
    if (!buf)
        return;
    if (elem_size == 0)
        elem_size = 1;
    buf->elem_size = elem_size;
    buf->elem_kind = (BufferElemKind)elem_kind;
    buf->elem_type_name = elem_type_name;
}

typedef struct {
    const char* name;
    int kind;
    int array_len;
    size_t offset;
    size_t size;
} LlvlFieldRt;

typedef struct {
    const char* name;
    int is_union;
    int field_count;
    LlvlFieldRt* fields;
    size_t size;
} LlvlTypeRt;

static LlvlTypeRt* llvl_rt_types = NULL;
static int llvl_rt_type_count = 0;
static int llvl_rt_type_capacity = 0;

static LlvlTypeRt* llvl_rt_find_type(const char* name) {
    if (!name)
        return NULL;
    for (int i = 0; i < llvl_rt_type_count; i++) {
        if (llvl_rt_types[i].name && strcmp(llvl_rt_types[i].name, name) == 0)
            return &llvl_rt_types[i];
    }
    return NULL;
}

static const LlvlFieldRt* llvl_rt_find_field(const LlvlTypeRt* def, const char* field_name) {
    if (!def || !field_name)
        return NULL;
    for (int i = 0; i < def->field_count; i++) {
        if (def->fields[i].name && strcmp(def->fields[i].name, field_name) == 0)
            return &def->fields[i];
    }
    return NULL;
}

static BufferElemKind llvl_rt_kind_to_buffer_kind(int kind) {
    switch (kind) {
        case 0: return BUFFER_ELEM_BYTE;
        case 1: return BUFFER_ELEM_INT;
        case 2: return BUFFER_ELEM_FLOAT;
        case 3: return BUFFER_ELEM_STRUCT;
        case 4: return BUFFER_ELEM_UNION;
        default: return BUFFER_ELEM_RAW;
    }
}

void rt_llvl_register_type(const char* name, int is_union, int field_count, size_t size) {
    if (!name || !name[0])
        runtime_error("Invalid low-level type", "Type name is missing.", "");
    if (llvl_rt_find_type(name))
        runtime_error("Low-level type conflict", "Type name already exists.", "");
    if (field_count < 0)
        runtime_error("Invalid low-level type", "Field count cannot be negative.", "");

    if (llvl_rt_type_count >= llvl_rt_type_capacity) {
        int new_cap = llvl_rt_type_capacity > 0 ? llvl_rt_type_capacity * 2 : 16;
        if (new_cap < llvl_rt_type_capacity)
            runtime_error("Low-level type registry", "Type registry overflow.", "");
        if ((size_t)new_cap > SIZE_MAX / sizeof(LlvlTypeRt))
            runtime_error("Low-level type registry", "Type registry overflow.", "");
        LlvlTypeRt* grown = (LlvlTypeRt*)realloc(llvl_rt_types, (size_t)new_cap * sizeof(LlvlTypeRt));
        if (!grown)
            runtime_error("Out of memory", "Could not grow low-level type registry.", "");
        memset(grown + llvl_rt_type_capacity, 0, (size_t)(new_cap - llvl_rt_type_capacity) * sizeof(LlvlTypeRt));
        llvl_rt_types = grown;
        llvl_rt_type_capacity = new_cap;
    }

    LlvlTypeRt* slot = &llvl_rt_types[llvl_rt_type_count++];
    memset(slot, 0, sizeof(LlvlTypeRt));
    slot->name = name;
    slot->is_union = is_union ? 1 : 0;
    slot->field_count = field_count;
    slot->size = size;

    if (field_count > 0) {
        slot->fields = (LlvlFieldRt*)calloc((size_t)field_count, sizeof(LlvlFieldRt));
        if (!slot->fields)
            runtime_error("Out of memory", "Could not allocate low-level type fields.", "");
    }
}

void rt_llvl_register_field(const char* type_name, int field_index, const char* field_name, int kind, int array_len, size_t offset, size_t size) {
    LlvlTypeRt* def = llvl_rt_find_type(type_name);
    if (!def)
        runtime_error("Unknown low-level type", "Type was not registered.", "");
    if (!def->fields || def->field_count <= 0)
        runtime_error("Invalid low-level field", "Type has no fields.", "");
    if (field_index < 0 || field_index >= def->field_count)
        runtime_error("Invalid low-level field", "Field index out of bounds.", "");
    if (!field_name || !field_name[0])
        runtime_error("Invalid low-level field", "Field name is missing.", "");

    LlvlFieldRt* field = &def->fields[field_index];
    field->name = field_name;
    field->kind = kind;
    field->array_len = array_len;
    field->offset = offset;
    field->size = size;
}

void rt_llvl_field_get(Value* out, const Value* buffer_val, const char* field_name) {
    if (!out)
        rt_null_value_error();
    if (!buffer_val || buffer_val->type != VALUE_BUFFER || !buffer_val->buffer_value)
        runtime_error("Invalid field access", "Target must be a struct/union buffer.", "");

    Buffer* buf = buffer_val->buffer_value;
    if (buf->elem_kind != BUFFER_ELEM_STRUCT && buf->elem_kind != BUFFER_ELEM_UNION)
        runtime_error("Invalid field access", "Buffer is not a struct/union allocation.", "");
    if (!buf->elem_type_name)
        runtime_error("Invalid field access", "Struct/union type name is missing.", "");

    LlvlTypeRt* def = llvl_rt_find_type(buf->elem_type_name);
    if (!def)
        runtime_error("Invalid field access", "Unknown low-level type.", "");

    const LlvlFieldRt* field = llvl_rt_find_field(def, field_name);
    if (!field)
        runtime_error("Invalid field access", "Field does not exist.", "");

    if (field->offset + field->size > buf->size)
        runtime_error("Invalid field access", "Field exceeds buffer size.", "");

    unsigned char* base = buf->data + field->offset;
    if (field->array_len > 0 || field->kind == 3 || field->kind == 4) {
        *out = value_address(base);
        return;
    }

    BufferElemKind kind = llvl_rt_kind_to_buffer_kind(field->kind);
    Value addr = value_address(base);
    rt_llvl_get_at_typed(out, &addr, (int)kind);
}

void rt_llvl_field_set(const Value* buffer_val, const char* field_name, const Value* value_val) {
    if (!buffer_val || buffer_val->type != VALUE_BUFFER || !buffer_val->buffer_value)
        runtime_error("Invalid field write", "Target must be a struct/union buffer.", "");
    if (!value_val)
        rt_null_value_error();

    Buffer* buf = buffer_val->buffer_value;
    if (buf->elem_kind != BUFFER_ELEM_STRUCT && buf->elem_kind != BUFFER_ELEM_UNION)
        runtime_error("Invalid field write", "Buffer is not a struct/union allocation.", "");
    if (!buf->elem_type_name)
        runtime_error("Invalid field write", "Struct/union type name is missing.", "");

    LlvlTypeRt* def = llvl_rt_find_type(buf->elem_type_name);
    if (!def)
        runtime_error("Invalid field write", "Unknown low-level type.", "");

    const LlvlFieldRt* field = llvl_rt_find_field(def, field_name);
    if (!field)
        runtime_error("Invalid field write", "Field does not exist.", "");

    if (field->array_len > 0 || field->kind == 3 || field->kind == 4)
        runtime_error("Invalid field write", "Cannot assign to struct/array field directly.", "");

    if (field->offset + field->size > buf->size)
        runtime_error("Invalid field write", "Field exceeds buffer size.", "");

    unsigned char* base = buf->data + field->offset;
    BufferElemKind kind = llvl_rt_kind_to_buffer_kind(field->kind);
    Value addr = value_address(base);
    rt_llvl_set_at_typed(&addr, value_val, (int)kind);
}

void rt_llvl_copy(Value* out, const Value* src_val) {
    if (!out || !src_val)
        rt_null_value_error();
    if (src_val->type != VALUE_BUFFER || !src_val->buffer_value)
        runtime_error("Invalid buffer copy", "Source must be a valid buffer.", "");
    Buffer* src = src_val->buffer_value;
    Buffer* buf = (Buffer*)malloc(sizeof(Buffer));
    if (!buf)
        runtime_error("Out of memory", "Could not allocate buffer metadata.", "");
    buf->size = src->size;
    buf->data = NULL;
    if (buf->size > 0) {
        buf->data = (unsigned char*)malloc(buf->size);
        if (!buf->data)
            runtime_error("Out of memory", "Could not allocate buffer data.", "");
        memcpy(buf->data, src->data, buf->size);
    }
    buf->elem_size = src->elem_size;
    buf->elem_kind = src->elem_kind;
    buf->elem_type_name = src->elem_type_name;
    *out = value_buffer(buf);
    llvl_track_buffer(buf);
}

void rt_llvl_copy_bytes(const Value* src_val, Value* dst_val, const Value* size_val) {
    if (!src_val || !dst_val || !size_val)
        rt_null_value_error();
    if (src_val->type != VALUE_BUFFER || !src_val->buffer_value)
        runtime_error("Invalid buffer copy", "Source must be a valid buffer.", "");
    Buffer* dst = rt_require_buffer(dst_val, "Invalid buffer copy");
    Buffer* src = src_val->buffer_value;
    size_t size = rt_size_from_value(size_val, "Invalid copy size");
    if (llvl_bounds_check_enabled) {
        if (size > src->size || size > dst->size)
            runtime_error("Copy out of bounds", "Copy size exceeds buffer size.", "");
    }
    if (size == 0)
        return;
    memmove(dst->data, src->data, size);
}

void rt_llvl_move(Value* dst_val, Value* src_val) {
    if (!dst_val || !src_val)
        rt_null_value_error();
    if (src_val->type != VALUE_BUFFER || !src_val->buffer_value)
        runtime_error("Invalid move", "Source must be a valid buffer.", "");
    if (dst_val == src_val)
        return;

    if (dst_val->type == VALUE_BUFFER && dst_val->buffer_value) {
        Buffer* existing = dst_val->buffer_value;
        llvl_untrack_buffer(existing);
        if (existing->data)
            free(existing->data);
        free(existing);
    }

    dst_val->type = VALUE_BUFFER;
    dst_val->buffer_value = src_val->buffer_value;
    dst_val->address_value = NULL;
    src_val->buffer_value = NULL;
    src_val->type = VALUE_BUFFER;
}

void rt_llvl_get_value(Value* out, const Value* src_val) {
    if (!out || !src_val)
        rt_null_value_error();

    const unsigned char* ptr = NULL;
    if (src_val->type == VALUE_BUFFER && src_val->buffer_value) {
        if (llvl_bounds_check_enabled && src_val->buffer_value->size < 4)
            runtime_error("Buffer too small", "Need at least 4 bytes for value.", "");
        ptr = src_val->buffer_value->data;
    } else if (src_val->type == VALUE_ADDRESS) {
        ptr = (const unsigned char*)src_val->address_value;
    } else {
        runtime_error("Invalid value source", "Expected buffer or address.", "");
    }

    if (!ptr)
        runtime_error("Invalid address", "Null pointer.", "");
    llvl_validate_address(ptr, 4, "Invalid address");

    uint32_t v = (uint32_t)ptr[0] |
                 ((uint32_t)ptr[1] << 8) |
                 ((uint32_t)ptr[2] << 16) |
                 ((uint32_t)ptr[3] << 24);
    *out = value_int((int)v);
}

void rt_llvl_set_value(Value* buffer_val, const Value* value_val) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid buffer write");
    if (llvl_bounds_check_enabled && buf->size < 4)
        runtime_error("Buffer too small", "Need at least 4 bytes for value.", "");
    int v = rt_int_from_value(value_val, "Invalid value");
    unsigned char* ptr = buf->data;
    llvl_validate_address(ptr, 4, "Invalid address");
    ptr[0] = (unsigned char)(v & 0xFF);
    ptr[1] = (unsigned char)((v >> 8) & 0xFF);
    ptr[2] = (unsigned char)((v >> 16) & 0xFF);
    ptr[3] = (unsigned char)((v >> 24) & 0xFF);
}

void rt_llvl_get_byte(Value* out, const Value* buffer_val, const Value* index_val) {
    if (!out)
        rt_null_value_error();
    if (!buffer_val || buffer_val->type != VALUE_BUFFER || !buffer_val->buffer_value)
        runtime_error("Invalid byte read", "Target must be a valid buffer.", "");
    size_t idx = rt_size_from_value(index_val, "Invalid byte index");
    Buffer* buf = buffer_val->buffer_value;
    if (llvl_bounds_check_enabled && idx >= buf->size)
        runtime_error("Index out of bounds", "Byte index exceeds buffer size.", "");
    llvl_validate_address(buf->data ? buf->data + idx : NULL, 1, "Invalid address");
    *out = value_int((int)buf->data[idx]);
}

void rt_llvl_set_byte(Value* buffer_val, const Value* index_val, const Value* value_val) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid byte write");
    size_t idx = rt_size_from_value(index_val, "Invalid byte index");
    if (llvl_bounds_check_enabled && idx >= buf->size)
        runtime_error("Index out of bounds", "Byte index exceeds buffer size.", "");
    int v = rt_int_from_value(value_val, "Invalid byte value");
    if (v < 0 || v > 255)
        runtime_error("Invalid byte value", "Byte value must be 0..255.", "");
    llvl_validate_address(buf->data ? buf->data + idx : NULL, 1, "Invalid address");
    buf->data[idx] = (unsigned char)v;
}

void rt_llvl_get_bit(Value* out, const Value* buffer_val, const Value* index_val) {
    if (!out)
        rt_null_value_error();
    if (!buffer_val || buffer_val->type != VALUE_BUFFER || !buffer_val->buffer_value)
        runtime_error("Invalid bit read", "Target must be a valid buffer.", "");
    size_t bit_index = rt_size_from_value(index_val, "Invalid bit index");
    Buffer* buf = buffer_val->buffer_value;
    size_t byte_index = bit_index / 8;
    if (llvl_bounds_check_enabled && byte_index >= buf->size)
        runtime_error("Index out of bounds", "Bit index exceeds buffer size.", "");
    llvl_validate_address(buf->data ? buf->data + byte_index : NULL, 1, "Invalid address");
    int bit = (buf->data[byte_index] >> (bit_index % 8)) & 1;
    *out = value_bool(bit);
}

void rt_llvl_bit_op(Value* buffer_val, const Value* index_val, int op) {
    Buffer* buf = rt_require_buffer(buffer_val, "Invalid bit operation");
    size_t bit_index = rt_size_from_value(index_val, "Invalid bit index");
    size_t byte_index = bit_index / 8;
    if (llvl_bounds_check_enabled && byte_index >= buf->size)
        runtime_error("Index out of bounds", "Bit index exceeds buffer size.", "");
    llvl_validate_address(buf->data ? buf->data + byte_index : NULL, 1, "Invalid address");
    unsigned char mask = (unsigned char)(1u << (bit_index % 8));
    if (op == 0) {
        buf->data[byte_index] |= mask;
    } else if (op == 1) {
        buf->data[byte_index] &= (unsigned char)~mask;
    } else {
        buf->data[byte_index] ^= mask;
    }
}

void rt_llvl_place_of(Value* out, const Value* buffer_val) {
    if (!out)
        rt_null_value_error();
    if (!buffer_val || buffer_val->type != VALUE_BUFFER || !buffer_val->buffer_value)
        runtime_error("Invalid place", "Target must be a valid buffer.", "");
    Buffer* buf = buffer_val->buffer_value;
    *out = value_address((void*)buf->data);
}

void rt_llvl_offset(Value* out, const Value* base_val, const Value* offset_val) {
    if (!out)
        rt_null_value_error();
    if (!base_val || !offset_val)
        rt_null_value_error();

    int offset = rt_int_from_value(offset_val, "Invalid address offset");
    const unsigned char* base_ptr = NULL;

    if (base_val->type == VALUE_BUFFER && base_val->buffer_value) {
        base_ptr = base_val->buffer_value->data;
    } else if (base_val->type == VALUE_ADDRESS) {
        base_ptr = (const unsigned char*)base_val->address_value;
    } else {
        runtime_error("Invalid address base", "Expected buffer or address.", "");
    }

    if (!base_ptr)
        runtime_error("Invalid address", "Null pointer.", "");

    base_ptr += offset;
    *out = value_address((void*)base_ptr);
}

void rt_llvl_get_at(Value* out, const Value* addr_val) {
    if (!out)
        rt_null_value_error();
    rt_llvl_get_value(out, addr_val);
}

void rt_llvl_set_at(const Value* addr_val, const Value* value_val) {
    if (!addr_val)
        rt_null_value_error();
    if (addr_val->type == VALUE_BUFFER) {
        Value tempBuf = *addr_val;
        rt_llvl_set_value(&tempBuf, value_val);
        return;
    }
    if (addr_val->type != VALUE_ADDRESS)
        runtime_error("Invalid address", "Expected address.", "");
    unsigned char* ptr = (unsigned char*)addr_val->address_value;
    if (!ptr)
        runtime_error("Invalid address", "Null pointer.", "");
    llvl_validate_address(ptr, 4, "Invalid address");
    int v = rt_int_from_value(value_val, "Invalid value");
    ptr[0] = (unsigned char)(v & 0xFF);
    ptr[1] = (unsigned char)((v >> 8) & 0xFF);
    ptr[2] = (unsigned char)((v >> 16) & 0xFF);
    ptr[3] = (unsigned char)((v >> 24) & 0xFF);
}

void rt_llvl_get_at_typed(Value* out, const Value* addr_val, int type_kind) {
    if (!out)
        rt_null_value_error();
    if (!addr_val)
        rt_null_value_error();

    const unsigned char* ptr = NULL;
    size_t available = 0;
    if (addr_val->type == VALUE_BUFFER && addr_val->buffer_value) {
        ptr = addr_val->buffer_value->data;
        available = addr_val->buffer_value->size;
    } else if (addr_val->type == VALUE_ADDRESS) {
        ptr = (const unsigned char*)addr_val->address_value;
        available = 0;
    } else {
        runtime_error("Invalid address", "Expected buffer or address.", "");
    }

    if (!ptr)
        runtime_error("Invalid address", "Null pointer.", "");

    size_t needed = 0;
    if (type_kind == BUFFER_ELEM_BYTE) {
        needed = 1;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 1 byte.", "");
        *out = value_int((int)ptr[0]);
        return;
    }

    if (type_kind == BUFFER_ELEM_INT) {
        needed = 4;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 4 bytes.", "");
        uint32_t v = (uint32_t)ptr[0] |
                     ((uint32_t)ptr[1] << 8) |
                     ((uint32_t)ptr[2] << 16) |
                     ((uint32_t)ptr[3] << 24);
        *out = value_int((int)v);
        return;
    }

    if (type_kind == BUFFER_ELEM_FLOAT) {
        needed = 8;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 8 bytes.", "");
        double d = 0.0;
        memcpy(&d, ptr, sizeof(double));
        *out = value_float(d);
        return;
    }

    runtime_error("Invalid typed read", "Unsupported type kind.", "");
}

void rt_llvl_set_at_typed(const Value* addr_val, const Value* value_val, int type_kind) {
    if (!addr_val)
        rt_null_value_error();
    if (!value_val)
        rt_null_value_error();

    unsigned char* ptr = NULL;
    size_t available = 0;
    if (addr_val->type == VALUE_BUFFER && addr_val->buffer_value) {
        ptr = addr_val->buffer_value->data;
        available = addr_val->buffer_value->size;
    } else if (addr_val->type == VALUE_ADDRESS) {
        ptr = (unsigned char*)addr_val->address_value;
        available = 0;
    } else {
        runtime_error("Invalid address", "Expected buffer or address.", "");
    }

    if (!ptr)
        runtime_error("Invalid address", "Null pointer.", "");

    size_t needed = 0;
    if (type_kind == BUFFER_ELEM_BYTE) {
        needed = 1;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 1 byte.", "");
        int v = rt_int_from_value(value_val, "Invalid byte value");
        if (v < 0 || v > 255)
            runtime_error("Invalid byte value", "Byte value must be 0..255.", "");
        ptr[0] = (unsigned char)v;
        return;
    }

    if (type_kind == BUFFER_ELEM_INT) {
        needed = 4;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 4 bytes.", "");
        int v = rt_int_from_value(value_val, "Invalid int value");
        ptr[0] = (unsigned char)(v & 0xFF);
        ptr[1] = (unsigned char)((v >> 8) & 0xFF);
        ptr[2] = (unsigned char)((v >> 16) & 0xFF);
        ptr[3] = (unsigned char)((v >> 24) & 0xFF);
        return;
    }

    if (type_kind == BUFFER_ELEM_FLOAT) {
        needed = 8;
        llvl_validate_address(ptr, needed, "Invalid address");
        if (llvl_bounds_check_enabled && available > 0 && available < needed)
            runtime_error("Buffer too small", "Need at least 8 bytes.", "");
        double d = 0.0;
        if (value_val->type == VALUE_FLOAT)
            d = value_val->float_value;
        else if (value_val->type == VALUE_INT)
            d = (double)value_val->int_value;
        else if (value_val->type == VALUE_BOOL)
            d = value_val->int_value ? 1.0 : 0.0;
        else
            runtime_error("Invalid float value", "Expected number or boolean.", "");
        memcpy(ptr, &d, sizeof(double));
        return;
    }

    runtime_error("Invalid typed write", "Unsupported type kind.", "");
}

void rt_llvl_wait_ms(const Value* ms_val) {
    size_t ms = rt_size_from_value(ms_val, "Invalid wait duration");
    if (ms == 0)
        return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

void rt_llvl_pin_write(const Value* value_val, const Value* pin_val) {
    (void)value_val;
    (void)pin_val;
    runtime_error("Unsupported operation", "Pin IO is not available on this platform.", "");
}

void rt_llvl_pin_read(Value* out, const Value* pin_val) {
    (void)pin_val;
    if (out)
        *out = value_int(0);
    runtime_error("Unsupported operation", "Pin IO is not available on this platform.", "");
}

typedef struct {
    int port;
    int value;
} LlvlPortEntry;

static LlvlPortEntry* llvl_ports = NULL;
static int llvl_port_count = 0;
static int llvl_port_capacity = 0;

static LlvlPortEntry* llvl_find_port(int port, int create) {
    for (int i = 0; i < llvl_port_count; i++) {
        if (llvl_ports[i].port == port)
            return &llvl_ports[i];
    }
    if (!create)
        return NULL;
    if (llvl_port_count >= llvl_port_capacity) {
        int new_cap = llvl_port_capacity > 0 ? llvl_port_capacity * 2 : 16;
        if (new_cap < llvl_port_capacity)
            runtime_error("Low-level ports", "Port registry overflow.", "");
        if ((size_t)new_cap > SIZE_MAX / sizeof(LlvlPortEntry))
            runtime_error("Low-level ports", "Port registry overflow.", "");
        LlvlPortEntry* grown = (LlvlPortEntry*)realloc(llvl_ports, (size_t)new_cap * sizeof(LlvlPortEntry));
        if (!grown)
            runtime_error("Out of memory", "Could not grow port registry.", "");
        memset(grown + llvl_port_capacity, 0, (size_t)(new_cap - llvl_port_capacity) * sizeof(LlvlPortEntry));
        llvl_ports = grown;
        llvl_port_capacity = new_cap;
    }
    llvl_ports[llvl_port_count].port = port;
    llvl_ports[llvl_port_count].value = 0;
    llvl_port_count++;
    return &llvl_ports[llvl_port_count - 1];
}

void rt_llvl_port_write(const Value* port_val, const Value* value_val) {
    int port = rt_int_from_value(port_val, "Invalid port");
    int value = rt_int_from_value(value_val, "Invalid port value");
    LlvlPortEntry* entry = llvl_find_port(port, 1);
    if (!entry)
        runtime_error("Low-level ports", "Unable to register port.", "");
    entry->value = value;
}

void rt_llvl_port_read(Value* out, const Value* port_val) {
    if (!out)
        rt_null_value_error();
    int port = rt_int_from_value(port_val, "Invalid port");
    LlvlPortEntry* entry = llvl_find_port(port, 0);
    int value = entry ? entry->value : 0;
    *out = value_int(value);
}

typedef struct {
    int id;
    char* handler_name;
} LlvlInterruptEntry;

static LlvlInterruptEntry* llvl_interrupts = NULL;
static int llvl_interrupt_count = 0;
static int llvl_interrupt_capacity = 0;

void rt_llvl_register_interrupt(const Value* id_val, const char* handler_name) {
    if (!handler_name || !handler_name[0])
        runtime_error("Invalid interrupt handler", "Handler name is missing.", "");
    int id = rt_int_from_value(id_val, "Invalid interrupt id");
    for (int i = 0; i < llvl_interrupt_count; i++) {
        if (llvl_interrupts[i].id == id) {
            free(llvl_interrupts[i].handler_name);
            llvl_interrupts[i].handler_name = rt_strdup_checked(handler_name);
            return;
        }
    }
    if (llvl_interrupt_count >= llvl_interrupt_capacity) {
        int new_cap = llvl_interrupt_capacity > 0 ? llvl_interrupt_capacity * 2 : 16;
        if (new_cap < llvl_interrupt_capacity)
            runtime_error("Low-level interrupts", "Interrupt registry overflow.", "");
        if ((size_t)new_cap > SIZE_MAX / sizeof(LlvlInterruptEntry))
            runtime_error("Low-level interrupts", "Interrupt registry overflow.", "");
        LlvlInterruptEntry* grown = (LlvlInterruptEntry*)realloc(llvl_interrupts, (size_t)new_cap * sizeof(LlvlInterruptEntry));
        if (!grown)
            runtime_error("Out of memory", "Could not grow interrupt registry.", "");
        memset(grown + llvl_interrupt_capacity, 0, (size_t)(new_cap - llvl_interrupt_capacity) * sizeof(LlvlInterruptEntry));
        llvl_interrupts = grown;
        llvl_interrupt_capacity = new_cap;
    }
    llvl_interrupts[llvl_interrupt_count].id = id;
    llvl_interrupts[llvl_interrupt_count].handler_name = rt_strdup_checked(handler_name);
    llvl_interrupt_count++;
}


