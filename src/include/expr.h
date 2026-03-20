#ifndef EXPR_H
#define EXPR_H
typedef struct Expr Expr;
typedef enum {
    EXPR_LITERAL,
    EXPR_FLOAT_LITERAL,
    EXPR_BOOL_LITERAL,
    EXPR_STRING_LITERAL,
    EXPR_VARIABLE,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_CAST,
    EXPR_INPUT,
    EXPR_BUILTIN,
    EXPR_CHAR_AT,
    EXPR_ARRAY_LITERAL,
    EXPR_ARRAY_INDEX,
    EXPR_LIST,
    EXPR_LIST_AT,
    EXPR_INDEX_OF,
    EXPR_DICT,
    EXPR_DICT_GET,
    EXPR_CALL,
    EXPR_LIST_ADD,
    EXPR_LIST_COMPREHENSION,
    EXPR_FILE_READ,
    EXPR_LLVL_VALUE_OF,
    EXPR_LLVL_BYTE_OF,
    EXPR_LLVL_BIT_OF,
    EXPR_LLVL_PLACE_OF,
    EXPR_LLVL_READ_PIN,
    EXPR_LLVL_PORT_READ,
    EXPR_LLVL_OFFSET,
    EXPR_LLVL_FIELD,
    EXPR_LLVL_ATOMIC_READ,
} ExprType;

typedef enum {
    
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_AND,
    OP_OR,
    OP_NOT,
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_SHL,
    OP_SHR,
    
    OP_NEG,


    OP_EQ,    
    OP_NE,    
    OP_GT,    
    OP_LT,    
    OP_GTE,   
    OP_LTE,   
    OP_CONTAINS   
} OpType;

typedef enum {
    CAST_TO_INTEGER,
    CAST_TO_FLOAT,
    CAST_TO_STRING,
    CAST_TO_BOOLEAN
} CastType;

typedef enum {
    BUILTIN_UPPERCASE,
    BUILTIN_LOWERCASE,
    BUILTIN_TRIM,
    BUILTIN_LENGTH,
    BUILTIN_SORT,
    BUILTIN_REVERSE,
    BUILTIN_NEXT
} BuiltinType;

typedef struct Expr {
    ExprType type;

    int line;
    int column;

    
    int int_value;
    double float_value;
    char* string_value;

    
    char* name;

    
    struct Expr* expr;

    
    struct Expr* left;
    struct Expr* right;
    OpType op;

    
    struct Expr* cast_expr;
    CastType cast_type;

    
    char* input_prompt; 
    
        
    BuiltinType builtin_type;
    struct Expr* builtin_arg;
    
    struct Expr* char_string;
    struct Expr* char_index;
        
    struct Expr** elements;
    int element_count;

    
    struct Expr* array_expr;
    struct Expr* index_expr;
    Expr** list_items;
    int list_count;
    struct Expr* list_expr;
    struct Expr* list_index;

    
    struct Expr** dict_keys;
    struct Expr** dict_values;
    int dict_count;
    Expr* dict_key;
    Expr* dict_expr;
    
    struct Expr* index_value;
    struct Expr* index_array;
    
    char* list_add_name;
    Expr* list_add_value;
    
    char* call_name;
    Expr** call_args;
    char** call_arg_names;
    int call_arg_count;
    
    char* comp_var_name;
    Expr* comp_iterable;
    Expr* comp_filter;
    Expr* comp_result;
    Expr* file_read_path;

    Expr* llvl_target;
    Expr* llvl_index;

} Expr;


Expr* expr_literal(int value);
Expr* expr_float(double value);
Expr* expr_bool(int value);
Expr* expr_string(const char* s);
Expr* expr_variable(const char* name);
Expr* expr_binary(OpType op, Expr* left, Expr* right);
Expr* expr_unary(OpType op, Expr* expr);
Expr* expr_cast(Expr* expr, CastType type);
Expr* expr_input(char* prompt);
Expr* expr_builtin(BuiltinType type, Expr* arg);
Expr* expr_char_at(Expr* string, Expr* index);
Expr* expr_array_literal(struct Expr** elements, int count);
Expr* expr_array_index(struct Expr* array, struct Expr* index);
Expr* expr_list(Expr** items, int count);
Expr* expr_list_at(Expr* list, Expr* index);
Expr* expr_dict(Expr** keys, Expr** values, int count);
Expr* expr_dict_get(Expr* key, Expr* dict);
Expr* expr_index_of(Expr* value, Expr* array);
Expr* expr_call(const char* name, Expr** args, char** arg_names, int count);
Expr* expr_list_comprehension(const char* var_name, Expr* iterable, Expr* filter, Expr* result);
Expr* expr_file_read(Expr* path_expr);
Expr* expr_llvl_value_of(Expr* target);
Expr* expr_llvl_byte_of(Expr* target, Expr* index);
Expr* expr_llvl_bit_of(Expr* target, Expr* index);
Expr* expr_llvl_place_of(Expr* target);
Expr* expr_llvl_read_pin(Expr* pin);
Expr* expr_llvl_port_read(Expr* port);
Expr* expr_llvl_offset(Expr* base, Expr* offset);
Expr* expr_llvl_field(const char* field_name, Expr* target);
Expr* expr_llvl_atomic_read(Expr* target);
void expr_set_location(Expr* expr, int line, int column);
const char* expr_type_name(ExprType type);
void expr_dump(const Expr* expr);
#endif

