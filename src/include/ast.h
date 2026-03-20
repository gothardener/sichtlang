#ifndef AST_H
#define AST_H

#include "expr.h"



typedef enum {
    AST_SET,
    AST_PRINT_EXPR,
    AST_PRINT_STRING,
    AST_IF,
    AST_TRY,
    AST_BLOCK,
    AST_LLVL_BLOCK,
    AST_PROGRAM,
    AST_WHILE,
    AST_REPEAT,
    AST_FOR_EACH,
    AST_MATCH,
    AST_EXPR_STMT,
    AST_FUNCTION,
    AST_CREATE_LIBRARY,
    AST_LOAD_LIBRARY,
    AST_LOAD_FILE,
    AST_LIBRARY_OFFER,
    AST_LIBRARY_TAKE,
    AST_LIBRARY_TAKE_ALL,
    AST_TYPE_DECL,
    AST_RETURN,
    AST_FILE_WRITE,
    AST_FILE_APPEND,
    AST_EXIT,
    AST_NEXT,
    AST_SET_ELEMENT,
    AST_LIST_REMOVE_ELEMENT,
    AST_LIST_ADD,
    AST_LIST_REMOVE,
    AST_LIST_CLEAR,
    AST_DICT_ADD,
    AST_DICT_REMOVE,
    AST_DICT_CLEAR,
    AST_DICT_CONTAINS_ITEM,
    AST_YIELD,
    AST_LLVL_SAVE,
    AST_LLVL_REMOVE,
    AST_LLVL_RESIZE,
    AST_LLVL_COPY,
    AST_LLVL_MOVE,
    AST_LLVL_SET_VALUE,
    AST_LLVL_SET_BYTE,
    AST_LLVL_BIT_OP,
    AST_LLVL_SET_AT,
    AST_LLVL_SET_FIELD,
    AST_LLVL_ATOMIC_OP,
    AST_LLVL_MARK_VOLATILE,
    AST_LLVL_SET_CHECK,
    AST_LLVL_PORT_WRITE,
    AST_LLVL_REGISTER_INTERRUPT,
    AST_LLVL_PIN_WRITE,
    AST_LLVL_WAIT,
    AST_LLVL_STRUCT_DECL,
    AST_LLVL_UNION_DECL,
    AST_LLVL_ENUM_DECL,
    AST_LLVL_BITFIELD_DECL
} ASTType;

typedef enum {
    LLVL_RESIZE_GROW,
    LLVL_RESIZE_SHRINK,
    LLVL_RESIZE_ANY
} LlvlResizeOp;

typedef enum {
    LLVL_BIT_SET,
    LLVL_BIT_CLEAR,
    LLVL_BIT_FLIP
} LlvlBitOp;

typedef enum {
    LLVL_ATOMIC_WRITE,
    LLVL_ATOMIC_ADD,
    LLVL_ATOMIC_SUB
} LlvlAtomicOp;

typedef enum {
    LLVL_CHECK_BOUNDS,
    LLVL_CHECK_POINTER
} LlvlCheckKind;

typedef enum {
    LLVL_TYPE_BYTE,
    LLVL_TYPE_INT,
    LLVL_TYPE_FLOAT,
    LLVL_TYPE_STRUCT,
    LLVL_TYPE_UNION
} LlvlTypeKind;

typedef struct {
    char* name;
    LlvlTypeKind kind;
    char* type_name;
    int array_len;
} LlvlField;


typedef struct {
    char* name;
    Expr* index;
    Expr* value;
} SetElementStmt;


typedef enum {
    STR_TEXT,
    STR_EXPR
} StringPartType;

typedef struct {
    StringPartType type;
    int line;
    int column;
    char* text;
    Expr* expr;
} StringPart;



typedef struct {
    Expr* condition;
    struct ASTNode* block;
} IfBranch;



typedef struct {
    Expr* condition;
    Expr* repeat_limit;
    struct ASTNode* body;
} WhileStmt;

typedef struct {
    Expr* times;
    struct ASTNode* body;
} RepeatStmt;

typedef struct {
    char* item_name;
    Expr* iterable;
    struct ASTNode* body;
} ForEachStmt;

typedef struct {
    Expr* value;
    struct ASTNode* block;
} MatchCaseBranch;



typedef struct ASTNode {
    ASTType type;

    int line;
    int column;
    int llvl_mode;

    
    char* name;
    Expr* expr;

    
    StringPart* parts;
    int part_count;

    
    IfBranch* branches;
    int branch_count;
    int branch_capacity;
    struct ASTNode* else_block;

    
    WhileStmt while_stmt;
    
    RepeatStmt repeat_stmt;
    
    ForEachStmt for_each_stmt;
    
    struct ASTNode** body;
    int count;
    int body_capacity;
    SetElementStmt set_element;

    struct {
    char* list_name;
    Expr* value;
} list_add;
    
    struct {
    char* list_name;
    Expr* value;
} list_remove;

    struct {
    char* list_name;
    Expr* index;
} list_remove_element;

    struct {
    char* list_name;
} list_clear;

struct {
    Expr* key;
    Expr* value;
    char* dict_name;
} dict_add;

        struct {
            char* dict_name;
            Expr* key;
        } dict_remove;

        struct {
            char* dict_name;
            Expr* key;
        } dict_contains;

        struct {
            char* dict_name;
        } dict_clear;
        struct {
            char* name;
            char** params;
            Expr** param_defaults;
            int param_count;
            int required_param_count;
            struct ASTNode* body;
        } function_decl;
        struct {
            Expr* target;
            MatchCaseBranch* branches;
            int branch_count;
            int branch_capacity;
            struct ASTNode* otherwise_block;
        } match_stmt;
        struct {
            struct ASTNode* try_block;
            struct ASTNode* otherwise_block;
        } try_stmt;
        struct {
            char* name;
        } library_decl;
        struct {
            char* name;
        } library_load;
        struct {
            Expr* path;
        } file_load;
        struct {
            char* name;
        } library_offer;
        struct {
            char* name;
            char* library_name;
            char* alias_name;
        } library_take;
        struct {
            char* library_name;
        } library_take_all;
        struct {
            char* name;
            char** fields;
            int field_count;
        } type_decl;
        struct {
            Expr* value;
        } return_stmt;
        struct {
            Expr* path;
            Expr* content;
        } file_io_stmt;

        struct {
            char* name;
            Expr* size;
            int has_type;
            LlvlTypeKind elem_kind;
            char* elem_type_name;
        } llvl_save;

        struct {
            char* name;
        } llvl_remove;

        struct {
            char* name;
            Expr* size;
            LlvlResizeOp op;
            int has_type;
            LlvlTypeKind elem_kind;
            char* elem_type_name;
        } llvl_resize;

        struct {
            char* src;
            char* dest;
            Expr* size;
            int has_size;
        } llvl_copy;

        struct {
            char* src;
            char* dest;
        } llvl_move;

        struct {
            char* name;
            Expr* value;
        } llvl_set_value;

        struct {
            char* name;
            Expr* index;
            Expr* value;
        } llvl_set_byte;

        struct {
            char* name;
            Expr* index;
            LlvlBitOp op;
        } llvl_bit_op;

        struct {
            Expr* address;
            Expr* value;
        } llvl_set_at;

        struct {
            char* field_name;
            Expr* target;
            Expr* value;
        } llvl_set_field;

        struct {
            Expr* address;
            Expr* value;
            LlvlAtomicOp op;
        } llvl_atomic_op;

        struct {
            Expr* target;
        } llvl_mark_volatile;

        struct {
            LlvlCheckKind kind;
            int enabled;
        } llvl_set_check;

        struct {
            Expr* port;
            Expr* value;
        } llvl_port_write;

        struct {
            Expr* interrupt_id;
            char* handler_name;
        } llvl_register_interrupt;

        struct {
            Expr* value;
            Expr* pin;
        } llvl_pin_write;

        struct {
            Expr* duration;
        } llvl_wait;

        struct {
            char* name;
            LlvlField* fields;
            int field_count;
        } llvl_struct_decl;

        struct {
            char* name;
            LlvlField* fields;
            int field_count;
        } llvl_union_decl;

        struct {
            char* name;
            char** names;
            int* values;
            int count;
        } llvl_enum_decl;

        struct {
            char* name;
            char** names;
            int* bits;
            int count;
        } llvl_bitfield_decl;

} ASTNode;



ASTNode* ast_program(void);
ASTNode* ast_block(void);
ASTNode* ast_llvl_block(void);

ASTNode* ast_set(const char* name, Expr* expr);
ASTNode* ast_print_expr(Expr* expr);
ASTNode* ast_print_string(StringPart* parts, int count);


ASTNode* ast_if(void);
void ast_if_add_branch(ASTNode* if_node, Expr* condition, ASTNode* block);
void ast_if_set_else(ASTNode* if_node, ASTNode* else_block);
ASTNode* ast_try(ASTNode* try_block, ASTNode* otherwise_block);


ASTNode* ast_while(Expr* condition, Expr* repeat_limit, ASTNode* body);
ASTNode* ast_repeat(Expr* times, ASTNode* body);
ASTNode* ast_for_each(const char* item_name, Expr* iterable, ASTNode* body);
ASTNode* ast_expr_stmt(Expr* expr);
ASTNode* ast_function(const char* name, char** params, Expr** param_defaults, int param_count, int required_param_count, ASTNode* body);
ASTNode* ast_match(Expr* target);
void ast_match_add_case(ASTNode* match_node, Expr* value, ASTNode* block);
void ast_match_set_otherwise(ASTNode* match_node, ASTNode* otherwise_block);
ASTNode* ast_create_library(const char* name);
ASTNode* ast_load_library(const char* name);
ASTNode* ast_load_file(Expr* path);
ASTNode* ast_library_offer(const char* name);
ASTNode* ast_library_take(const char* name, const char* library_name, const char* alias_name);
ASTNode* ast_library_take_all(const char* library_name);
ASTNode* ast_type_decl(const char* name, char** fields, int field_count);
ASTNode* ast_return(Expr* value);
ASTNode* ast_file_write(Expr* path, Expr* content);
ASTNode* ast_file_append(Expr* path, Expr* content);
ASTNode* ast_next(void);
ASTNode* ast_exit(void);
ASTNode* ast_set_element(char* name, Expr* index, Expr* value);
ASTNode* ast_list_add(char* name, Expr* value);
ASTNode* ast_list_remove(char* name, Expr* value);
ASTNode* ast_list_remove_element(char* name, Expr* index);
ASTNode* ast_list_clear(char* name);
ASTNode* ast_dict_add(char* dict_name, Expr* key, Expr* value);
ASTNode* ast_dict_remove(char* dict_name, Expr* key);
ASTNode* ast_dict_clear(char* dict_name);
ASTNode* ast_dict_contains(char* dict_name, Expr* key);
ASTNode* ast_yield(Expr* value);
ASTNode* ast_llvl_save(const char* name, Expr* size, int has_type, LlvlTypeKind elem_kind, const char* elem_type_name);
ASTNode* ast_llvl_remove(const char* name);
ASTNode* ast_llvl_resize(const char* name, Expr* size, LlvlResizeOp op, int has_type, LlvlTypeKind elem_kind, const char* elem_type_name);
ASTNode* ast_llvl_copy(const char* src, const char* dest, Expr* size, int has_size);
ASTNode* ast_llvl_move(const char* src, const char* dest);
ASTNode* ast_llvl_set_value(const char* name, Expr* value);
ASTNode* ast_llvl_set_byte(const char* name, Expr* index, Expr* value);
ASTNode* ast_llvl_bit_op(const char* name, Expr* index, LlvlBitOp op);
ASTNode* ast_llvl_set_at(Expr* address, Expr* value);
ASTNode* ast_llvl_set_field(const char* field_name, Expr* target, Expr* value);
ASTNode* ast_llvl_atomic_op(Expr* address, Expr* value, LlvlAtomicOp op);
ASTNode* ast_llvl_mark_volatile(Expr* target);
ASTNode* ast_llvl_set_check(LlvlCheckKind kind, int enabled);
ASTNode* ast_llvl_port_write(Expr* port, Expr* value);
ASTNode* ast_llvl_register_interrupt(Expr* interrupt_id, const char* handler_name);
ASTNode* ast_llvl_pin_write(Expr* value, Expr* pin);
ASTNode* ast_llvl_wait(Expr* duration);
ASTNode* ast_llvl_struct_decl(const char* name, LlvlField* fields, int field_count);
ASTNode* ast_llvl_union_decl(const char* name, LlvlField* fields, int field_count);
ASTNode* ast_llvl_enum_decl(const char* name, char** names, int* values, int count);
ASTNode* ast_llvl_bitfield_decl(const char* name, char** names, int* bits, int count);
void ast_add(ASTNode* block, ASTNode* stmt);
const char* ast_type_name(ASTType type);
void ast_dump(const ASTNode* node);

#endif

