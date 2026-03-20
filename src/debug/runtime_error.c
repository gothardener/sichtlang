#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "runtime_error.h"
#include "source.h"
#include "error.h"

static jmp_buf* try_stack[32];
static int try_depth = 0;
static jmp_buf try_env_pool[32];
static int try_env_pool_depth = 0;
static int try_trace_cached = -1;
static int current_line = 0;
static int current_column = 0;

static int str_empty(const char* s) {
    return !s || s[0] == '\0';
}

static const char* runtime_fallback_hint(const char* title, const char* message) {
    if (!str_empty(title) && strcmp(title, "Division by zero") == 0)
        return "Ensure the right-hand side is never 0 before dividing.";
    if (!str_empty(title) && strcmp(title, "Undefined variable") == 0)
        return "Define it first with `set name to value` or fix the variable name.";
    if (!str_empty(title) && strcmp(title, "Invalid cast") == 0)
        return "Cast only compatible values (for example numeric text to int/float).";
    if (!str_empty(title) && strcmp(title, "Execution step limit exceeded") == 0)
        return "Raise SICHT_MAX_STEPS or make the loop/control flow terminate sooner.";
    if (!str_empty(title) && strcmp(title, "Library not found") == 0)
        return "Check the module path and ensure the `.si` file exists.";
    if (!str_empty(title) && strcmp(title, "Circular import") == 0)
        return "Break the cycle by moving shared code to a third module.";
    if (!str_empty(title) && strcmp(title, "Unavailable symbol") == 0)
        return "Ensure the symbol is offered by the library and imported with the correct name.";
    if (!str_empty(title) && strcmp(title, "Invalid function call") == 0)
        return "Match the function parameter list: count, names, and order.";
    if (!str_empty(title) && strcmp(title, "Invalid type construction") == 0)
        return "Provide exactly the required fields and avoid unknown/duplicate field names.";
    if (!str_empty(title) && strcmp(title, "Integer overflow") == 0)
        return "Use smaller integer values or switch to float arithmetic where appropriate.";
    if (!str_empty(message) && strstr(message, "step budget"))
        return "Raise SICHT_MAX_STEPS or make the loop/control flow terminate sooner.";
    return "Check the reported operation and adjust the code to the expected Sicht pattern.";
}

void runtime_error_set_location(int line, int column) {
    current_line = line > 0 ? line : 0;
    current_column = column > 0 ? column : 0;
}

static int runtime_try_trace_enabled(void) {
    if (try_trace_cached >= 0)
        return try_trace_cached;
    const char* env = getenv("SICHT_TRY_TRACE");
    try_trace_cached = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return try_trace_cached;
}

int runtime_try_push(jmp_buf* env) {
    if (try_depth >= 32)
        return -1;

    try_stack[try_depth++] = env;
    if (runtime_try_trace_enabled())
        fprintf(stderr, "[try] push depth=%d env=%p\n", try_depth, (void*)env);
    return 0;
}

void runtime_try_pop(void) {
    if (try_depth > 0)
        try_depth--;
    if (runtime_try_trace_enabled())
        fprintf(stderr, "[try] pop depth=%d\n", try_depth);
}

int runtime_try_begin(jmp_buf* env) {
    if (!env)
        return -1;
    if (runtime_try_push(env) < 0)
        return -1;
    return setjmp(*env);
}

jmp_buf* runtime_try_alloc(void) {
    if (try_env_pool_depth >= 32)
        return NULL;
    return &try_env_pool[try_env_pool_depth++];
}

void runtime_try_end(jmp_buf* env) {
    runtime_try_pop();
    if (env && try_env_pool_depth > 0)
        try_env_pool_depth--;
    if (runtime_try_trace_enabled())
        fprintf(stderr, "[try] end env=%p pool=%d\n", (void*)env, try_env_pool_depth);
}

void runtime_error(
    const char* title,
    const char* message,
    const char* hint
) {
    if (runtime_try_trace_enabled())
        fprintf(stderr, "[try] runtime_error depth=%d\n", try_depth);
    if (try_depth > 0)
        longjmp(*try_stack[try_depth - 1], 1);

    const char* used_title = str_empty(title) ? "Runtime failure" : title;
    const char* used_message = str_empty(message)
        ? "Execution stopped because an invalid runtime operation was detected."
        : message;
    const char* used_hint = str_empty(hint) ? runtime_fallback_hint(title, message) : hint;

    error_report(
        "Runtime",
        current_line,
        current_column,
        used_title,
        used_message,
        used_hint
    );
}

