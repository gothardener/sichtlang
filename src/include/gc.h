#ifndef GC_H
#define GC_H

#include <stddef.h>
#include "value.h"

void gc_init(void);
void gc_shutdown(void);

int gc_needs_collect(void);
void gc_collect(const Value* roots, int root_count);
void gc_set_paused(int paused);
int gc_is_paused(void);
size_t gc_live_count(void);
size_t gc_next_threshold_value(void);

char* gc_string_new(const char* s);
unsigned int gc_string_hash(const char* s);
List* gc_list_new(int initial_capacity);
Dict* gc_dict_new(int initial_capacity);
Generator* gc_generator_new(int arg_count);

void gc_list_reserve(List* list, int min_capacity);
void gc_dict_reserve(Dict* dict, int min_capacity);
int gc_dict_find_index(Dict* dict, const char* key);
void gc_dict_set(Dict* dict, const char* key, Value value);
void gc_dict_set_key(Dict* dict, const char* key, Value value, int key_is_gc);
int gc_dict_remove(Dict* dict, const char* key);
void gc_dict_clear(Dict* dict);

#endif

