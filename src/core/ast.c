#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "ast.h"
#include "arena.h"

static size_t ast_checked_mul(size_t count, size_t elem_size, const char* label) {
    if (elem_size != 0 && count > SIZE_MAX / elem_size) {
        fprintf(stderr, "AST allocation overflow: %s.\n", label);
        exit(1);
    }
    return count * elem_size;
}

static char* ast_strdup_checked(const char* s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "AST string exceeded maximum supported size.\n");
        exit(1);
    }
    char* out = arena_alloc(len + 1);
    memcpy(out, s, len + 1);
    return out;
}



ASTNode* ast_program(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_PROGRAM;
    n->llvl_mode = 0;
    n->count = 0;
    n->body_capacity = 32;
    n->body = arena_alloc(ast_checked_mul((size_t)n->body_capacity, sizeof(ASTNode*), "program body"));
    return n;
}

ASTNode* ast_block(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_BLOCK;
    n->llvl_mode = 0;
    n->count = 0;
    n->body_capacity = 32;
    n->body = arena_alloc(ast_checked_mul((size_t)n->body_capacity, sizeof(ASTNode*), "block body"));
    return n;
}

ASTNode* ast_llvl_block(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_LLVL_BLOCK;
    n->llvl_mode = 1;
    n->count = 0;
    n->body_capacity = 32;
    n->body = arena_alloc(ast_checked_mul((size_t)n->body_capacity, sizeof(ASTNode*), "llvl block body"));
    return n;
}

static void ast_ensure_body_capacity(ASTNode* block, int needed) {
    if (!block)
        return;
    if (block->body_capacity >= needed)
        return;
    int new_cap = block->body_capacity > 0 ? block->body_capacity : 32;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2) {
            fprintf(stderr, "AST block exceeded maximum supported size.\n");
            exit(1);
        }
        new_cap *= 2;
    }
    ASTNode** grown = arena_alloc(ast_checked_mul((size_t)new_cap, sizeof(ASTNode*), "block body"));
    if (block->body && block->count > 0)
        memcpy(grown, block->body, sizeof(ASTNode*) * (size_t)block->count);
    block->body = grown;
    block->body_capacity = new_cap;
}

void ast_add(ASTNode* block, ASTNode* stmt) {
    ast_ensure_body_capacity(block, block->count + 1);
    block->body[block->count++] = stmt;
}



ASTNode* ast_set(const char* name, Expr* expr) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_SET;

    n->name = ast_strdup_checked(name);

    n->expr = expr;
    return n;
}

ASTNode* ast_print_expr(Expr* expr) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_PRINT_EXPR;
    n->expr = expr;
    return n;
}

ASTNode* ast_print_string(StringPart* parts, int count) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_PRINT_STRING;
    n->parts = parts;
    n->part_count = count;
    return n;
}



ASTNode* ast_if(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_IF;
    n->branch_capacity = 4;
    n->branches = arena_alloc(ast_checked_mul((size_t)n->branch_capacity, sizeof(IfBranch), "if branches"));
    n->branch_count = 0;
    n->else_block = NULL;
    return n;
}

void ast_if_add_branch(ASTNode* if_node, Expr* condition, ASTNode* block) {
    if (if_node->branch_count >= if_node->branch_capacity) {
        int new_cap = 4;
        if (if_node->branch_capacity > 0) {
            if (if_node->branch_capacity > INT_MAX / 2) {
                fprintf(stderr, "AST if-branch list exceeded maximum supported size.\n");
                exit(1);
            }
            new_cap = if_node->branch_capacity * 2;
        }
        IfBranch* grown = arena_alloc(ast_checked_mul((size_t)new_cap, sizeof(IfBranch), "if branches"));
        if (if_node->branches && if_node->branch_count > 0)
            memcpy(grown, if_node->branches, sizeof(IfBranch) * (size_t)if_node->branch_count);
        if_node->branches = grown;
        if_node->branch_capacity = new_cap;
    }
    IfBranch* b = &if_node->branches[if_node->branch_count++];
    b->condition = condition;
    b->block = block;
}

void ast_if_set_else(ASTNode* if_node, ASTNode* else_block) {
    if_node->else_block = else_block;
}

ASTNode* ast_try(ASTNode* try_block, ASTNode* otherwise_block) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_TRY;
    n->try_stmt.try_block = try_block;
    n->try_stmt.otherwise_block = otherwise_block;
    return n;
}



ASTNode* ast_while(Expr* condition, Expr* repeat_limit, ASTNode* body) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_WHILE;

    n->while_stmt.condition = condition;
    n->while_stmt.repeat_limit = repeat_limit;
    n->while_stmt.body = body;

    return n;
}

ASTNode* ast_repeat(Expr* times, ASTNode* body) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_REPEAT;

    n->repeat_stmt.times = times;
    n->repeat_stmt.body = body;

    return n;
}

ASTNode* ast_for_each(const char* item_name, Expr* iterable, ASTNode* body) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_FOR_EACH;
    n->for_each_stmt.item_name = ast_strdup_checked(item_name);
    n->for_each_stmt.iterable = iterable;
    n->for_each_stmt.body = body;
    return n;
}

ASTNode* ast_expr_stmt(Expr* expr) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_EXPR_STMT;
    node->expr = expr;
    node->line = 0;
    node->column = 0;
    return node;
}

ASTNode* ast_function(const char* name, char** params, Expr** param_defaults, int param_count, int required_param_count, ASTNode* body) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_FUNCTION;
    node->function_decl.name = ast_strdup_checked(name);
    node->function_decl.param_count = param_count;
    node->function_decl.required_param_count = required_param_count;
    if (param_count > 0) {
        node->function_decl.params = arena_alloc(ast_checked_mul((size_t)param_count, sizeof(char*), "function params"));
        node->function_decl.param_defaults = arena_alloc(ast_checked_mul((size_t)param_count, sizeof(Expr*), "function param defaults"));
        for (int i = 0; i < param_count; i++) {
            node->function_decl.params[i] = ast_strdup_checked(params[i]);
            node->function_decl.param_defaults[i] = param_defaults ? param_defaults[i] : NULL;
        }
    } else {
        node->function_decl.params = NULL;
        node->function_decl.param_defaults = NULL;
    }
    node->function_decl.body = body;
    return node;
}

ASTNode* ast_match(Expr* target) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_MATCH;
    node->match_stmt.target = target;
    node->match_stmt.branch_capacity = 8;
    node->match_stmt.branches = arena_alloc(ast_checked_mul((size_t)node->match_stmt.branch_capacity, sizeof(MatchCaseBranch), "match branches"));
    node->match_stmt.branch_count = 0;
    node->match_stmt.otherwise_block = NULL;
    return node;
}

void ast_match_add_case(ASTNode* match_node, Expr* value, ASTNode* block) {
    if (match_node->match_stmt.branch_count >= match_node->match_stmt.branch_capacity) {
        int new_cap = 8;
        if (match_node->match_stmt.branch_capacity > 0) {
            if (match_node->match_stmt.branch_capacity > INT_MAX / 2) {
                fprintf(stderr, "AST match-branch list exceeded maximum supported size.\n");
                exit(1);
            }
            new_cap = match_node->match_stmt.branch_capacity * 2;
        }
        MatchCaseBranch* grown = arena_alloc(ast_checked_mul((size_t)new_cap, sizeof(MatchCaseBranch), "match branches"));
        if (match_node->match_stmt.branches && match_node->match_stmt.branch_count > 0)
            memcpy(grown, match_node->match_stmt.branches, sizeof(MatchCaseBranch) * (size_t)match_node->match_stmt.branch_count);
        match_node->match_stmt.branches = grown;
        match_node->match_stmt.branch_capacity = new_cap;
    }
    MatchCaseBranch* branch = &match_node->match_stmt.branches[match_node->match_stmt.branch_count++];
    branch->value = value;
    branch->block = block;
}

void ast_match_set_otherwise(ASTNode* match_node, ASTNode* otherwise_block) {
    match_node->match_stmt.otherwise_block = otherwise_block;
}

ASTNode* ast_create_library(const char* name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_CREATE_LIBRARY;
    node->library_decl.name = ast_strdup_checked(name);
    return node;
}

ASTNode* ast_load_library(const char* name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LOAD_LIBRARY;
    node->library_load.name = ast_strdup_checked(name);
    return node;
}

ASTNode* ast_load_file(Expr* path) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LOAD_FILE;
    node->file_load.path = path;
    return node;
}

ASTNode* ast_library_offer(const char* name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIBRARY_OFFER;
    node->library_offer.name = ast_strdup_checked(name);
    return node;
}

ASTNode* ast_library_take(const char* name, const char* library_name, const char* alias_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIBRARY_TAKE;
    node->library_take.name = ast_strdup_checked(name);
    node->library_take.library_name = ast_strdup_checked(library_name);
    if (alias_name) {
        node->library_take.alias_name = ast_strdup_checked(alias_name);
    } else {
        node->library_take.alias_name = NULL;
    }
    return node;
}

ASTNode* ast_library_take_all(const char* library_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIBRARY_TAKE_ALL;
    node->library_take_all.library_name = ast_strdup_checked(library_name);
    return node;
}

ASTNode* ast_type_decl(const char* name, char** fields, int field_count) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_TYPE_DECL;
    node->type_decl.name = ast_strdup_checked(name);
    node->type_decl.field_count = field_count;
    if (field_count > 0) {
        node->type_decl.fields = arena_alloc(ast_checked_mul((size_t)field_count, sizeof(char*), "type fields"));
        for (int i = 0; i < field_count; i++) {
            node->type_decl.fields[i] = ast_strdup_checked(fields[i]);
        }
    } else {
        node->type_decl.fields = NULL;
    }
    return node;
}

ASTNode* ast_file_write(Expr* path, Expr* content) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_FILE_WRITE;
    node->file_io_stmt.path = path;
    node->file_io_stmt.content = content;
    return node;
}

ASTNode* ast_file_append(Expr* path, Expr* content) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_FILE_APPEND;
    node->file_io_stmt.path = path;
    node->file_io_stmt.content = content;
    return node;
}

ASTNode* ast_return(Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_RETURN;
    node->return_stmt.value = value;
    return node;
}

ASTNode* ast_next(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_NEXT;
    return n;
}

ASTNode* ast_exit(void) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_EXIT;
    return n;
}

ASTNode* ast_set_element(char* name, Expr* index, Expr* value) {
    ASTNode* n = arena_alloc(sizeof(ASTNode));
    n->type = AST_SET_ELEMENT;

    n->set_element.name = ast_strdup_checked(name);
    n->set_element.index = index;
    n->set_element.value = value;

    return n;
}

ASTNode* ast_list_add(char* name, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIST_ADD;
    node->list_add.list_name = ast_strdup_checked(name);
    node->list_add.value = value;
    return node;
}

ASTNode* ast_list_remove(char* name, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIST_REMOVE;
    node->list_remove.list_name = ast_strdup_checked(name);
    node->list_remove.value = value;
    return node;
}

ASTNode* ast_list_remove_element(char* name, Expr* index) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIST_REMOVE_ELEMENT;

    node->list_remove_element.list_name = ast_strdup_checked(name);
    node->list_remove_element.index = index;

    return node;
}

ASTNode* ast_list_clear(char* name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LIST_CLEAR;

    node->list_clear.list_name = ast_strdup_checked(name);

    return node;
}

ASTNode* ast_dict_add(char* dict_name, Expr* key, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_DICT_ADD;
    node->dict_add.dict_name = ast_strdup_checked(dict_name);
    node->dict_add.key = key;
    node->dict_add.value = value;

    return node;
}

ASTNode* ast_dict_remove(char* dict_name, Expr* key) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_DICT_REMOVE;
    node->dict_remove.dict_name = ast_strdup_checked(dict_name);
    node->dict_remove.key = key;
    return node;
}

ASTNode* ast_dict_contains(char* dict_name, Expr* key) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_DICT_CONTAINS_ITEM;
    node->dict_contains.dict_name = ast_strdup_checked(dict_name);
    node->dict_contains.key = key;
    return node;
}

ASTNode* ast_dict_clear(char* dict_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_DICT_CLEAR;
    node->dict_clear.dict_name = ast_strdup_checked(dict_name);
    return node;
}

const char* ast_type_name(ASTType type) {
    switch (type) {
        case AST_SET: return "AST_SET";
        case AST_PRINT_EXPR: return "AST_PRINT_EXPR";
        case AST_PRINT_STRING: return "AST_PRINT_STRING";
        case AST_IF: return "AST_IF";
        case AST_TRY: return "AST_TRY";
        case AST_BLOCK: return "AST_BLOCK";
        case AST_LLVL_BLOCK: return "AST_LLVL_BLOCK";
        case AST_PROGRAM: return "AST_PROGRAM";
        case AST_WHILE: return "AST_WHILE";
        case AST_REPEAT: return "AST_REPEAT";
        case AST_FOR_EACH: return "AST_FOR_EACH";
        case AST_MATCH: return "AST_MATCH";
        case AST_EXPR_STMT: return "AST_EXPR_STMT";
        case AST_FUNCTION: return "AST_FUNCTION";
        case AST_CREATE_LIBRARY: return "AST_CREATE_LIBRARY";
        case AST_LOAD_LIBRARY: return "AST_LOAD_LIBRARY";
        case AST_LOAD_FILE: return "AST_LOAD_FILE";
        case AST_LIBRARY_OFFER: return "AST_LIBRARY_OFFER";
        case AST_LIBRARY_TAKE: return "AST_LIBRARY_TAKE";
        case AST_LIBRARY_TAKE_ALL: return "AST_LIBRARY_TAKE_ALL";
        case AST_TYPE_DECL: return "AST_TYPE_DECL";
        case AST_RETURN: return "AST_RETURN";
        case AST_FILE_WRITE: return "AST_FILE_WRITE";
        case AST_FILE_APPEND: return "AST_FILE_APPEND";
        case AST_EXIT: return "AST_EXIT";
        case AST_NEXT: return "AST_NEXT";
        case AST_SET_ELEMENT: return "AST_SET_ELEMENT";
        case AST_LIST_REMOVE_ELEMENT: return "AST_LIST_REMOVE_ELEMENT";
        case AST_LIST_ADD: return "AST_LIST_ADD";
        case AST_LIST_REMOVE: return "AST_LIST_REMOVE";
        case AST_LIST_CLEAR: return "AST_LIST_CLEAR";
        case AST_DICT_ADD: return "AST_DICT_ADD";
        case AST_DICT_REMOVE: return "AST_DICT_REMOVE";
        case AST_DICT_CLEAR: return "AST_DICT_CLEAR";
        case AST_DICT_CONTAINS_ITEM: return "AST_DICT_CONTAINS_ITEM";
        case AST_YIELD: return "AST_YIELD";
        case AST_LLVL_SAVE: return "AST_LLVL_SAVE";
        case AST_LLVL_REMOVE: return "AST_LLVL_REMOVE";
        case AST_LLVL_RESIZE: return "AST_LLVL_RESIZE";
        case AST_LLVL_COPY: return "AST_LLVL_COPY";
        case AST_LLVL_MOVE: return "AST_LLVL_MOVE";
        case AST_LLVL_SET_VALUE: return "AST_LLVL_SET_VALUE";
        case AST_LLVL_SET_BYTE: return "AST_LLVL_SET_BYTE";
        case AST_LLVL_BIT_OP: return "AST_LLVL_BIT_OP";
        case AST_LLVL_SET_AT: return "AST_LLVL_SET_AT";
        case AST_LLVL_SET_FIELD: return "AST_LLVL_SET_FIELD";
        case AST_LLVL_ATOMIC_OP: return "AST_LLVL_ATOMIC_OP";
        case AST_LLVL_MARK_VOLATILE: return "AST_LLVL_MARK_VOLATILE";
        case AST_LLVL_SET_CHECK: return "AST_LLVL_SET_CHECK";
        case AST_LLVL_PORT_WRITE: return "AST_LLVL_PORT_WRITE";
        case AST_LLVL_REGISTER_INTERRUPT: return "AST_LLVL_REGISTER_INTERRUPT";
        case AST_LLVL_PIN_WRITE: return "AST_LLVL_PIN_WRITE";
        case AST_LLVL_WAIT: return "AST_LLVL_WAIT";
        case AST_LLVL_STRUCT_DECL: return "AST_LLVL_STRUCT_DECL";
        case AST_LLVL_UNION_DECL: return "AST_LLVL_UNION_DECL";
        case AST_LLVL_ENUM_DECL: return "AST_LLVL_ENUM_DECL";
        case AST_LLVL_BITFIELD_DECL: return "AST_LLVL_BITFIELD_DECL";
    }
    return "AST_UNKNOWN";
}

static void ast_dump_indent(int indent) {
    for (int i = 0; i < indent; i++)
        printf("  ");
}

static void ast_dump_node(const ASTNode* node, int indent) {
    if (!node)
        return;

    ast_dump_indent(indent);
    printf("%s", ast_type_name(node->type));

    switch (node->type) {
        case AST_SET:
            printf(" name=%s expr=%s\n", node->name, expr_type_name(node->expr->type));
            return;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
            printf(" expr=%s\n", expr_type_name(node->expr->type));
            return;
        case AST_FUNCTION:
            printf(" name=%s params=%d required=%d\n",
                node->function_decl.name,
                node->function_decl.param_count,
                node->function_decl.required_param_count);
            ast_dump_node(node->function_decl.body, indent + 1);
            return;
        case AST_TRY:
            printf(" otherwise=%s\n", node->try_stmt.otherwise_block ? "yes" : "no");
            ast_dump_node(node->try_stmt.try_block, indent + 1);
            if (node->try_stmt.otherwise_block)
                ast_dump_node(node->try_stmt.otherwise_block, indent + 1);
            return;
        case AST_CREATE_LIBRARY:
            printf(" name=%s\n", node->library_decl.name);
            return;
        case AST_LOAD_LIBRARY:
            printf(" name=%s\n", node->library_load.name);
            return;
        case AST_LOAD_FILE:
            printf(" path=%s\n", expr_type_name(node->file_load.path->type));
            return;
        case AST_LIBRARY_OFFER:
            printf(" name=%s\n", node->library_offer.name);
            return;
        case AST_LIBRARY_TAKE:
            printf(" name=%s from=%s alias=%s\n",
                node->library_take.name,
                node->library_take.library_name,
                node->library_take.alias_name ? node->library_take.alias_name : "-");
            return;
        case AST_LIBRARY_TAKE_ALL:
            printf(" from=%s\n", node->library_take_all.library_name);
            return;
        case AST_TYPE_DECL:
            printf(" name=%s fields=%d\n",
                node->type_decl.name,
                node->type_decl.field_count);
            return;
        case AST_RETURN:
            printf(" value=%s\n", expr_type_name(node->return_stmt.value->type));
            return;
        case AST_YIELD:
            printf(" value=%s\n", node->expr ? expr_type_name(node->expr->type) : "-");
            return;
        case AST_FILE_WRITE:
            printf(" path=%s content=%s\n",
                expr_type_name(node->file_io_stmt.path->type),
                expr_type_name(node->file_io_stmt.content->type));
            return;
        case AST_FILE_APPEND:
            printf(" path=%s content=%s\n",
                expr_type_name(node->file_io_stmt.path->type),
                expr_type_name(node->file_io_stmt.content->type));
            return;
        case AST_PRINT_STRING:
            printf(" parts=%d\n", node->part_count);
            return;
        case AST_SET_ELEMENT:
            printf(" list=%s index=%s value=%s\n",
                node->set_element.name,
                expr_type_name(node->set_element.index->type),
                expr_type_name(node->set_element.value->type));
            return;
        case AST_LIST_ADD:
            printf(" list=%s value=%s\n",
                node->list_add.list_name,
                expr_type_name(node->list_add.value->type));
            return;
        case AST_LIST_REMOVE:
            printf(" list=%s value=%s\n",
                node->list_remove.list_name,
                expr_type_name(node->list_remove.value->type));
            return;
        case AST_LIST_REMOVE_ELEMENT:
            printf(" list=%s index=%s\n",
                node->list_remove_element.list_name,
                expr_type_name(node->list_remove_element.index->type));
            return;
        case AST_LIST_CLEAR:
            printf(" list=%s\n", node->list_clear.list_name);
            return;
        case AST_DICT_ADD:
            printf(" dict=%s key=%s value=%s\n",
                node->dict_add.dict_name,
                expr_type_name(node->dict_add.key->type),
                expr_type_name(node->dict_add.value->type));
            return;
        case AST_DICT_REMOVE:
            printf(" dict=%s key=%s\n",
                node->dict_remove.dict_name,
                expr_type_name(node->dict_remove.key->type));
            return;
        case AST_DICT_CLEAR:
            printf(" dict=%s\n", node->dict_clear.dict_name);
            return;
        case AST_DICT_CONTAINS_ITEM:
            printf(" dict=%s key=%s\n",
                node->dict_contains.dict_name,
                expr_type_name(node->dict_contains.key->type));
            return;
        case AST_LLVL_SET_CHECK:
            printf(" kind=%d enabled=%d\n", (int)node->llvl_set_check.kind, node->llvl_set_check.enabled);
            return;
        case AST_LLVL_PORT_WRITE:
            printf(" port=%s value=%s\n",
                expr_type_name(node->llvl_port_write.port->type),
                expr_type_name(node->llvl_port_write.value->type));
            return;
        case AST_LLVL_REGISTER_INTERRUPT:
            printf(" interrupt=%s handler=%s\n",
                expr_type_name(node->llvl_register_interrupt.interrupt_id->type),
                node->llvl_register_interrupt.handler_name ? node->llvl_register_interrupt.handler_name : "-");
            return;
        case AST_NEXT:
        case AST_EXIT:
            printf("\n");
            return;
        case AST_IF:
            printf(" branches=%d else=%s\n",
                node->branch_count,
                node->else_block ? "yes" : "no");
            for (int i = 0; i < node->branch_count; i++) {
                ast_dump_indent(indent + 1);
                printf("condition=%s\n", expr_type_name(node->branches[i].condition->type));
                ast_dump_node(node->branches[i].block, indent + 1);
            }
            if (node->else_block) {
                ast_dump_indent(indent + 1);
                printf("else:\n");
                ast_dump_node(node->else_block, indent + 1);
            }
            return;
        case AST_WHILE:
            if (node->while_stmt.repeat_limit) {
                printf(" condition=%s repeat=%s\n",
                    expr_type_name(node->while_stmt.condition->type),
                    expr_type_name(node->while_stmt.repeat_limit->type));
            } else {
                printf(" condition=%s\n", expr_type_name(node->while_stmt.condition->type));
            }
            ast_dump_node(node->while_stmt.body, indent + 1);
            return;
        case AST_REPEAT:
            printf(" times=%s\n", expr_type_name(node->repeat_stmt.times->type));
            ast_dump_node(node->repeat_stmt.body, indent + 1);
            return;
        case AST_FOR_EACH:
            printf(" item=%s iterable=%s\n",
                node->for_each_stmt.item_name,
                expr_type_name(node->for_each_stmt.iterable->type));
            ast_dump_node(node->for_each_stmt.body, indent + 1);
            return;
        case AST_MATCH:
            printf(" target=%s cases=%d otherwise=%s\n",
                expr_type_name(node->match_stmt.target->type),
                node->match_stmt.branch_count,
                node->match_stmt.otherwise_block ? "yes" : "no");
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                ast_dump_indent(indent + 1);
                printf("case=%s\n", expr_type_name(node->match_stmt.branches[i].value->type));
                ast_dump_node(node->match_stmt.branches[i].block, indent + 1);
            }
            if (node->match_stmt.otherwise_block) {
                ast_dump_indent(indent + 1);
                printf("otherwise:\n");
                ast_dump_node(node->match_stmt.otherwise_block, indent + 1);
            }
            return;
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            printf(" count=%d\n", node->count);
            for (int i = 0; i < node->count; i++)
                ast_dump_node(node->body[i], indent + 1);
            return;
        default:
            printf("\n");
            return;
    }
}

ASTNode* ast_yield(Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_YIELD;
    node->expr = value;
    return node;
}

ASTNode* ast_llvl_save(const char* name, Expr* size, int has_type, LlvlTypeKind elem_kind, const char* elem_type_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SAVE;
    node->llvl_save.name = ast_strdup_checked(name);
    node->llvl_save.size = size;
    node->llvl_save.has_type = has_type ? 1 : 0;
    node->llvl_save.elem_kind = elem_kind;
    node->llvl_save.elem_type_name = elem_type_name ? ast_strdup_checked(elem_type_name) : NULL;
    return node;
}

ASTNode* ast_llvl_remove(const char* name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_REMOVE;
    node->llvl_remove.name = ast_strdup_checked(name);
    return node;
}

ASTNode* ast_llvl_resize(const char* name, Expr* size, LlvlResizeOp op, int has_type, LlvlTypeKind elem_kind, const char* elem_type_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_RESIZE;
    node->llvl_resize.name = ast_strdup_checked(name);
    node->llvl_resize.size = size;
    node->llvl_resize.op = op;
    node->llvl_resize.has_type = has_type ? 1 : 0;
    node->llvl_resize.elem_kind = elem_kind;
    node->llvl_resize.elem_type_name = elem_type_name ? ast_strdup_checked(elem_type_name) : NULL;
    return node;
}

ASTNode* ast_llvl_copy(const char* src, const char* dest, Expr* size, int has_size) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_COPY;
    node->llvl_copy.src = ast_strdup_checked(src);
    node->llvl_copy.dest = ast_strdup_checked(dest);
    node->llvl_copy.size = size;
    node->llvl_copy.has_size = has_size;
    return node;
}

ASTNode* ast_llvl_move(const char* src, const char* dest) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_MOVE;
    node->llvl_move.src = ast_strdup_checked(src);
    node->llvl_move.dest = ast_strdup_checked(dest);
    return node;
}

ASTNode* ast_llvl_set_value(const char* name, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SET_VALUE;
    node->llvl_set_value.name = ast_strdup_checked(name);
    node->llvl_set_value.value = value;
    return node;
}

ASTNode* ast_llvl_set_byte(const char* name, Expr* index, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SET_BYTE;
    node->llvl_set_byte.name = ast_strdup_checked(name);
    node->llvl_set_byte.index = index;
    node->llvl_set_byte.value = value;
    return node;
}

ASTNode* ast_llvl_bit_op(const char* name, Expr* index, LlvlBitOp op) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_BIT_OP;
    node->llvl_bit_op.name = ast_strdup_checked(name);
    node->llvl_bit_op.index = index;
    node->llvl_bit_op.op = op;
    return node;
}

ASTNode* ast_llvl_set_at(Expr* address, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SET_AT;
    node->llvl_set_at.address = address;
    node->llvl_set_at.value = value;
    return node;
}

ASTNode* ast_llvl_set_field(const char* field_name, Expr* target, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SET_FIELD;
    node->llvl_set_field.field_name = ast_strdup_checked(field_name);
    node->llvl_set_field.target = target;
    node->llvl_set_field.value = value;
    return node;
}

ASTNode* ast_llvl_atomic_op(Expr* address, Expr* value, LlvlAtomicOp op) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_ATOMIC_OP;
    node->llvl_atomic_op.address = address;
    node->llvl_atomic_op.value = value;
    node->llvl_atomic_op.op = op;
    return node;
}

ASTNode* ast_llvl_mark_volatile(Expr* target) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_MARK_VOLATILE;
    node->llvl_mark_volatile.target = target;
    return node;
}

ASTNode* ast_llvl_set_check(LlvlCheckKind kind, int enabled) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_SET_CHECK;
    node->llvl_set_check.kind = kind;
    node->llvl_set_check.enabled = enabled ? 1 : 0;
    return node;
}

ASTNode* ast_llvl_port_write(Expr* port, Expr* value) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_PORT_WRITE;
    node->llvl_port_write.port = port;
    node->llvl_port_write.value = value;
    return node;
}

ASTNode* ast_llvl_register_interrupt(Expr* interrupt_id, const char* handler_name) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_REGISTER_INTERRUPT;
    node->llvl_register_interrupt.interrupt_id = interrupt_id;
    node->llvl_register_interrupt.handler_name = handler_name ? ast_strdup_checked(handler_name) : NULL;
    return node;
}

ASTNode* ast_llvl_pin_write(Expr* value, Expr* pin) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_PIN_WRITE;
    node->llvl_pin_write.value = value;
    node->llvl_pin_write.pin = pin;
    return node;
}

ASTNode* ast_llvl_wait(Expr* duration) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_WAIT;
    node->llvl_wait.duration = duration;
    return node;
}

ASTNode* ast_llvl_struct_decl(const char* name, LlvlField* fields, int field_count) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_STRUCT_DECL;
    node->llvl_struct_decl.name = ast_strdup_checked(name);
    node->llvl_struct_decl.field_count = field_count;
    if (field_count > 0) {
        node->llvl_struct_decl.fields = arena_alloc(ast_checked_mul((size_t)field_count, sizeof(LlvlField), "struct fields"));
        for (int i = 0; i < field_count; i++) {
            node->llvl_struct_decl.fields[i].name = ast_strdup_checked(fields[i].name);
            node->llvl_struct_decl.fields[i].kind = fields[i].kind;
            node->llvl_struct_decl.fields[i].type_name = fields[i].type_name ? ast_strdup_checked(fields[i].type_name) : NULL;
            node->llvl_struct_decl.fields[i].array_len = fields[i].array_len;
        }
    } else {
        node->llvl_struct_decl.fields = NULL;
    }
    return node;
}

ASTNode* ast_llvl_union_decl(const char* name, LlvlField* fields, int field_count) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_UNION_DECL;
    node->llvl_union_decl.name = ast_strdup_checked(name);
    node->llvl_union_decl.field_count = field_count;
    if (field_count > 0) {
        node->llvl_union_decl.fields = arena_alloc(ast_checked_mul((size_t)field_count, sizeof(LlvlField), "union fields"));
        for (int i = 0; i < field_count; i++) {
            node->llvl_union_decl.fields[i].name = ast_strdup_checked(fields[i].name);
            node->llvl_union_decl.fields[i].kind = fields[i].kind;
            node->llvl_union_decl.fields[i].type_name = fields[i].type_name ? ast_strdup_checked(fields[i].type_name) : NULL;
            node->llvl_union_decl.fields[i].array_len = fields[i].array_len;
        }
    } else {
        node->llvl_union_decl.fields = NULL;
    }
    return node;
}

ASTNode* ast_llvl_enum_decl(const char* name, char** names, int* values, int count) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_ENUM_DECL;
    node->llvl_enum_decl.name = ast_strdup_checked(name);
    node->llvl_enum_decl.count = count;
    if (count > 0) {
        node->llvl_enum_decl.names = arena_alloc(ast_checked_mul((size_t)count, sizeof(char*), "enum names"));
        node->llvl_enum_decl.values = arena_alloc(ast_checked_mul((size_t)count, sizeof(int), "enum values"));
        for (int i = 0; i < count; i++) {
            node->llvl_enum_decl.names[i] = ast_strdup_checked(names[i]);
            node->llvl_enum_decl.values[i] = values[i];
        }
    } else {
        node->llvl_enum_decl.names = NULL;
        node->llvl_enum_decl.values = NULL;
    }
    return node;
}

ASTNode* ast_llvl_bitfield_decl(const char* name, char** names, int* bits, int count) {
    ASTNode* node = arena_alloc(sizeof(ASTNode));
    node->type = AST_LLVL_BITFIELD_DECL;
    node->llvl_bitfield_decl.name = ast_strdup_checked(name);
    node->llvl_bitfield_decl.count = count;
    if (count > 0) {
        node->llvl_bitfield_decl.names = arena_alloc(ast_checked_mul((size_t)count, sizeof(char*), "bitfield names"));
        node->llvl_bitfield_decl.bits = arena_alloc(ast_checked_mul((size_t)count, sizeof(int), "bitfield bits"));
        for (int i = 0; i < count; i++) {
            node->llvl_bitfield_decl.names[i] = ast_strdup_checked(names[i]);
            node->llvl_bitfield_decl.bits[i] = bits[i];
        }
    } else {
        node->llvl_bitfield_decl.names = NULL;
        node->llvl_bitfield_decl.bits = NULL;
    }
    return node;
}

void ast_dump(const ASTNode* node) {
    ast_dump_node(node, 0);
}


