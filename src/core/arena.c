#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include "arena.h"

typedef struct Block {
    struct Block* next;
} Block;

static Block* blocks = NULL;

void arena_init(void) {
    blocks = NULL;
}

void* arena_alloc(size_t size) {
    if (size > SIZE_MAX - sizeof(Block)) {
        fprintf(stderr, "Arena allocation exceeded maximum supported size.\n");
        exit(1);
    }
    Block* b = malloc(sizeof(Block) + size);
    if (!b) {
        fprintf(stderr, "Out of memory in arena allocator.\n");
        exit(1);
    }
    b->next = blocks;
    blocks = b;
    return (void*)(b + 1);
}

void arena_free_all(void) {
    while (blocks) {
        Block* b = blocks;
        blocks = blocks->next;
        free(b);
    }
}



