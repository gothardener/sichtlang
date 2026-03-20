#ifndef SOURCE_H
#define SOURCE_H

void source_load(const char* src);
const char* source_get_line(int line);
void source_set_label(const char* label);
const char* source_get_label(void);
void source_push_context(void);
void source_pop_context(void);

#endif

