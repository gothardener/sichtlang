#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

void arena_init(void);
void* arena_alloc(size_t size);
void arena_free_all(void);

#endif
