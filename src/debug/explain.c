#ifndef ERROR_H
#define ERROR_H

void error_report(
    const char* stage,   
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
);

#endif
