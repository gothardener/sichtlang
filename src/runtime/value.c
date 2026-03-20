#include "value.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>



Value value_int(int v) {
    Value val;

    val.type = VALUE_INT;
    val.int_value = v;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}



Value value_float(double v) {
    Value val;

    val.type = VALUE_FLOAT;
    val.int_value = 0;
    val.float_value = v;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}



Value value_bool(int v) {
    Value val;

    val.type = VALUE_BOOL;
    val.int_value = v ? 1 : 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}



Value value_string(const char* s) {
    Value val;

    val.type = VALUE_STRING;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = gc_string_new(s);
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}



Value value_list(List* list) {
    Value val;

    val.type = VALUE_LIST;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = list;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}



Value value_dict(Dict* dict) {
    Value val;

    val.type = VALUE_DICT;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = dict;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}

Value value_generator(Generator* generator) {
    Value val;

    val.type = VALUE_GENERATOR;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = generator;
    val.buffer_value = NULL;
    val.address_value = NULL;

    return val;
}

Value value_buffer(Buffer* buffer) {
    Value val;

    val.type = VALUE_BUFFER;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = buffer;
    val.address_value = NULL;

    return val;
}

Value value_address(void* address) {
    Value val;

    val.type = VALUE_ADDRESS;
    val.int_value = 0;
    val.float_value = 0.0;
    val.string_value = NULL;
    val.list_value = NULL;
    val.dict_value = NULL;
    val.generator_value = NULL;
    val.buffer_value = NULL;
    val.address_value = address;

    return val;
}


