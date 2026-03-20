#ifndef LEX_ERROR_H
#define LEX_ERROR_H

void lex_error(
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
);

#endif
