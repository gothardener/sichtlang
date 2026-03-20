#ifndef RUNTIME_ERROR_H
#define RUNTIME_ERROR_H

#include <setjmp.h>

void runtime_error(
    const char* title,
    const char* message,
    const char* hint
);

void runtime_error_set_location(int line, int column);

int runtime_try_push(jmp_buf* env);
void runtime_try_pop(void);
int runtime_try_begin(jmp_buf* env);
jmp_buf* runtime_try_alloc(void);
void runtime_try_end(jmp_buf* env);

#endif

