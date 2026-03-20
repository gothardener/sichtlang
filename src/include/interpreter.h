#ifndef INTERPRETER_H
#define INTERPRETER_H

#include <stdio.h>
#include "ast.h"
typedef struct ASTNode ASTNode;

void interpreter_set_trace(int enabled);
void interpreter_reset(void);
void execute(ASTNode* program);
void execute_incremental(ASTNode* program);
void interpreter_dump_vars(FILE* out);
void interpreter_dump_functions(FILE* out);
int interpreter_collect_symbols(const char* prefix, const char** out, int max_out);
const char* interpreter_eval_expr_type(Expr* expr);

#endif

