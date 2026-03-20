#ifndef VALUE_H
#define VALUE_H

#include <stddef.h>

typedef enum {
    VALUE_INT,
    VALUE_FLOAT,
    VALUE_BOOL,
    VALUE_STRING,
    VALUE_LIST,
    VALUE_DICT,
    VALUE_GENERATOR,
    VALUE_BUFFER,
    VALUE_ADDRESS
} ValueType;

typedef struct Value Value;
typedef struct Generator Generator;
typedef struct Buffer Buffer;

typedef enum {
    BUFFER_ELEM_RAW,
    BUFFER_ELEM_BYTE,
    BUFFER_ELEM_INT,
    BUFFER_ELEM_FLOAT,
    BUFFER_ELEM_STRUCT,
    BUFFER_ELEM_UNION
} BufferElemKind;


typedef struct {
    Value* items;
    int count;
    int capacity;
} List;



typedef struct DictEntry DictEntry;
typedef struct Dict Dict;

struct Generator {
    List* cache;
    int index;
    int initialized;
    void* function_node;
    Value* args;
    int arg_count;
    const char* owner_library;
};

struct Buffer {
    unsigned char* data;
    size_t size;
    size_t elem_size;
    BufferElemKind elem_kind;
    const char* elem_type_name;
};



struct Value {
    ValueType type;

    int int_value;
    double float_value;
    char* string_value;

    List* list_value;
    Dict* dict_value;
    Generator* generator_value;
    Buffer* buffer_value;
    void* address_value;
};



struct DictEntry {
    char* key;
    unsigned char key_is_gc;
    Value value;
};

struct Dict {
    DictEntry* entries;
    int count;
    int capacity;
    int* hash_slots;
    int hash_capacity;
};



Value value_int(int v);
Value value_float(double v);
Value value_bool(int v);
Value value_string(const char* s);
Value value_list(List* list);
Value value_dict(Dict* dict);
Value value_generator(Generator* generator);
Value value_buffer(Buffer* buffer);
Value value_address(void* address);

#endif

