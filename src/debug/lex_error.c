#include "lex_error.h"
#include "error.h"

void lex_error(
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
) {
    error_report("Lexer", line, column, title, message, hint);
}

