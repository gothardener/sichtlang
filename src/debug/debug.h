#pragma once

typedef enum {
    ERR_PARSER,
    ERR_RUNTIME,
    ERR_LEXER
} ErrorCategory;

void debug_error(
    ErrorCategory category,
    const char* title,
    const char* message,
    const char* hint
);
