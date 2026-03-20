#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "gc.h"

typedef enum {
    GC_OBJ_STRING,
    GC_OBJ_LIST,
    GC_OBJ_DICT,
    GC_OBJ_GENERATOR
} GCObjType;

typedef struct GCObject {
    GCObjType type;
    int marked;
    struct GCObject* next;
} GCObject;

typedef struct GCString {
    GCObject obj;
    size_t len;
    unsigned int hash;
    struct GCString* intern_next;
    char chars[];
} GCString;

typedef struct {
    GCObject obj;
    List list;
} GCList;

typedef struct {
    GCObject obj;
    Dict dict;
} GCDict;

typedef struct {
    GCObject obj;
    Generator generator;
} GCGenerator;

static GCObject* gc_objects = NULL;
static size_t gc_object_count = 0;
static size_t gc_next_threshold = 1024;

#define GC_STRING_BUCKET_COUNT 4096
static GCString* gc_string_buckets[GC_STRING_BUCKET_COUNT];
static int gc_paused = 0;

#define GC_DICT_MIN_HASH_CAPACITY 8
#define GC_DICT_MAX_LOAD_PERCENT 70

static GCObject* gc_new_object(size_t size, GCObjType type) {
    if (gc_object_count == SIZE_MAX) {
        fprintf(stderr, "GC object count exceeded maximum supported size.\n");
        exit(1);
    }
    GCObject* obj = malloc(size);
    if (!obj) {
        fprintf(stderr, "Out of memory in GC allocator.\n");
        exit(1);
    }

    obj->type = type;
    obj->marked = 0;
    obj->next = gc_objects;
    gc_objects = obj;
    gc_object_count++;
    return obj;
}

unsigned int gc_string_hash(const char* s) {
    unsigned int hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        hash ^= (unsigned int)(*p);
        hash *= 16777619u;
    }
    return hash;
}

static size_t gc_string_bucket_index(unsigned int hash) {
    return (size_t)(hash % GC_STRING_BUCKET_COUNT);
}

static void gc_unintern_string(GCString* target) {
    size_t bucket = gc_string_bucket_index(target->hash);
    GCString** cursor = &gc_string_buckets[bucket];
    while (*cursor) {
        if (*cursor == target) {
            *cursor = target->intern_next;
            target->intern_next = NULL;
            return;
        }
        cursor = &(*cursor)->intern_next;
    }
}

static int gc_dict_target_hash_capacity(int item_count) {
    if (item_count < 0) {
        fprintf(stderr, "Dictionary size exceeded maximum supported size.\n");
        exit(1);
    }
    int cap = GC_DICT_MIN_HASH_CAPACITY;
    int need = item_count > 0 ? item_count : 1;
    while ((long long)need * 100 > (long long)cap * GC_DICT_MAX_LOAD_PERCENT) {
        if (cap > INT_MAX / 2) {
            fprintf(stderr, "Dictionary hash index exceeded maximum supported size.\n");
            exit(1);
        }
        cap *= 2;
    }
    return cap;
}

static void gc_dict_fill_empty_slots(Dict* dict) {
    for (int i = 0; i < dict->hash_capacity; i++)
        dict->hash_slots[i] = -1;
}

static int gc_dict_lookup_slot(const Dict* dict, const char* key) {
    if (!dict || !key || !dict->hash_slots || dict->hash_capacity <= 0)
        return -1;

    unsigned int mask = (unsigned int)(dict->hash_capacity - 1);
    unsigned int slot = gc_string_hash(key) & mask;

    for (int probe = 0; probe < dict->hash_capacity; probe++) {
        int entry_index = dict->hash_slots[slot];
        if (entry_index < 0)
            return -1;

        const char* entry_key = dict->entries[entry_index].key;
        if (entry_key == key || strcmp(entry_key, key) == 0)
            return entry_index;

        slot = (slot + 1u) & mask;
    }

    return -1;
}

static void gc_dict_insert_slot(Dict* dict, const char* key, int entry_index) {
    unsigned int mask = (unsigned int)(dict->hash_capacity - 1);
    unsigned int slot = gc_string_hash(key) & mask;

    for (int probe = 0; probe < dict->hash_capacity && dict->hash_slots[slot] >= 0; probe++)
        slot = (slot + 1u) & mask;
    if (dict->hash_slots[slot] >= 0) {
        fprintf(stderr, "Dictionary hash index insertion failed due to full table.\n");
        exit(1);
    }

    dict->hash_slots[slot] = entry_index;
}

static void gc_dict_rebuild_index(Dict* dict, int min_items) {
    if (min_items < 0) {
        fprintf(stderr, "Dictionary size exceeded maximum supported size.\n");
        exit(1);
    }
    int wanted = gc_dict_target_hash_capacity(min_items);

    if (dict->hash_capacity != wanted) {
        if ((size_t)wanted > SIZE_MAX / sizeof(int)) {
            fprintf(stderr, "Dictionary hash index exceeded maximum supported size.\n");
            exit(1);
        }
        int* new_slots = realloc(dict->hash_slots, sizeof(int) * (size_t)wanted);
        if (!new_slots) {
            fprintf(stderr, "Out of memory while growing dictionary hash index.\n");
            exit(1);
        }
        dict->hash_slots = new_slots;
        dict->hash_capacity = wanted;
    }

    gc_dict_fill_empty_slots(dict);
    for (int i = 0; i < dict->count; i++)
        gc_dict_insert_slot(dict, dict->entries[i].key, i);
}

static GCObject* gc_object_from_string(const char* s) {
    return &((GCString*)((char*)s - offsetof(GCString, chars)))->obj;
}

static GCObject* gc_object_from_list(const List* l) {
    return &((GCList*)((char*)l - offsetof(GCList, list)))->obj;
}

static GCObject* gc_object_from_dict(const Dict* d) {
    return &((GCDict*)((char*)d - offsetof(GCDict, dict)))->obj;
}

static GCObject* gc_object_from_generator(const Generator* g) {
    return &((GCGenerator*)((char*)g - offsetof(GCGenerator, generator)))->obj;
}

static void gc_mark_value(Value v);

static void gc_mark_object(GCObject* obj) {
    if (!obj || obj->marked)
        return;

    obj->marked = 1;

    if (obj->type == GC_OBJ_LIST) {
        List* l = &((GCList*)obj)->list;
        for (int i = 0; i < l->count; i++)
            gc_mark_value(l->items[i]);
        return;
    }

    if (obj->type == GC_OBJ_DICT) {
        Dict* d = &((GCDict*)obj)->dict;
        for (int i = 0; i < d->count; i++) {
            if (d->entries[i].key && d->entries[i].key_is_gc)
                gc_mark_object(gc_object_from_string(d->entries[i].key));
            gc_mark_value(d->entries[i].value);
        }
        return;
    }

    if (obj->type == GC_OBJ_GENERATOR) {
        Generator* g = &((GCGenerator*)obj)->generator;
        if (g->cache)
            gc_mark_object(gc_object_from_list(g->cache));
        for (int i = 0; i < g->arg_count; i++)
            gc_mark_value(g->args[i]);
    }
}

static void gc_mark_value(Value v) {
    switch (v.type) {
        case VALUE_STRING:
            if (v.string_value)
                gc_mark_object(gc_object_from_string(v.string_value));
            return;
        case VALUE_LIST:
            if (v.list_value)
                gc_mark_object(gc_object_from_list(v.list_value));
            return;
        case VALUE_DICT:
            if (v.dict_value)
                gc_mark_object(gc_object_from_dict(v.dict_value));
            return;
        case VALUE_GENERATOR:
            if (v.generator_value)
                gc_mark_object(gc_object_from_generator(v.generator_value));
            return;
        case VALUE_INT:
        case VALUE_FLOAT:
        case VALUE_BOOL:
        case VALUE_BUFFER:
        case VALUE_ADDRESS:
            return;
    }
}

static void gc_free_object(GCObject* obj) {
    if (obj->type == GC_OBJ_STRING) {
        gc_unintern_string((GCString*)obj);
    } else if (obj->type == GC_OBJ_LIST) {
        List* l = &((GCList*)obj)->list;
        free(l->items);
        l->items = NULL;
        l->count = 0;
        l->capacity = 0;
    } else if (obj->type == GC_OBJ_DICT) {
        Dict* d = &((GCDict*)obj)->dict;
        free(d->entries);
        free(d->hash_slots);
        d->entries = NULL;
        d->hash_slots = NULL;
        d->count = 0;
        d->capacity = 0;
        d->hash_capacity = 0;
    } else if (obj->type == GC_OBJ_GENERATOR) {
        Generator* g = &((GCGenerator*)obj)->generator;
        free(g->args);
        g->args = NULL;
        g->arg_count = 0;
        g->cache = NULL;
    }

    free(obj);
}

void gc_init(void) {
    gc_objects = NULL;
    gc_object_count = 0;
    gc_next_threshold = 1024;
    memset(gc_string_buckets, 0, sizeof(gc_string_buckets));
}

void gc_shutdown(void) {
    GCObject* obj = gc_objects;
    while (obj) {
        GCObject* next = obj->next;
        gc_free_object(obj);
        obj = next;
    }

    gc_objects = NULL;
    gc_object_count = 0;
    gc_next_threshold = 1024;
    memset(gc_string_buckets, 0, sizeof(gc_string_buckets));
}

int gc_needs_collect(void) {
    if (gc_paused)
        return 0;
    return gc_object_count >= gc_next_threshold;
}

void gc_set_paused(int paused) {
    gc_paused = paused ? 1 : 0;
}

int gc_is_paused(void) {
    return gc_paused;
}

void gc_collect(const Value* roots, int root_count) {
    for (int i = 0; i < root_count; i++)
        gc_mark_value(roots[i]);

    GCObject** current = &gc_objects;
    while (*current) {
        GCObject* obj = *current;
        if (!obj->marked) {
            *current = obj->next;
            gc_object_count--;
            gc_free_object(obj);
            continue;
        }

        obj->marked = 0;
        current = &obj->next;
    }

    if (gc_object_count > (SIZE_MAX - 1024) / 2) {
        gc_next_threshold = SIZE_MAX;
    } else {
        gc_next_threshold = gc_object_count * 2 + 1024;
    }
}

size_t gc_live_count(void) {
    return gc_object_count;
}

size_t gc_next_threshold_value(void) {
    return gc_next_threshold;
}

char* gc_string_new(const char* s) {
    if (!s)
        s = "";

    unsigned int hash = gc_string_hash(s);
    size_t bucket = gc_string_bucket_index(hash);
    for (GCString* it = gc_string_buckets[bucket]; it; it = it->intern_next) {
        if (it->hash == hash && strcmp(it->chars, s) == 0)
            return it->chars;
    }

    size_t len = strlen(s);
    if (len > SIZE_MAX - sizeof(GCString) - 1) {
        fprintf(stderr, "String exceeded maximum supported size.\n");
        exit(1);
    }
    GCString* str = (GCString*)gc_new_object(sizeof(GCString) + len + 1, GC_OBJ_STRING);
    str->len = len;
    str->hash = hash;
    str->intern_next = gc_string_buckets[bucket];
    gc_string_buckets[bucket] = str;
    memcpy(str->chars, s, len + 1);
    return str->chars;
}

List* gc_list_new(int initial_capacity) {
    if (initial_capacity < 0)
        initial_capacity = 0;
    if ((size_t)initial_capacity > SIZE_MAX / sizeof(Value)) {
        fprintf(stderr, "List exceeded maximum supported size.\n");
        exit(1);
    }

    GCList* list = (GCList*)gc_new_object(sizeof(GCList), GC_OBJ_LIST);
    list->list.count = 0;
    list->list.capacity = initial_capacity;
    list->list.items = initial_capacity > 0
        ? malloc(sizeof(Value) * (size_t)initial_capacity)
        : NULL;
    if (initial_capacity > 0 && !list->list.items) {
        fprintf(stderr, "Out of memory while allocating list storage.\n");
        exit(1);
    }
    return &list->list;
}

Dict* gc_dict_new(int initial_capacity) {
    if (initial_capacity < 0)
        initial_capacity = 0;
    if ((size_t)initial_capacity > SIZE_MAX / sizeof(DictEntry)) {
        fprintf(stderr, "Dictionary exceeded maximum supported size.\n");
        exit(1);
    }

    GCDict* dict = (GCDict*)gc_new_object(sizeof(GCDict), GC_OBJ_DICT);
    dict->dict.count = 0;
    dict->dict.capacity = initial_capacity;
    dict->dict.hash_slots = NULL;
    dict->dict.hash_capacity = 0;
    dict->dict.entries = initial_capacity > 0
        ? malloc(sizeof(DictEntry) * (size_t)initial_capacity)
        : NULL;
    if (initial_capacity > 0 && !dict->dict.entries) {
        fprintf(stderr, "Out of memory while allocating dictionary storage.\n");
        exit(1);
    }

    if (initial_capacity > 0)
        gc_dict_rebuild_index(&dict->dict, initial_capacity);

    return &dict->dict;
}

Generator* gc_generator_new(int arg_count) {
    if (arg_count < 0)
        arg_count = 0;
    if ((size_t)arg_count > SIZE_MAX / sizeof(Value)) {
        fprintf(stderr, "Generator argument list exceeded maximum supported size.\n");
        exit(1);
    }

    GCGenerator* gen = (GCGenerator*)gc_new_object(sizeof(GCGenerator), GC_OBJ_GENERATOR);
    gen->generator.cache = NULL;
    gen->generator.index = 0;
    gen->generator.initialized = 0;
    gen->generator.function_node = NULL;
    gen->generator.arg_count = arg_count;
    gen->generator.owner_library = NULL;
    gen->generator.args = arg_count > 0
        ? malloc(sizeof(Value) * (size_t)arg_count)
        : NULL;

    if (arg_count > 0 && !gen->generator.args) {
        fprintf(stderr, "Out of memory while allocating generator arguments.\n");
        exit(1);
    }
    return &gen->generator;
}

void gc_list_reserve(List* list, int min_capacity) {
    if (min_capacity < 0) {
        fprintf(stderr, "List exceeded maximum supported size.\n");
        exit(1);
    }
    if (min_capacity <= list->capacity)
        return;

    int new_capacity = list->capacity > 0 ? list->capacity : 4;
    while (new_capacity < min_capacity) {
        if (new_capacity > INT_MAX / 2) {
            fprintf(stderr, "List exceeded maximum supported size.\n");
            exit(1);
        }
        new_capacity *= 2;
    }

    if ((size_t)new_capacity > SIZE_MAX / sizeof(Value)) {
        fprintf(stderr, "List exceeded maximum supported size.\n");
        exit(1);
    }
    Value* new_items = realloc(list->items, sizeof(Value) * (size_t)new_capacity);
    if (!new_items) {
        fprintf(stderr, "Out of memory while growing list storage.\n");
        exit(1);
    }

    list->items = new_items;
    list->capacity = new_capacity;
}

void gc_dict_reserve(Dict* dict, int min_capacity) {
    if (min_capacity < 0) {
        fprintf(stderr, "Dictionary exceeded maximum supported size.\n");
        exit(1);
    }
    if (min_capacity <= dict->capacity)
        return;

    int new_capacity = dict->capacity > 0 ? dict->capacity : 4;
    while (new_capacity < min_capacity) {
        if (new_capacity > INT_MAX / 2) {
            fprintf(stderr, "Dictionary exceeded maximum supported size.\n");
            exit(1);
        }
        new_capacity *= 2;
    }

    if ((size_t)new_capacity > SIZE_MAX / sizeof(DictEntry)) {
        fprintf(stderr, "Dictionary exceeded maximum supported size.\n");
        exit(1);
    }
    DictEntry* new_entries = realloc(dict->entries, sizeof(DictEntry) * (size_t)new_capacity);
    if (!new_entries) {
        fprintf(stderr, "Out of memory while growing dictionary storage.\n");
        exit(1);
    }

    dict->entries = new_entries;
    dict->capacity = new_capacity;
}

int gc_dict_find_index(Dict* dict, const char* key) {
    if (!dict || !key || dict->count <= 0)
        return -1;

    if (!dict->hash_slots || dict->hash_capacity <= 0)
        gc_dict_rebuild_index(dict, dict->count);

    return gc_dict_lookup_slot(dict, key);
}

void gc_dict_set_key(Dict* dict, const char* key, Value value, int key_is_gc) {
    if (!dict || !key)
        return;

    int existing = gc_dict_find_index(dict, key);
    if (existing >= 0) {
        dict->entries[existing].value = value;
        return;
    }

    if (dict->count == INT_MAX) {
        fprintf(stderr, "Dictionary exceeded maximum supported size.\n");
        exit(1);
    }
    int next_count = dict->count + 1;
    gc_dict_reserve(dict, next_count);

    if (!dict->hash_slots || dict->hash_capacity <= 0 ||
        ((long long)next_count * 100) > ((long long)dict->hash_capacity * GC_DICT_MAX_LOAD_PERCENT)) {
        gc_dict_rebuild_index(dict, next_count);
    }

    dict->entries[dict->count].key = (char*)key;
    dict->entries[dict->count].key_is_gc = key_is_gc ? 1u : 0u;
    dict->entries[dict->count].value = value;
    gc_dict_insert_slot(dict, key, dict->count);
    dict->count++;
}

void gc_dict_set(Dict* dict, const char* key, Value value) {
    gc_dict_set_key(dict, key, value, 1);
}

int gc_dict_remove(Dict* dict, const char* key) {
    if (!dict || !key || dict->count <= 0)
        return 0;

    int idx = gc_dict_find_index(dict, key);
    if (idx < 0)
        return 0;

    for (int i = idx; i < dict->count - 1; i++)
        dict->entries[i] = dict->entries[i + 1];
    dict->count--;

    if (dict->count == 0) {
        gc_dict_clear(dict);
        return 1;
    }

    gc_dict_rebuild_index(dict, dict->count);
    return 1;
}

void gc_dict_clear(Dict* dict) {
    if (!dict)
        return;

    dict->count = 0;
    if (dict->hash_slots && dict->hash_capacity > 0)
        gc_dict_fill_empty_slots(dict);
}


