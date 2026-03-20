#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
ASTNode* parse(Token* tokens, int count);
Expr* parse_expression_snippet(const char* source, int base_line, int base_column);

#endif

