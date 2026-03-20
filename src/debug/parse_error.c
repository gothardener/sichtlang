#include "parse_error.h"
#include "error.h"

void parse_error(
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
) {
    error_report("Parser", line, column, title, message, hint);
}

