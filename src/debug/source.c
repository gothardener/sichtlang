#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* source;
static char** lines;
static int line_count;
static int line_capacity;
static char* source_label;

typedef struct {
    const char* source;
    char** lines;
    int line_count;
    int line_capacity;
    char* source_label;
} SourceContext;

static SourceContext* context_stack = NULL;
static int context_count = 0;
static int context_capacity = 0;

static void* checked_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory in debug source loader.\n");
        exit(1);
    }
    return ptr;
}

static void* checked_realloc(void* ptr, size_t size) {
    void* out = realloc(ptr, size);
    if (!out) {
        fprintf(stderr, "Out of memory in debug source loader.\n");
        exit(1);
    }
    return out;
}

static void source_reset_lines(void) {
    if (!lines)
        return;

    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    free(lines);
    lines = NULL;
    line_count = 0;
    line_capacity = 0;
}

static void source_reset_label(void) {
    if (source_label) {
        free(source_label);
        source_label = NULL;
    }
}

static void source_push_context_internal(void) {
    if (context_count >= context_capacity) {
        int new_capacity = context_capacity > 0 ? context_capacity * 2 : 8;
        context_stack = checked_realloc(context_stack, sizeof(SourceContext) * (size_t)new_capacity);
        context_capacity = new_capacity;
    }
    context_stack[context_count++] = (SourceContext){
        .source = source,
        .lines = lines,
        .line_count = line_count,
        .line_capacity = line_capacity,
        .source_label = source_label
    };
    source = NULL;
    lines = NULL;
    line_count = 0;
    line_capacity = 0;
    source_label = NULL;
}

static int source_pop_context_internal(void) {
    if (context_count <= 0)
        return 0;
    SourceContext ctx = context_stack[--context_count];
    source = ctx.source;
    lines = ctx.lines;
    line_count = ctx.line_count;
    line_capacity = ctx.line_capacity;
    source_label = ctx.source_label;
    return 1;
}

static void source_push_line(const char* start, int len) {
    if (len > 0 && start[len - 1] == '\r')
        len--;

    if (line_count >= line_capacity) {
        int new_capacity = line_capacity > 0 ? line_capacity * 2 : 128;
        lines = checked_realloc(lines, sizeof(char*) * (size_t)new_capacity);
        line_capacity = new_capacity;
    }

    char* line = checked_malloc((size_t)len + 1);
    memcpy(line, start, (size_t)len);
    line[len] = '\0';
    lines[line_count++] = line;
}

void source_load(const char* src) {
    source = src;

    source_reset_lines();
    lines = checked_malloc(sizeof(char*) * 128);
    line_capacity = 128;
    line_count = 0;

    const char* start = src;
    const char* p = src;

    while (*p) {
        if (*p == '\n') {
            int len = (int)(p - start);
            source_push_line(start, len);
            start = p + 1;
        }
        p++;
    }

    if (start != p) {
        source_push_line(start, (int)(p - start));
    }
}

const char* source_get_line(int line) {
    if (line <= 0 || line > line_count) return "";
    return lines[line - 1];
}

void source_set_label(const char* label) {
    source_reset_label();
    if (!label || label[0] == '\0')
        return;

    size_t len = strlen(label);
    source_label = checked_malloc(len + 1);
    memcpy(source_label, label, len + 1);
}

const char* source_get_label(void) {
    return source_label ? source_label : "";
}

void source_push_context(void) {
    source_push_context_internal();
}

void source_pop_context(void) {
    if (context_count <= 0)
        return;
    source_reset_lines();
    source_reset_label();
    (void)source_pop_context_internal();
}

