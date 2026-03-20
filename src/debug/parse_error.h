#ifndef PARSE_ERROR_H
#define PARSE_ERROR_H

void parse_error(
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
);

#endif
