#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#include "parser.h"
#include "lexer.h"
#include "expr.h"
#include "ast.h"
#include "arena.h"
#include "parse_error.h"
#include "error.h"



static char* sicht_strdup(const char* s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "Parser string exceeded maximum supported size.\n");
        exit(1);
    }
    char* out = arena_alloc(len + 1);
    memcpy(out, s, len + 1);
    return out;
}

static char* sicht_strndup(const char* s, size_t n) {
    if (!s)
        s = "";
    if (n > SIZE_MAX - 1) {
        fprintf(stderr, "Parser string exceeded maximum supported size.\n");
        exit(1);
    }
    char* out = arena_alloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}



static Token* tokens;
static int current;
static int parser_error_count = 0;
static int parser_panic_mode = 0;
static const int parser_error_limit = 50;



static Token* peek(void) { return &tokens[current]; }
static Token* previous(void) { return &tokens[current - 1]; }
static int is_at_end(void) { return peek()->type == TOKEN_EOF; }

static Token* advance(void) {
    if (!is_at_end()) current++;
    return previous();
}

static int check(SichtTokenType type) {
    return !is_at_end() && peek()->type == type;
}

static int check_next(SichtTokenType type) {
    if (is_at_end())
        return 0;
    return tokens[current + 1].type == type;
}

static int match(SichtTokenType type) {
    if (check(type)) {
        advance();
        return 1;
    }
    return 0;
}

static void parser_report_error(
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
) {
    if (parser_panic_mode)
        return;
    parser_panic_mode = 1;
    parser_error_count++;
    if (parser_error_count >= parser_error_limit) {
        error_report(
            "Parser",
            line,
            column,
            "Too many errors",
            "Stopped after too many parse errors.",
            "Fix earlier errors first, then retry."
        );
        exit(1);
    }
    error_report("Parser", line, column, title, message, hint);
}

static int is_statement_start_token(SichtTokenType type) {
    switch (type) {
        case TOKEN_CREATE:
        case TOKEN_LOAD:
        case TOKEN_OFFER:
        case TOKEN_TAKE:
        case TOKEN_SET:
        case TOKEN_PRINT:
        case TOKEN_IF:
        case TOKEN_TRY:
        case TOKEN_WHILE:
        case TOKEN_REPEAT:
        case TOKEN_FOR:
        case TOKEN_MATCH:
        case TOKEN_RETURN:
        case TOKEN_YIELD:
        case TOKEN_EXIT:
        case TOKEN_NEXT:
        case TOKEN_ADD:
        case TOKEN_REMOVE:
        case TOKEN_CLEAR:
        case TOKEN_READ_KEYWORD:
        case TOKEN_WRITE_KEYWORD:
        case TOKEN_APPEND:
        case TOKEN_LLVL:
        case TOKEN_ENABLE:
        case TOKEN_DISABLE:
        case TOKEN_SAVE:
        case TOKEN_GROW:
        case TOKEN_SHRINK:
        case TOKEN_COPY:
        case TOKEN_MOVE:
        case TOKEN_RESIZE:
        case TOKEN_SEND:
        case TOKEN_WAIT:
        case TOKEN_REGISTER:
        case TOKEN_ATOMIC:
        case TOKEN_MARK:
        case TOKEN_FLIP:
        case TOKEN_STRUCT:
        case TOKEN_UNION:
        case TOKEN_ENUM:
        case TOKEN_BITFIELD:
            return 1;
        default:
            return 0;
    }
}

static int is_block_end_token(SichtTokenType type) {
    switch (type) {
        case TOKEN_END:
        case TOKEN_ENDIF:
        case TOKEN_ENDTRY:
        case TOKEN_ENDWHILE:
        case TOKEN_ENDREPEAT:
        case TOKEN_ENDFOR:
        case TOKEN_ENDMATCH:
        case TOKEN_ENDFUNCTION:
        case TOKEN_ENDTYPE:
        case TOKEN_ENDLLVL:
        case TOKEN_CASE:
        case TOKEN_OTHERWISE:
            return 1;
        default:
            return 0;
    }
}

static void synchronize(void) {
    parser_panic_mode = 0;
    if (is_at_end())
        return;

    if (is_statement_start_token(peek()->type))
        return;

    if (is_block_end_token(peek()->type)) {
        advance();
        return;
    }

    while (!is_at_end()) {
        advance();
        if (is_statement_start_token(peek()->type) || is_block_end_token(peek()->type))
            return;
    }
}

#define parse_error parser_report_error

static void consume(SichtTokenType type, const char* title, const char* hint) {
    if (check(type)) {
        advance();
        return;
    }

    parse_error(
        peek()->line,
        peek()->column,
        title,
        "Unexpected token.",
        hint
    );
}

static void* parser_realloc(void* ptr, size_t size) {
    void* out = realloc(ptr, size);
    if (!out) {
        fprintf(stderr, "Out of memory while parsing.\n");
        exit(1);
    }
    return out;
}

static void parser_reserve_expr_array(Expr*** items, int* capacity, int needed) {
    if (needed <= *capacity)
        return;

    int next = *capacity > 0 ? *capacity : 4;
    while (next < needed) {
        if (next > INT_MAX / 2) {
            fprintf(stderr, "Out of memory while parsing expression list.\n");
            exit(1);
        }
        next *= 2;
    }

    if ((size_t)next > SIZE_MAX / sizeof(Expr*)) {
        fprintf(stderr, "Out of memory while parsing expression list.\n");
        exit(1);
    }
    *items = parser_realloc(*items, sizeof(Expr*) * (size_t)next);
    *capacity = next;
}

static void parser_reserve_dict_arrays(Expr*** keys, Expr*** values, int* capacity, int needed) {
    if (needed <= *capacity)
        return;

    int next = *capacity > 0 ? *capacity : 4;
    while (next < needed) {
        if (next > INT_MAX / 2) {
            fprintf(stderr, "Out of memory while parsing dictionary literal.\n");
            exit(1);
        }
        next *= 2;
    }

    if ((size_t)next > SIZE_MAX / sizeof(Expr*)) {
        fprintf(stderr, "Out of memory while parsing dictionary literal.\n");
        exit(1);
    }
    *keys = parser_realloc(*keys, sizeof(Expr*) * (size_t)next);
    *values = parser_realloc(*values, sizeof(Expr*) * (size_t)next);
    *capacity = next;
}


static Expr* expr_with_loc(Expr* expr, int line, int column) {
    expr_set_location(expr, line, column);
    return expr;
}

static int parse_int_literal(const Token* tok) {
    errno = 0;
    char* end = NULL;
    long v = strtol(tok->value, &end, 10);
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX || !end || *end != '\0') {
        parse_error(
            tok->line,
            tok->column,
            "Invalid integer literal",
            "Integer literal is out of range.",
            "Use a smaller integer or cast from float."
        );
        return 0;
    }
    return (int)v;
}

static double parse_float_literal(const Token* tok) {
    errno = 0;
    char* end = NULL;
    double v = strtod(tok->value, &end);
    if (errno == ERANGE || !end || *end != '\0') {
        parse_error(
            tok->line,
            tok->column,
            "Invalid float literal",
            "Float literal is out of range.",
            "Use a smaller float value."
        );
        return 0.0;
    }
    return v;
}


static Expr* expression(void);
static Expr* logical_or(void);
static Expr* logical_and(void);
static Expr* comparison(void);
static Expr* term(void);
static Expr* factor(void);
static Expr* unary(void);
static Expr* primary(void);
static Expr* expression_no_cast(void);

static int parser_allow_casts = 1;



static Expr* primary(void) {

    Expr* expr = NULL;

    if (match(TOKEN_INTEGER)) {
        Token* tok = previous();
        expr = expr_with_loc(expr_literal(parse_int_literal(tok)), tok->line, tok->column);
    }

    else if (match(TOKEN_FLOAT)) {
        Token* tok = previous();
        expr = expr_with_loc(expr_float(parse_float_literal(tok)), tok->line, tok->column);
    }

    else if (match(TOKEN_BOOLEAN)) {
        Token* tok = previous();
        expr = expr_with_loc(expr_bool(tok->value[0] == 't'), tok->line, tok->column);
    }

else if (match(TOKEN_STRING)) {
    Token* tok = previous();
    expr = expr_with_loc(expr_string(tok->value), tok->line, tok->column);
}


else if (match(TOKEN_INPUT)) {
    Token* tok = previous();

    char* prompt = NULL;

    if (match(TOKEN_STRING)) {
        prompt = previous()->value;
    }

    expr = expr_with_loc(expr_input(prompt), tok->line, tok->column);
}

else if (match(TOKEN_FIELD)) {
    Token* tok = previous();

    consume(TOKEN_IDENTIFIER,
        "Expected field name",
        "Example: field id of(item)"
    );
    char* field_name = previous()->value;

    consume(TOKEN_OF,
        "Expected 'of'",
        "Example: field id of(item)"
    );

    consume(TOKEN_LPAREN,
        "Expected '('",
        "Example: field id of(item)"
    );

    Expr* target = expression();

    consume(TOKEN_RPAREN,
        "Expected ')'",
        "Close field access with ')'"
    );

    expr = expr_with_loc(expr_llvl_field(field_name, target), tok->line, tok->column);
}

else if (match(TOKEN_ADD) || match(TOKEN_SUBTRACT)) {
    Token* tok = previous();
    int is_add = tok->type == TOKEN_ADD;

    Expr* offset = expression();

    consume(is_add ? TOKEN_TO : TOKEN_FROM,
        is_add ? "Expected 'to'" : "Expected 'from'",
        is_add ? "Example: add 4 to(location)" : "Example: subtract 4 from(location)"
    );

    consume(TOKEN_LPAREN,
        "Expected '('",
        is_add ? "Example: add 4 to(location)" : "Example: subtract 4 from(location)"
    );

    Expr* base = expression();

    consume(TOKEN_RPAREN,
        "Expected ')'",
        "Close offset target with ')'"
    );

    if (!is_add)
        offset = expr_with_loc(expr_unary(OP_NEG, offset), tok->line, tok->column);

    expr = expr_with_loc(expr_llvl_offset(base, offset), tok->line, tok->column);
}

else if (match(TOKEN_SHIFT)) {
    Token* tok = previous();
    int is_left = 0;
    if (match(TOKEN_LEFT))
        is_left = 1;
    else if (match(TOKEN_RIGHT))
        is_left = 0;
    else {
        parse_error(
            peek()->line,
            peek()->column,
            "Expected shift direction",
            "Use 'left' or 'right' after shift.",
            "Example: shift left value by 2"
        );
        return NULL;
    }

    Expr* value = expression();
    consume(TOKEN_BY, "Expected 'by'", "Example: shift left value by 2");
    Expr* amount = expression();

    expr = expr_with_loc(expr_binary(is_left ? OP_SHL : OP_SHR, value, amount), tok->line, tok->column);
}

else if (match(TOKEN_CHARACTER)) {
    Token* tok = previous();

    Expr* index = expression();

    consume(TOKEN_OF,
        "Expected 'of'",
        "Example: character 2 of(name)"
    );

    consume(TOKEN_LPAREN,
        "Expected '('",
        "Example: character 2 of(name)"
    );

    Expr* str = expression();

    consume(TOKEN_RPAREN,
        "Expected ')'",
        "Close character access with ')'"
    );

    expr = expr_with_loc(expr_char_at(str, index), tok->line, tok->column);
}

else if (match(TOKEN_VALUE)) {
    Token* tok = previous();
    if (match(TOKEN_OF)) {
        consume(TOKEN_LPAREN, "Expected '('", "Example: value of(buffer)");
        Expr* target = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close value source with ')'");
        expr = expr_with_loc(expr_llvl_value_of(target), tok->line, tok->column);
    } else if (match(TOKEN_AT)) {
        consume(TOKEN_LPAREN, "Expected '('", "Example: value at(location)");
        Expr* target = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close address with ')'");
        expr = expr_with_loc(expr_llvl_value_of(target), tok->line, tok->column);
    } else {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid value expression",
            "Expected 'of' or 'at' after value.",
            "Examples: value of(buffer), value at(location)"
        );
        return NULL;
    }
}

else if (match(TOKEN_ATOMIC)) {
    Token* tok = previous();
    if (!match(TOKEN_READ_KEYWORD)) {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid atomic expression",
            "Expected 'read' after atomic.",
            "Example: atomic read from(location)"
        );
        return NULL;
    }
    consume(TOKEN_FROM, "Expected 'from'", "Example: atomic read from(location)");
    consume(TOKEN_LPAREN, "Expected '('", "Example: atomic read from(location)");
    Expr* target = expression();
    consume(TOKEN_RPAREN, "Expected ')'", "Close atomic read with ')'");
    expr = expr_with_loc(expr_llvl_atomic_read(target), tok->line, tok->column);
}

else if (match(TOKEN_BYTE)) {
    Token* tok = previous();
    Expr* index = expression();
    consume(TOKEN_OF, "Expected 'of'", "Example: byte 0 of(buffer)");
    consume(TOKEN_LPAREN, "Expected '('", "Example: byte 0 of(buffer)");
    Expr* target = expression();
    consume(TOKEN_RPAREN, "Expected ')'", "Close byte source with ')'");
    expr = expr_with_loc(expr_llvl_byte_of(target, index), tok->line, tok->column);
}

else if (match(TOKEN_BIT)) {
    Token* tok = previous();

    if (match(TOKEN_AND) || match(TOKEN_OR) || match(TOKEN_XOR)) {
        SichtTokenType op_tok = previous()->type;
        if (!match(TOKEN_OF) && !check(TOKEN_LPAREN)) {
            parse_error(
                peek()->line,
                peek()->column,
                "Expected 'of' or '('",
                "Bitwise calls require of(...) or (...).",
                "Example: bit and of(a, b)"
            );
            return NULL;
        }
        consume(TOKEN_LPAREN, "Expected '('", "Example: bit and of(a, b)");
        Expr* left = expression();
        consume(TOKEN_COMMA, "Expected ','", "Example: bit and of(a, b)");
        Expr* right = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close bitwise call with ')'");

        OpType op = OP_BIT_AND;
        if (op_tok == TOKEN_OR) op = OP_BIT_OR;
        else if (op_tok == TOKEN_XOR) op = OP_BIT_XOR;

        expr = expr_with_loc(expr_binary(op, left, right), tok->line, tok->column);
    }
    else if (match(TOKEN_NOT)) {
        if (!match(TOKEN_OF) && !check(TOKEN_LPAREN)) {
            parse_error(
                peek()->line,
                peek()->column,
                "Expected 'of' or '('",
                "Bitwise not requires of(...) or (...).",
                "Example: bit not of(a)"
            );
            return NULL;
        }
        consume(TOKEN_LPAREN, "Expected '('", "Example: bit not of(a)");
        Expr* value = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close bitwise call with ')'");
        expr = expr_with_loc(expr_unary(OP_BIT_NOT, value), tok->line, tok->column);
    }
    else {
        Expr* index = expression();
        consume(TOKEN_OF, "Expected 'of'", "Example: bit 3 of(buffer)");
        consume(TOKEN_LPAREN, "Expected '('", "Example: bit 3 of(buffer)");
        Expr* target = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close bit source with ')'");
        expr = expr_with_loc(expr_llvl_bit_of(target, index), tok->line, tok->column);
    }
}

else if (match(TOKEN_PLACE)) {
    Token* tok = previous();
    consume(TOKEN_OF, "Expected 'of'", "Example: place of(buffer)");
    consume(TOKEN_LPAREN, "Expected '('", "Example: place of(buffer)");
    Expr* target = expression();
    consume(TOKEN_RPAREN, "Expected ')'", "Close place source with ')'");
    expr = expr_with_loc(expr_llvl_place_of(target), tok->line, tok->column);
}

else if (match(TOKEN_ELEMENT)) {
    Token* tok = previous();

    Expr* index = expression();

    consume(TOKEN_OF, "Expected 'of' after index.", NULL);
    consume(TOKEN_LPAREN, "Expected '(' after 'of'.", NULL);

    Expr* list = expression();

    consume(TOKEN_RPAREN, "Expected ')'.", NULL);

    expr = expr_with_loc(expr_list_at(list, index), tok->line, tok->column);
}

else if (match(TOKEN_INDEX)) {
    Token* tok = previous();

    consume(TOKEN_OF, "Expected 'of'", NULL);
    consume(TOKEN_LPAREN, "Expected '('", NULL);

    Expr* value = expression();

    consume(TOKEN_RPAREN, "Expected ')'", NULL);

    consume(TOKEN_IN, "Expected 'in'", NULL);
    consume(TOKEN_LPAREN, "Expected '('", NULL);

    Expr* list = expression();

    consume(TOKEN_RPAREN, "Expected ')'", NULL);

    return expr_with_loc(expr_index_of(value, list), tok->line, tok->column);
}

else if (match(TOKEN_NEXT)) {
    Token* tok = previous();
    consume(TOKEN_FROM, "Expected 'from'", "Use: next from(generator)");
    consume(TOKEN_LPAREN, "Expected '('", "Use: next from(generator)");
    Expr* arg = expression();
    consume(TOKEN_RPAREN, "Expected ')'", "Close with ')'");
    expr = expr_with_loc(expr_builtin(BUILTIN_NEXT, arg), tok->line, tok->column);
}

else if (
    match(TOKEN_UPPERCASE) ||
    match(TOKEN_LOWERCASE) ||
    match(TOKEN_TRIM) ||
    match(TOKEN_LENGTH)
) {
    Token* tok = previous();
    SichtTokenType t = tok->type;

    if (!match(TOKEN_OF) && !check(TOKEN_LPAREN)) {
        parse_error(
            peek()->line,
            peek()->column,
            "Expected 'of' or '('",
            "Builtin call is missing `of` or opening parenthesis.",
            "Use: length of(values) or length(values)"
        );
        return NULL;
    }

    consume(TOKEN_LPAREN,
        "Expected '('",
        "Example: uppercase(name) or uppercase of(name)"
    );

    Expr* arg = expression();

    consume(TOKEN_RPAREN,
        "Expected ')'",
        "Close builtin call with ')'"
    );

    BuiltinType bt;

    if (t == TOKEN_UPPERCASE) bt = BUILTIN_UPPERCASE;
    else if (t == TOKEN_LOWERCASE) bt = BUILTIN_LOWERCASE;
    else if (t == TOKEN_TRIM) bt = BUILTIN_TRIM;
    else bt = BUILTIN_LENGTH;

    expr = expr_with_loc(expr_builtin(bt, arg), tok->line, tok->column);
}






else if (match(TOKEN_SORT)) {
    Token* tok = previous();

    consume(TOKEN_LPAREN, "Expected '(' after sort", "");

    Expr* arg = expression();

    consume(TOKEN_RPAREN, "Expected ')'", "");

    return expr_with_loc(expr_builtin(BUILTIN_SORT, arg), tok->line, tok->column);
}

else if (match(TOKEN_REVERSE)) {
    Token* tok = previous();

    consume(TOKEN_LPAREN, "Expected '(' after reverse", "");

    Expr* arg = expression();

    consume(TOKEN_RPAREN, "Expected ')'", "");

    return expr_with_loc(expr_builtin(BUILTIN_REVERSE, arg), tok->line, tok->column);
}


else if (match(TOKEN_LBRACKET)) {
    Token* tok = previous();
    if (match(TOKEN_EACH)) {
        consume(TOKEN_IDENTIFIER, "Expected item name", "Example: [each item in(values): item]");
        char* item_name = previous()->value;

        consume(TOKEN_IN, "Expected 'in'", "Example: [each item in(values): item]");
        consume(TOKEN_LPAREN, "Expected '('", "Example: [each item in(values): item]");
        Expr* iterable = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close iterable with ')'");

        Expr* filter = NULL;
        if (match(TOKEN_IF))
            filter = expression();

        consume(TOKEN_COLON, "Expected ':'", "Example: [each item in(values): item]");
        Expr* result = expression();
        consume(TOKEN_RBRACKET, "Expected ']'.", "Close list comprehension with ']'");

        return expr_with_loc(expr_list_comprehension(item_name, iterable, filter, result), tok->line, tok->column);
    }

    Expr** elements = NULL;
    int count = 0;
    int capacity = 0;

    if (!check(TOKEN_RBRACKET)) {
        do {
            Expr* value = expression();

            parser_reserve_expr_array(&elements, &capacity, count + 1);
            elements[count++] = value;

        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACKET, "Expected ']'.", "Close the array literal.");

    return expr_with_loc(expr_list(elements, count), tok->line, tok->column);
}



else if (match(TOKEN_LBRACE)) {
    Token* tok = previous();

    Expr** keys = NULL;
    Expr** values = NULL;
    int count = 0;
    int capacity = 0;

    if (!check(TOKEN_RBRACE)) {
        do {

            Expr* key = expression();

            consume(TOKEN_COLON,
                "Expected ':'",
                "Dictionary syntax: { key : value }"
            );

            Expr* value = expression();

            parser_reserve_dict_arrays(&keys, &values, &capacity, count + 1);

            keys[count] = key;
            values[count] = value;
            count++;

        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE,
        "Expected '}'",
        "Close dictionary literal."
    );

    return expr_with_loc(expr_dict(keys, values, count), tok->line, tok->column);
}

else if (match(TOKEN_DICTIONARY)) {
    Token* tok = previous();

    consume(TOKEN_LBRACE, "Expected '{'", "Example: dictionary { \"Anna\": 123 }");

    Expr** keys = NULL;
    Expr** values = NULL;
    int count = 0;
    int capacity = 0;

    if (!check(TOKEN_RBRACE)) {
        do {
            Expr* key = expression();

            consume(TOKEN_COLON, "Expected ':'", "Example: \"Anna\": 123");

            Expr* value = expression();

            parser_reserve_dict_arrays(&keys, &values, &capacity, count + 1);

            keys[count] = key;
            values[count] = value;

            count++;

        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expected '}'", "Close dictionary with '}'");

    return expr_with_loc(expr_dict(keys, values, count), tok->line, tok->column);
}

else if (match(TOKEN_GET)) {
    Token* tok = previous();

    consume(TOKEN_ITEM, "Expected 'item'", "Example: get item \"Anna\" from(phonebook)");

    Expr* key = expression();

    consume(TOKEN_FROM, "Expected 'from'", NULL);

    consume(TOKEN_LPAREN, "Expected '('", NULL);

    Expr* dict = expression();

    consume(TOKEN_RPAREN, "Expected ')'", NULL);

    return expr_with_loc(expr_dict_get(key, dict), tok->line, tok->column);
}

else if (match(TOKEN_READ_KEYWORD)) {
    Token* tok = previous();
    if (match(TOKEN_PIN)) {
        Expr* pin = expression();
        return expr_with_loc(expr_llvl_read_pin(pin), tok->line, tok->column);
    }
    if (match(TOKEN_PORT)) {
        Expr* port = expression();
        return expr_with_loc(expr_llvl_port_read(port), tok->line, tok->column);
    }
    consume(TOKEN_FILE, "Expected 'file'", "Use: read file \"notes.txt\"");

    Expr* path = NULL;
    if (match(TOKEN_LPAREN)) {
        path = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close file path with ')'");
    } else {
        path = unary();
    }

    return expr_with_loc(expr_file_read(path), tok->line, tok->column);
}

else if (match(TOKEN_IDENTIFIER)) {
    Token* tok = previous();
    char* name = tok->value;

    if (match(TOKEN_LPAREN)) {
        Expr** args = arena_alloc(sizeof(Expr*) * 64);
        char** arg_names = arena_alloc(sizeof(char*) * 64);
        int arg_count = 0;
        int seen_named = 0;

        if (!check(TOKEN_RPAREN)) {
            do {
                if (arg_count >= 64) {
                    parse_error(
                        peek()->line,
                        peek()->column,
                        "Too many arguments",
                        "Function calls support at most 64 arguments.",
                        "Reduce argument count."
                    );
                    return NULL;
                }

                char* arg_name = NULL;
                if (check(TOKEN_IDENTIFIER) && check_next(TOKEN_COLON)) {
                    advance();
                    arg_name = previous()->value;
                    consume(TOKEN_COLON, "Expected ':'", "Use named arguments like: greet(name: \"Ari\")");
                    seen_named = 1;
                } else if (seen_named) {
                    parse_error(
                        peek()->line,
                        peek()->column,
                        "Invalid argument order",
                        "Positional arguments cannot come after named arguments.",
                        "Move positional arguments before named ones."
                    );
                    return NULL;
                }

                arg_names[arg_count] = arg_name;
                args[arg_count++] = expression();
            } while (match(TOKEN_COMMA));
        }

        consume(TOKEN_RPAREN, "Expected ')'", "Close function call with ')'");
        expr = expr_with_loc(expr_call(name, args, arg_names, arg_count), tok->line, tok->column);
    } else {
        expr = expr_with_loc(expr_variable(name), tok->line, tok->column);
    }
}

    else if (match(TOKEN_LPAREN)) {
        Token* tok = previous();
        expr = expr_with_loc(expression(), tok->line, tok->column);
        consume(TOKEN_RPAREN, "Missing ')'", "Example: (x + 2)");
    }

    else {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid expression",
            "I couldn't understand this expression.",
            "Examples: 5, x, true, (x + 2)"
        );
        return NULL;
    }

    while (match(TOKEN_LBRACKET)) {
        Token* tok = previous();
        Expr* index = expression();
        consume(TOKEN_RBRACKET, "Expected ']'", "Close index access with ']'");
        expr = expr_with_loc(expr_array_index(expr, index), tok->line, tok->column);
    }

    if (parser_allow_casts) {
        while (match(TOKEN_AS)) {
            Token* tok = previous();

            if (match(TOKEN_TYPE_INT)) {
                expr = expr_with_loc(expr_cast(expr, CAST_TO_INTEGER), tok->line, tok->column);
            }
            else if (match(TOKEN_TYPE_FLOAT)) {
                expr = expr_with_loc(expr_cast(expr, CAST_TO_FLOAT), tok->line, tok->column);
            }
            else if (match(TOKEN_TYPE_BOOL)) {
                expr = expr_with_loc(expr_cast(expr, CAST_TO_BOOLEAN), tok->line, tok->column);
            }
            else if (match(TOKEN_TYPE_STRING)) {
                expr = expr_with_loc(expr_cast(expr, CAST_TO_STRING), tok->line, tok->column);
            }
            else {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Invalid cast",
                    "Expected a type after 'as'.",
                    "Example: 4 as float"
                );
                return NULL;
            }
        }
    }

    return expr;
}



static Expr* unary(void) {
    if (match(TOKEN_CAST)) {
        Token* tok = previous();
        Expr* value = unary();

    consume(TOKEN_TO,
        "Expected 'to'",
        "Use: cast <expression> to <type>"
    );

    if (match(TOKEN_TYPE_INT))
        return expr_with_loc(expr_cast(value, CAST_TO_INTEGER), tok->line, tok->column);

    if (match(TOKEN_TYPE_FLOAT))
        return expr_with_loc(expr_cast(value, CAST_TO_FLOAT), tok->line, tok->column);

    if (match(TOKEN_TYPE_BOOL))
        return expr_with_loc(expr_cast(value, CAST_TO_BOOLEAN), tok->line, tok->column);

    if (match(TOKEN_TYPE_STRING))
        return expr_with_loc(expr_cast(value, CAST_TO_STRING), tok->line, tok->column);

    parse_error(
        peek()->line,
        peek()->column,
        "Expected type",
        "Invalid cast target.",
        "Valid types: integer/int, float, bool, string"
    );
}


    
    if (match(TOKEN_MINUS)) {
        Token* tok = previous();
        return expr_with_loc(expr_unary(OP_NEG, unary()), tok->line, tok->column);
    }

    return primary();
}



static Expr* factor(void) {
    Expr* e = unary();

    while (match(TOKEN_STAR) || match(TOKEN_SLASH)) {
        Token* tok = previous();
        SichtTokenType t = tok->type;
        e = expr_with_loc(expr_binary(
            t == TOKEN_STAR ? OP_MUL : OP_DIV,
            e,
            unary()
        ), tok->line, tok->column);
    }
    return e;
}



static Expr* term(void) {
    Expr* e = factor();

    while (match(TOKEN_PLUS) || match(TOKEN_MINUS)) {
        Token* tok = previous();
        SichtTokenType t = tok->type;
        e = expr_with_loc(expr_binary(
            t == TOKEN_PLUS ? OP_ADD : OP_SUB,
            e,
            factor()
        ), tok->line, tok->column);
    }
    return e;
}



static Expr* comparison(void) {
    Expr* left = term();

    
    if (match(TOKEN_CONTAINS)) {
        Token* tok = previous();
        (void)match(TOKEN_ITEM);
        Expr* value = term();
        return expr_with_loc(expr_binary(OP_CONTAINS, left, value), tok->line, tok->column);
    }

    
    if (match(TOKEN_NOT)) {
        Token* tok = previous();
        consume(TOKEN_IN, "Expected 'in'", "Use: value not in(collection)");

        Expr* container = NULL;
        if (match(TOKEN_LPAREN)) {
            container = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close membership check with ')'");
        } else {
            container = term();
        }

        Expr* contains_expr = expr_binary(OP_CONTAINS, container, left);
        return expr_with_loc(expr_unary(OP_NOT, contains_expr), tok->line, tok->column);
    }

    if (match(TOKEN_IN)) {
        Token* tok = previous();
        Expr* container = NULL;
        if (match(TOKEN_LPAREN)) {
            container = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close membership check with ')'");
        } else {
            container = term();
        }

        return expr_with_loc(expr_binary(OP_CONTAINS, container, left), tok->line, tok->column);
    }

    if (!match(TOKEN_IS))
        return left;

    Token* is_tok = previous();
    
    if (match(TOKEN_AND)) {
        Token* tok = previous();
        consume(TOKEN_EQUAL, "Expected 'equal'", "Use: x is and equal to 5");
        consume(TOKEN_TO, "Expected 'to'", "Use: x is and equal to 5");
        return expr_with_loc(expr_binary(OP_EQ, left, term()), tok->line, tok->column);
    }

    
    if (match(TOKEN_NOT)) {
        Token* tok = previous();

        
        if (match(TOKEN_EQUAL)) {
            consume(TOKEN_TO,
                "Expected 'to'",
                "Use: x is not equal to 5"
            );
        }

        
        return expr_with_loc(expr_binary(OP_NE, left, term()), tok->line, tok->column);
    }

    
    if (match(TOKEN_GREATER)) {
        Token* tok = previous();
        int saw_than = match(TOKEN_THAN);
        if (match(TOKEN_OR)) {
            consume(TOKEN_EQUAL, "Expected 'equal'", "x is greater than or equal to 5");
            consume(TOKEN_TO, "Expected 'to'", "x is greater than or equal to 5");
            return expr_with_loc(expr_binary(OP_GTE, left, term()), tok->line, tok->column);
        }
        if (!saw_than)
            consume(TOKEN_THAN, "Expected 'than'", "x is greater than 5");
        return expr_with_loc(expr_binary(OP_GT, left, term()), tok->line, tok->column);
    }

    
    if (match(TOKEN_LESS)) {
        Token* tok = previous();
        int saw_than = match(TOKEN_THAN);
        if (match(TOKEN_OR)) {
            consume(TOKEN_EQUAL, "Expected 'equal'", "x is less than or equal to 5");
            consume(TOKEN_TO, "Expected 'to'", "x is less than or equal to 5");
            return expr_with_loc(expr_binary(OP_LTE, left, term()), tok->line, tok->column);
        }
        if (!saw_than)
            consume(TOKEN_THAN, "Expected 'than'", "x is less than 5");
        return expr_with_loc(expr_binary(OP_LT, left, term()), tok->line, tok->column);
    }

    
    if (match(TOKEN_EQUAL)) {
        Token* tok = previous();
        consume(TOKEN_TO, "Expected 'to'", "x is equal to 5");
        return expr_with_loc(expr_binary(OP_EQ, left, term()), tok->line, tok->column);
    }

    
    return expr_with_loc(expr_binary(OP_EQ, left, term()), is_tok->line, is_tok->column);
}



static Expr* logical_and(void) {
    Expr* e = comparison();
    while (match(TOKEN_AND)) {
        Token* tok = previous();
        e = expr_with_loc(expr_binary(OP_AND, e, comparison()), tok->line, tok->column);
    }
    return e;
}

static Expr* logical_or(void) {
    Expr* e = logical_and();
    while (match(TOKEN_OR)) {
        Token* tok = previous();
        e = expr_with_loc(expr_binary(OP_OR, e, logical_and()), tok->line, tok->column);
    }
    return e;
}

static Expr* expression(void) {
    return logical_or();
}

static Expr* expression_no_cast(void) {
    int prev = parser_allow_casts;
    parser_allow_casts = 0;
    Expr* expr = expression();
    parser_allow_casts = prev;
    return expr;
}

Expr* parse_expression_snippet(const char* src, int base_line, int base_column) {
    int count = 0;
    Token* toks = lex(src, &count);

    Token* saved_tokens = tokens;
    int saved_current = current;

    tokens = toks;
    current = 0;

    if ((base_line > 0 || base_column > 0) && toks) {
        for (int i = 0; i < count; i++) {
            int line = toks[i].line;
            if (base_line > 0)
                toks[i].line = base_line + line - 1;
            if (line == 1 && base_column > 0)
                toks[i].column = base_column + toks[i].column - 1;
        }
    }

    Expr* e = expression();
    if (!e)
        e = expr_with_loc(expr_literal(0), base_line, base_column);

    if (!is_at_end()) {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid interpolation expression",
            "Extra tokens inside string interpolation.",
            "Example: \"Value is $(x + 2)\""
        );
    }

    tokens = saved_tokens;
    current = saved_current;
    lex_free(toks, count);

    return e;
}

static int interpolation_legacy_enabled(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    const char* env = getenv("SICHT_LEGACY_INTERP");
    cached = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return cached;
}

static void ensure_string_parts_capacity(StringPart** parts, int* capacity, int count, int needed) {
    if (!parts || !capacity)
        return;
    if (*capacity >= needed)
        return;
    int new_cap = *capacity > 0 ? *capacity * 2 : 16;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2) {
            parse_error(0, 0, "Interpolation too complex", "String interpolation exceeded internal limits.", "");
            exit(1);
        }
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(StringPart)) {
        parse_error(0, 0, "Interpolation too complex", "String interpolation exceeded internal limits.", "");
        exit(1);
    }
    StringPart* grown = arena_alloc(sizeof(StringPart) * (size_t)new_cap);
    if (*parts && count > 0)
        memcpy(grown, *parts, sizeof(StringPart) * (size_t)count);
    *parts = grown;
    *capacity = new_cap;
}

static void parse_interpolated_string(
    const char* src,
    int line,
    int column,
    StringPart** out_parts,
    int* out_count
) {
    const char* p = src;
    const char* s = src;

    int capacity = 16;
    if ((size_t)capacity > SIZE_MAX / sizeof(StringPart)) {
        parse_error(line, column, "Interpolation too complex", "String interpolation exceeded internal limits.", "");
        exit(1);
    }
    *out_parts = arena_alloc(sizeof(StringPart) * (size_t)capacity);
    *out_count = 0;
    int legacy_interp = interpolation_legacy_enabled();

    while (*p) {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            continue;
        }

        int is_interp = 0;
        const char* marker = NULL;
        if (*p == '$' && p[1] == '(') {
            is_interp = 1;
            marker = p;
            p += 2;
        } else if (legacy_interp && *p == '(') {
            is_interp = 1;
            marker = p;
            p += 1;
        }

        if (is_interp) {
            if (marker > s) {
                ensure_string_parts_capacity(out_parts, &capacity, *out_count, *out_count + 1);
                (*out_parts)[(*out_count)++] = (StringPart){
                    .type = STR_TEXT,
                    .line = line,
                    .column = column,
                    .text = sicht_strndup(s, marker - s),
                    .expr = NULL
                };
            }

            const char* expr_start = p;
            int depth = 1;

            while (*p && depth > 0) {
                if (*p == '"') {
                    p++;
                    while (*p) {
                        if (*p == '\\' && p[1] != '\0') {
                            p += 2;
                            continue;
                        }
                        if (*p == '"') {
                            p++;
                            break;
                        }
                        p++;
                    }
                    continue;
                }
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }

            if (depth != 0) {
                parse_error(
                    line,
                    column,
                    "Unclosed interpolation",
                    "Missing ')'",
                    legacy_interp ? "Example: \"Value is (x)\"" : "Example: \"Value is $(x)\""
                );
                s = p;
                break;
            }

            char* expr_src = sicht_strndup(expr_start, (p - 1) - expr_start);
            {
                int expr_column = column;
                if (column > 0) {
                    size_t offset = (size_t)(expr_start - src);
                    expr_column = column + 1 + (int)offset;
                }
                Expr* e = parse_expression_snippet(expr_src, line, expr_column);

                ensure_string_parts_capacity(out_parts, &capacity, *out_count, *out_count + 1);
                (*out_parts)[(*out_count)++] = (StringPart){
                    .type = STR_EXPR,
                    .line = line,
                    .column = column,
                    .expr = e,
                    .text = NULL
                };
            }

            s = p;
            continue;
        }

        p++;
    }

    if (p > s) {
        ensure_string_parts_capacity(out_parts, &capacity, *out_count, *out_count + 1);
        (*out_parts)[(*out_count)++] = (StringPart){
            .type = STR_TEXT,
            .line = line,
            .column = column,
            .text = sicht_strndup(s, p - s),
            .expr = NULL
        };
    }
}



static ASTNode* statement(void);
static ASTNode* statement_safe(void);
static void ast_add_safe(ASTNode* block);
static char* parse_library_name(const char* expected_title, const char* expected_hint);

static ASTNode* node_with_location(ASTNode* node, int line, int column) {
    if (!node)
        return NULL;
    node->line = line;
    node->column = column;
    return node;
}

static int str_eq_ci(const char* a, const char* b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (tolower(ca) != tolower(cb))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int llvl_unit_multiplier(const char* unit, int* out_mult) {
    if (!unit || !out_mult)
        return 0;
    if (str_eq_ci(unit, "b") || str_eq_ci(unit, "byte") || str_eq_ci(unit, "bytes")) {
        *out_mult = 1;
        return 1;
    }
    if (str_eq_ci(unit, "kb") || str_eq_ci(unit, "kib")) {
        *out_mult = 1024;
        return 1;
    }
    if (str_eq_ci(unit, "mb") || str_eq_ci(unit, "mib")) {
        *out_mult = 1024 * 1024;
        return 1;
    }
    if (str_eq_ci(unit, "gb") || str_eq_ci(unit, "gib")) {
        *out_mult = 1024 * 1024 * 1024;
        return 1;
    }
    return 0;
}

static int llvl_time_multiplier(const char* unit, int* out_mult) {
    if (!unit || !out_mult)
        return 0;
    if (str_eq_ci(unit, "ms") || str_eq_ci(unit, "millisecond") || str_eq_ci(unit, "milliseconds")) {
        *out_mult = 1;
        return 1;
    }
    if (str_eq_ci(unit, "s") || str_eq_ci(unit, "sec") ||
        str_eq_ci(unit, "second") || str_eq_ci(unit, "seconds")) {
        *out_mult = 1000;
        return 1;
    }
    return 0;
}

static int llvl_time_multiplier_float(const char* unit, double* out_mult) {
    if (!unit || !out_mult)
        return 0;
    if (str_eq_ci(unit, "us") || str_eq_ci(unit, "microsecond") || str_eq_ci(unit, "microseconds")) {
        *out_mult = 0.001;
        return 1;
    }
    if (str_eq_ci(unit, "ns") || str_eq_ci(unit, "nanosecond") || str_eq_ci(unit, "nanoseconds")) {
        *out_mult = 0.000001;
        return 1;
    }
    return 0;
}

static int llvl_consume_unit_multiplier(int* out_mult) {
    if (match(TOKEN_BYTE)) {
        *out_mult = 1;
        return 1;
    }
    if (check(TOKEN_IDENTIFIER)) {
        int mult = 0;
        if (llvl_unit_multiplier(peek()->value, &mult)) {
            advance();
            *out_mult = mult;
            return 1;
        }
    }
    return 0;
}

static int llvl_consume_time_multiplier(int* out_mult) {
    if (check(TOKEN_IDENTIFIER)) {
        int mult = 0;
        if (llvl_time_multiplier(peek()->value, &mult)) {
            advance();
            *out_mult = mult;
            return 1;
        }
    }
    return 0;
}

static Expr* llvl_parse_size_expr(void) {
    Expr* size = expression();
    int mult = 0;
    if (llvl_consume_unit_multiplier(&mult) && mult != 1) {
        size = expr_binary(OP_MUL, size, expr_literal(mult));
    }
    return size;
}

static Expr* llvl_parse_time_expr(void) {
    Expr* duration = expression();
    if (check(TOKEN_IDENTIFIER)) {
        double fmult = 0.0;
        if (llvl_time_multiplier_float(peek()->value, &fmult)) {
            advance();
            Expr* base = expr_cast(duration, CAST_TO_FLOAT);
            duration = expr_binary(OP_MUL, base, expr_float(fmult));
            return duration;
        }
    }
    int mult = 0;
    if (llvl_consume_time_multiplier(&mult) && mult != 1) {
        duration = expr_binary(OP_MUL, duration, expr_literal(mult));
    }
    return duration;
}

static int llvl_parse_type_spec(LlvlTypeKind* out_kind, char** out_name) {
    if (!out_kind || !out_name)
        return 0;
    *out_name = NULL;

    if (match(TOKEN_TYPE_INT)) {
        *out_kind = LLVL_TYPE_INT;
        return 1;
    }
    if (match(TOKEN_TYPE_FLOAT)) {
        *out_kind = LLVL_TYPE_FLOAT;
        return 1;
    }
    if (match(TOKEN_BYTE)) {
        *out_kind = LLVL_TYPE_BYTE;
        return 1;
    }
    if (match(TOKEN_STRUCT)) {
        consume(TOKEN_IDENTIFIER, "Expected struct name", "Use: struct MyStruct");
        *out_kind = LLVL_TYPE_STRUCT;
        *out_name = previous()->value;
        return 1;
    }
    if (match(TOKEN_UNION)) {
        consume(TOKEN_IDENTIFIER, "Expected union name", "Use: union MyUnion");
        *out_kind = LLVL_TYPE_UNION;
        *out_name = previous()->value;
        return 1;
    }

    return 0;
}

static int parse_positive_int_token(const char* text, int* out_value) {
    if (!text || !out_value)
        return 0;
    errno = 0;
    char* end = NULL;
    long val = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return 0;
    if (val < 0 || val > INT_MAX)
        return 0;
    *out_value = (int)val;
    return 1;
}

static int llvl_parse_field_decl(LlvlField* out_field) {
    if (!out_field)
        return 0;

    LlvlTypeKind kind;
    char* type_name = NULL;
    if (!llvl_parse_type_spec(&kind, &type_name))
        return 0;

    int array_len = 0;
    if (match(TOKEN_LBRACKET)) {
        if (!check(TOKEN_INTEGER)) {
            parse_error(
                peek()->line,
                peek()->column,
                "Expected array length",
                "Field arrays require a literal length.",
                "Example: byte[8] data"
            );
            return 0;
        }
        consume(TOKEN_INTEGER, "Expected array length", "Example: byte[8] data");
        int len = 0;
        if (!parse_positive_int_token(previous()->value, &len) || len <= 0) {
            parse_error(
                previous()->line,
                previous()->column,
                "Invalid array length",
                "Array length must be a positive integer.",
                "Example: byte[8] data"
            );
            return 0;
        }
        consume(TOKEN_RBRACKET, "Expected ']'", "Close array length with ']'");
        array_len = len;
    }

    consume(TOKEN_IDENTIFIER, "Expected field name", "Example: int id");

    out_field->name = previous()->value;
    out_field->kind = kind;
    out_field->type_name = type_name;
    out_field->array_len = array_len;
    return 1;
}

static int is_llvl_block_end(void) {
    if (check(TOKEN_ENDLLVL))
        return 1;
    if (check(TOKEN_END) && check_next(TOKEN_LLVL))
        return 1;
    return 0;
}

static int is_struct_block_end(void) {
    if (check(TOKEN_ENDSTRUCT))
        return 1;
    if (check(TOKEN_END) && check_next(TOKEN_STRUCT))
        return 1;
    return 0;
}

static int is_union_block_end(void) {
    if (check(TOKEN_ENDUNION))
        return 1;
    if (check(TOKEN_END) && check_next(TOKEN_UNION))
        return 1;
    return 0;
}

static int is_enum_block_end(void) {
    if (check(TOKEN_ENDENUM))
        return 1;
    if (check(TOKEN_END) && check_next(TOKEN_ENUM))
        return 1;
    return 0;
}

static int is_bitfield_block_end(void) {
    if (check(TOKEN_ENDBITFIELD))
        return 1;
    if (check(TOKEN_END) && check_next(TOKEN_BITFIELD))
        return 1;
    return 0;
}



static ASTNode* while_statement(void) {
    Expr* condition = expression();
    Expr* repeat_limit = NULL;

    if (match(TOKEN_REPEAT)) {
        int wrapped = match(TOKEN_LPAREN);
        repeat_limit = expression();
        consume(TOKEN_TIMES, "Expected 'times'", "Use: while condition repeat (5 times)");
        if (wrapped)
            consume(TOKEN_RPAREN, "Expected ')'", "Close repeat limit with ')'");
    }

    ASTNode* body = ast_program();
    while (!check(TOKEN_ENDWHILE))
        ast_add_safe(body);

    consume(TOKEN_ENDWHILE, "Missing 'endwhile'", "Close loop with 'endwhile'");

    return ast_while(condition, repeat_limit, body);
}
static ASTNode* repeat_statement(void) {

    Expr* times = expression();
    consume(TOKEN_TIMES, "Expected 'times'", "Use: repeat 5 times");

    ASTNode* body = ast_program();
    while (!check(TOKEN_ENDREPEAT))
        ast_add_safe(body);

    consume(TOKEN_ENDREPEAT, "Missing 'endrepeat'", "Close loop with 'endrepeat'");

    return ast_repeat(times, body);
}

static ASTNode* for_each_statement(void) {
    consume(TOKEN_EACH, "Expected 'each'", "Use: for each item in(values)");
    consume(TOKEN_IDENTIFIER, "Expected item name", "Use: for each item in(values)");
    char* item_name = previous()->value;

    consume(TOKEN_IN, "Expected 'in'", "Use: for each item in(values)");
    Expr* iterable = NULL;
    if (match(TOKEN_LPAREN)) {
        iterable = expression();
        consume(TOKEN_RPAREN, "Expected ')'", "Close iterable with ')'");
    } else {
        iterable = expression();
    }

    ASTNode* body = ast_program();
    while (!check(TOKEN_ENDFOR) && !is_at_end())
        ast_add_safe(body);

    consume(TOKEN_ENDFOR, "Missing 'endfor'", "Close loop with 'endfor'");
    return ast_for_each(item_name, iterable, body);
}

static ASTNode* match_statement(void) {
    ASTNode* node = ast_match(expression());
    int case_count = 0;

    while (match(TOKEN_CASE)) {
        Expr* case_value = expression();
        ASTNode* block = ast_program();

        while (!check(TOKEN_CASE) &&
               !check(TOKEN_OTHERWISE) &&
               !check(TOKEN_ENDMATCH) &&
               !is_at_end()) {
            ast_add_safe(block);
        }

        ast_match_add_case(node, case_value, block);
        case_count++;
    }

    if (case_count == 0) {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid match statement",
            "A match statement needs at least one case.",
            "Example: match x / case 1 ... / endmatch"
        );
        return NULL;
    }

    if (match(TOKEN_OTHERWISE)) {
        ASTNode* otherwise_block = ast_program();
        while (!check(TOKEN_ENDMATCH) && !is_at_end())
            ast_add_safe(otherwise_block);
        ast_match_set_otherwise(node, otherwise_block);
    }

    consume(TOKEN_ENDMATCH, "Missing 'endmatch'", "Close match with 'endmatch'");
    return node;
}

static char* normalize_library_name(const char* raw_name) {
    char buffer[512];
    int len = 0;

    while (raw_name[len]) {
        if (len >= (int)sizeof(buffer) - 1) {
            parse_error(
                peek()->line,
                peek()->column,
                "Library name too long",
                "Library names are limited to 511 characters.",
                "Use a shorter module path."
            );
            return sicht_strdup("");
        }
        buffer[len] = raw_name[len] == '\\' ? '/' : raw_name[len];
        len++;
    }
    buffer[len] = '\0';

    if (len >= 3 && strcmp(buffer + len - 3, ".si") == 0)
        buffer[len - 3] = '\0';

    if (buffer[0] == '\0') {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid library name",
            "Library name cannot be empty.",
            "Use a name like: math or libs/math"
        );
        return sicht_strdup("");
    }

    return sicht_strdup(buffer);
}

static char* parse_library_name(const char* expected_title, const char* expected_hint) {
    if (match(TOKEN_STRING))
        return normalize_library_name(previous()->value);

    consume(TOKEN_IDENTIFIER, expected_title, expected_hint);

    char combined[512];
    int len = 0;
    const char* first = previous()->value;

    while (first[len]) {
        if (len >= (int)sizeof(combined) - 1) {
            parse_error(
                previous()->line,
                previous()->column,
                "Library name too long",
                "Library names are limited to 511 characters.",
                "Use a shorter module path."
            );
            return sicht_strdup("");
        }
        combined[len] = first[len];
        len++;
    }
    combined[len] = '\0';

    while (match(TOKEN_SLASH)) {
        if (len >= (int)sizeof(combined) - 2) {
            parse_error(
                previous()->line,
                previous()->column,
                "Library name too long",
                "Library names are limited to 511 characters.",
                "Use a shorter module path."
            );
            return sicht_strdup("");
        }
        combined[len++] = '/';
        combined[len] = '\0';

        consume(TOKEN_IDENTIFIER,
            "Expected library path segment",
            "Use module paths like: libs/math"
        );

        const char* segment = previous()->value;
        for (int i = 0; segment[i]; i++) {
            if (len >= (int)sizeof(combined) - 1) {
                parse_error(
                    previous()->line,
                    previous()->column,
                    "Library name too long",
                    "Library names are limited to 511 characters.",
                    "Use a shorter module path."
                );
                return sicht_strdup("");
            }
            combined[len++] = segment[i];
        }
        combined[len] = '\0';
    }

    return normalize_library_name(combined);
}

static ASTNode* try_statement(void) {
    ASTNode* try_block = ast_program();
    while (!check(TOKEN_OTHERWISE) && !check(TOKEN_ENDTRY) && !is_at_end())
        ast_add_safe(try_block);

    ASTNode* otherwise_block = NULL;
    if (match(TOKEN_OTHERWISE)) {
        otherwise_block = ast_program();
        while (!check(TOKEN_ENDTRY) && !is_at_end())
            ast_add_safe(otherwise_block);
    }

    consume(TOKEN_ENDTRY, "Missing 'endtry'", "Close try block with 'endtry'");
    return ast_try(try_block, otherwise_block);
}



static ASTNode* if_statement(void) {
    ASTNode* if_node = ast_if();

    Expr* cond = expression();
    consume(TOKEN_THEN, "Missing 'then'", "Use: if <condition> then");

    ASTNode* block = ast_program();
    while (!check(TOKEN_OTHERWISE) && !check(TOKEN_ENDIF) && !is_at_end())
        ast_add_safe(block);

    ast_if_add_branch(if_node, cond, block);

    while (match(TOKEN_OTHERWISE)) {
        Token* otherwise_token = previous();
        int same_line_else_if = check(TOKEN_IF) && peek()->line == otherwise_token->line;
        if (same_line_else_if) {
            advance();
            Expr* c = expression();
            consume(TOKEN_THEN, "Missing 'then'", "otherwise if <cond> then");

            ASTNode* b = ast_program();
            while (!check(TOKEN_OTHERWISE) && !check(TOKEN_ENDIF) && !is_at_end())
                ast_add_safe(b);

            ast_if_add_branch(if_node, c, b);
        } else {
            ASTNode* else_block = ast_program();
            while (!check(TOKEN_ENDIF) && !is_at_end())
                ast_add_safe(else_block);

            ast_if_set_else(if_node, else_block);
            break;
        }
    }

    consume(TOKEN_ENDIF, "Missing 'endif'", "Close if with 'endif'");
    return if_node;
}


static ASTNode* statement(void) {
    int stmt_line = peek()->line;
    int stmt_column = peek()->column;

    
    if (match(TOKEN_CREATE)) {
        if (match(TOKEN_FUNCTION)) {
            consume(TOKEN_IDENTIFIER, "Expected function name", "Use: create function name(...)");
            char* name = previous()->value;

            char* params[64];
            Expr* defaults[64];
            int param_count = 0;
            int required_param_count = 0;
            int seen_default = 0;

            if (match(TOKEN_LPAREN)) {
                if (!check(TOKEN_RPAREN)) {
                    do {
                        if (param_count >= 64) {
                            parse_error(
                                peek()->line,
                                peek()->column,
                                "Too many parameters",
                                "Functions support at most 64 parameters.",
                                "Reduce parameter count."
                            );
                            return NULL;
                        }

                        consume(TOKEN_IDENTIFIER, "Expected parameter name", "Example: create function sum(a, b)");
                        params[param_count] = previous()->value;
                        defaults[param_count] = NULL;

                        if (match(TOKEN_DEFAULT)) {
                            defaults[param_count] = expression();
                            seen_default = 1;
                        } else {
                            if (seen_default) {
                                parse_error(
                                    peek()->line,
                                    peek()->column,
                                    "Invalid parameter order",
                                    "Parameters without defaults cannot come after default parameters.",
                                    "Move required parameters before default parameters."
                                );
                                return NULL;
                            }
                            required_param_count++;
                        }

                        param_count++;
                    } while (match(TOKEN_COMMA));
                }
                consume(TOKEN_RPAREN, "Expected ')'", "Close parameter list with ')'");
            }

            ASTNode* body = ast_program();
            while (!check(TOKEN_ENDFUNCTION) && !is_at_end())
                ast_add_safe(body);

            consume(TOKEN_ENDFUNCTION, "Missing 'endfunction'", "Close function with 'endfunction'");
            return node_with_location(
                ast_function(name, params, defaults, param_count, required_param_count, body),
                stmt_line,
                stmt_column
            );
        }

        if (match(TOKEN_TYPE_KEYWORD)) {
            consume(TOKEN_IDENTIFIER, "Expected type name", "Use: create type Person(name, age)");
            char* type_name = previous()->value;

            char* fields[64];
            int field_count = 0;

            if (match(TOKEN_LPAREN)) {
                if (!check(TOKEN_RPAREN)) {
                    do {
                        if (field_count >= 64) {
                            parse_error(
                                peek()->line,
                                peek()->column,
                                "Too many fields",
                                "Types support at most 64 fields.",
                                "Reduce field count."
                            );
                            return NULL;
                        }

                        consume(TOKEN_IDENTIFIER, "Expected field name", "Example: create type Person(name, age)");
                        fields[field_count++] = previous()->value;
                    } while (match(TOKEN_COMMA));
                }
                consume(TOKEN_RPAREN, "Expected ')'", "Close field list with ')'");
            }

            consume(TOKEN_ENDTYPE, "Missing 'endtype'", "Close type declaration with 'endtype'");
            return node_with_location(ast_type_decl(type_name, fields, field_count), stmt_line, stmt_column);
        }

        if (match(TOKEN_LIBRARY)) {
            char* library_name = parse_library_name(
                "Expected library name",
                "Use: create library math (or create library libs/math)"
            );
            return node_with_location(ast_create_library(library_name), stmt_line, stmt_column);
        }

        parse_error(
            peek()->line,
            peek()->column,
            "Invalid create statement",
            "Expected 'function', 'type', or 'library' after 'create'.",
            "Examples: create function sum(a, b), create type Person(name), create library math"
        );
        return NULL;
    }

    if (match(TOKEN_STRUCT)) {
        consume(TOKEN_IDENTIFIER, "Expected struct name", "Use: struct MyStruct");
        char* struct_name = previous()->value;

        LlvlField fields[128];
        int field_count = 0;

        while (!is_struct_block_end() && !is_at_end()) {
            if (field_count >= 128) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Too many fields",
                    "Structs support at most 128 fields.",
                    "Reduce field count."
                );
                return NULL;
            }

            if (!llvl_parse_field_decl(&fields[field_count])) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Invalid struct field",
                    "Expected a typed field declaration.",
                    "Example: int id"
                );
                return NULL;
            }
            field_count++;
        }

        if (match(TOKEN_ENDSTRUCT)) {
        } else {
            consume(TOKEN_END, "Missing 'end struct'", "Close struct with 'end struct'");
            consume(TOKEN_STRUCT, "Missing 'struct' after end", "Use: end struct");
        }

        return node_with_location(
            ast_llvl_struct_decl(struct_name, fields, field_count),
            stmt_line,
            stmt_column
        );
    }

    if (match(TOKEN_UNION)) {
        consume(TOKEN_IDENTIFIER, "Expected union name", "Use: union MyUnion");
        char* union_name = previous()->value;

        LlvlField fields[128];
        int field_count = 0;

        while (!is_union_block_end() && !is_at_end()) {
            if (field_count >= 128) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Too many fields",
                    "Unions support at most 128 fields.",
                    "Reduce field count."
                );
                return NULL;
            }

            if (!llvl_parse_field_decl(&fields[field_count])) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Invalid union field",
                    "Expected a typed field declaration.",
                    "Example: int id"
                );
                return NULL;
            }
            field_count++;
        }

        if (match(TOKEN_ENDUNION)) {
        } else {
            consume(TOKEN_END, "Missing 'end union'", "Close union with 'end union'");
            consume(TOKEN_UNION, "Missing 'union' after end", "Use: end union");
        }

        return node_with_location(
            ast_llvl_union_decl(union_name, fields, field_count),
            stmt_line,
            stmt_column
        );
    }

    if (match(TOKEN_ENUM)) {
        consume(TOKEN_IDENTIFIER, "Expected enum name", "Use: enum Color");
        char* enum_name = previous()->value;

        char* names[128];
        int values[128];
        int count = 0;
        int next_value = 0;

        while (!is_enum_block_end() && !is_at_end()) {
            if (count >= 128) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Too many enum values",
                    "Enums support at most 128 entries.",
                    "Reduce entry count."
                );
                return NULL;
            }

            consume(TOKEN_IDENTIFIER, "Expected enum name", "Example: RED");
            char* entry_name = previous()->value;
            int value = next_value;

            if (match(TOKEN_EQUAL) || match(TOKEN_IS)) {
                if (previous()->type == TOKEN_IS && match(TOKEN_EQUAL)) {
                    (void)match(TOKEN_TO);
                } else {
                    (void)match(TOKEN_TO);
                }

                int sign = 1;
                if (match(TOKEN_MINUS))
                    sign = -1;

                consume(TOKEN_INTEGER, "Expected integer enum value", "Example: RED equal 0");
                int parsed = 0;
                if (!parse_positive_int_token(previous()->value, &parsed)) {
                    parse_error(
                        previous()->line,
                        previous()->column,
                        "Invalid enum value",
                        "Enum values must be integers.",
                        "Example: RED equal 0"
                    );
                    return NULL;
                }
                value = sign * parsed;
            }

            names[count] = entry_name;
            values[count] = value;
            next_value = value + 1;
            count++;
        }

        if (match(TOKEN_ENDENUM)) {
        } else {
            consume(TOKEN_END, "Missing 'end enum'", "Close enum with 'end enum'");
            consume(TOKEN_ENUM, "Missing 'enum' after end", "Use: end enum");
        }

        return node_with_location(
            ast_llvl_enum_decl(enum_name, names, values, count),
            stmt_line,
            stmt_column
        );
    }

    if (match(TOKEN_BITFIELD)) {
        consume(TOKEN_IDENTIFIER, "Expected bitfield name", "Use: bitfield Flags");
        char* bitfield_name = previous()->value;

        char* names[128];
        int bits[128];
        int count = 0;

        while (!is_bitfield_block_end() && !is_at_end()) {
            if (count >= 128) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Too many bitfield entries",
                    "Bitfields support at most 128 entries.",
                    "Reduce entry count."
                );
                return NULL;
            }

            consume(TOKEN_BIT, "Expected 'bit'", "Example: bit 0 ready");
            consume(TOKEN_INTEGER, "Expected bit index", "Example: bit 0 ready");
            int bit_index = 0;
            if (!parse_positive_int_token(previous()->value, &bit_index)) {
                parse_error(
                    previous()->line,
                    previous()->column,
                    "Invalid bit index",
                    "Bit index must be a non-negative integer.",
                    "Example: bit 0 ready"
                );
                return NULL;
            }
            consume(TOKEN_IDENTIFIER, "Expected bit name", "Example: bit 0 ready");
            char* entry_name = previous()->value;

            names[count] = entry_name;
            bits[count] = bit_index;
            count++;
        }

        if (match(TOKEN_ENDBITFIELD)) {
        } else {
            consume(TOKEN_END, "Missing 'end bitfield'", "Close bitfield with 'end bitfield'");
            consume(TOKEN_BITFIELD, "Missing 'bitfield' after end", "Use: end bitfield");
        }

        return node_with_location(
            ast_llvl_bitfield_decl(bitfield_name, names, bits, count),
            stmt_line,
            stmt_column
        );
    }

    
    if (match(TOKEN_LOAD)) {
        if (match(TOKEN_LIBRARY)) {
            char* library_name = parse_library_name(
                "Expected library name",
                "Use: load library math, load library libs/math, or load library \"libs/math\""
            );
            return node_with_location(ast_load_library(library_name), stmt_line, stmt_column);
        }

        if (match(TOKEN_FILE)) {
            Expr* path = expression();
            return node_with_location(ast_load_file(path), stmt_line, stmt_column);
        }

        parse_error(
            peek()->line,
            peek()->column,
            "Invalid load statement",
            "Expected 'library' or 'file' after 'load'.",
            "Use: load library math, or load file \"script.si\""
        );
        return NULL;
    }

    if (match(TOKEN_LLVL)) {
        ASTNode* body = ast_llvl_block();
        while (!is_llvl_block_end() && !is_at_end())
            ast_add_safe(body);

        if (match(TOKEN_ENDLLVL)) {
            return node_with_location(body, stmt_line, stmt_column);
        }
        consume(TOKEN_END, "Missing 'end llvl'", "Close llvl block with 'end llvl'");
        consume(TOKEN_LLVL, "Missing 'llvl' after end", "Use: end llvl");
        return node_with_location(body, stmt_line, stmt_column);
    }

    if (match(TOKEN_ENABLE) || match(TOKEN_DISABLE)) {
        int enabled = previous()->type == TOKEN_ENABLE;
        consume(TOKEN_LLVL, "Expected 'llvl'", "Use: enable llvl bounds check");
        if (match(TOKEN_BOUNDS)) {
            consume(TOKEN_CHECK, "Expected 'check'", "Use: enable llvl bounds check");
            return node_with_location(ast_llvl_set_check(LLVL_CHECK_BOUNDS, enabled), stmt_line, stmt_column);
        }
        if (match(TOKEN_POINTER)) {
            consume(TOKEN_CHECK, "Expected 'check(s)'", "Use: enable llvl pointer checks");
            return node_with_location(ast_llvl_set_check(LLVL_CHECK_POINTER, enabled), stmt_line, stmt_column);
        }
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid llvl safety toggle",
            "Expected bounds or pointer after llvl.",
            "Use: enable llvl bounds check or enable llvl pointer checks"
        );
        return NULL;
    }

    if (match(TOKEN_OFFER)) {
        consume(TOKEN_IDENTIFIER, "Expected name to offer", "Use: offer square");
        return node_with_location(ast_library_offer(previous()->value), stmt_line, stmt_column);
    }

    if (match(TOKEN_TAKE)) {
        if (check(TOKEN_IDENTIFIER) && strcmp(peek()->value, "all") == 0) {
            Token* tok = advance();
            parse_error(
                tok->line,
                tok->column,
                "Invalid take syntax",
                "`take all` was replaced.",
                "Use: take everything from <library>"
            );
            return NULL;
        }

        if (check(TOKEN_IDENTIFIER) && strcmp(peek()->value, "everything") == 0) {
            advance();
            if (match(TOKEN_AS) || match(TOKEN_COMMA)) {
                parse_error(
                    previous()->line,
                    previous()->column,
                    "Invalid take everything syntax",
                    "`take everything` does not support aliases or multiple names.",
                    "Use: take everything from <library>"
                );
                return NULL;
            }
            consume(TOKEN_FROM, "Expected 'from'", "Use: take everything from math");
            char* library_name = parse_library_name(
                "Expected library name",
                "Use: take everything from math or take everything from \"libs/math\""
            );
            return node_with_location(ast_library_take_all(library_name), stmt_line, stmt_column);
        }

        char* names[64];
        char* aliases[64];
        int take_count = 0;

        do {
            if (take_count >= 64) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Too many take items",
                    "take supports at most 64 names.",
                    "Split imports into multiple take statements."
                );
                return NULL;
            }

            consume(TOKEN_IDENTIFIER, "Expected name to take", "Use: take square from math");
            names[take_count] = previous()->value;
            aliases[take_count] = NULL;

            if (match(TOKEN_AS)) {
                consume(TOKEN_IDENTIFIER, "Expected alias name", "Use: take square as sq from math");
                aliases[take_count] = previous()->value;
            }

            take_count++;
        } while (match(TOKEN_COMMA));

        consume(TOKEN_FROM, "Expected 'from'", "Use: take square from math");
        char* library_name = parse_library_name(
            "Expected library name",
            "Use: take square from math or take square from \"libs/math\""
        );

        if (take_count == 1 && !aliases[0] && match(TOKEN_AS)) {
            consume(TOKEN_IDENTIFIER, "Expected alias name", "Use: take square from math as sq");
            aliases[0] = previous()->value;
        }

        if (take_count == 1)
            return node_with_location(ast_library_take(names[0], library_name, aliases[0]), stmt_line, stmt_column);

        ASTNode* block = ast_block();
        for (int i = 0; i < take_count; i++) {
            ASTNode* item = ast_library_take(names[i], library_name, aliases[i]);
            item->line = stmt_line;
            item->column = stmt_column;
            ast_add(block, item);
        }
        return node_with_location(block, stmt_line, stmt_column);
    }

    if (match(TOKEN_WRITE_KEYWORD) || match(TOKEN_APPEND)) {
        int append_mode = previous()->type == TOKEN_APPEND;
        if (!append_mode && match(TOKEN_PORT)) {
            Expr* port = expression();
            consume(TOKEN_VALUE, "Expected 'value'", "Use: write port 0x3F8 value 42");
            Expr* value = expression();
            return node_with_location(ast_llvl_port_write(port, value), stmt_line, stmt_column);
        }
        consume(TOKEN_FILE, "Expected 'file'", "Use: write file \"notes.txt\" to \"hello\"");
        Expr* path = expression();
        consume(TOKEN_TO, "Expected 'to'", "Use: write file \"notes.txt\" to \"hello\"");
        Expr* content = expression();
        return node_with_location(
            append_mode ? ast_file_append(path, content) : ast_file_write(path, content),
            stmt_line,
            stmt_column
        );
    }

    if (match(TOKEN_YIELD)) {

    if (check(TOKEN_ENDFUNCTION) || check(TOKEN_END) || check(TOKEN_EOF)) {
        parse_error(
            peek()->line,
            peek()->column,
            "Missing yield value",
            "yield needs a value.",
            "Use: yield <expression>"
        );
        return NULL;
    }

    return node_with_location(ast_yield(expression()), stmt_line, stmt_column);
}



    if (match(TOKEN_RETURN)) {
        if (check(TOKEN_ENDFUNCTION) || check(TOKEN_END) || check(TOKEN_EOF)) {
            parse_error(
                peek()->line,
                peek()->column,
                "Missing return value",
                "return needs a value.",
                "Use: return <expression> (or give <expression>)"
            );
            return NULL;
        }
        return node_with_location(ast_return(expression()), stmt_line, stmt_column);
    }

    if (match(TOKEN_SAVE)) {
        Expr* size_expr = NULL;
        int has_type = 0;
        LlvlTypeKind elem_kind = LLVL_TYPE_BYTE;
        char* elem_type_name = NULL;

        if (match(TOKEN_STRUCT) || match(TOKEN_UNION)) {
            has_type = 1;
            elem_kind = previous()->type == TOKEN_STRUCT ? LLVL_TYPE_STRUCT : LLVL_TYPE_UNION;
            consume(TOKEN_IDENTIFIER, "Expected type name", "Use: save struct MyStruct for item");
            elem_type_name = previous()->value;
            size_expr = expr_literal(1);
        } else {
            Expr* first = expression();
            if (match(TOKEN_OF)) {
                if (!llvl_parse_type_spec(&elem_kind, &elem_type_name)) {
                    parse_error(
                        peek()->line,
                        peek()->column,
                        "Expected type after 'of'",
                        "Valid types: int, float, byte, struct <Name>, union <Name>.",
                        "Example: save 10 of int for numbers"
                    );
                    return NULL;
                }
                has_type = 1;
                size_expr = first;
            } else if (match(TOKEN_STRUCT) || match(TOKEN_UNION)) {
                has_type = 1;
                elem_kind = previous()->type == TOKEN_STRUCT ? LLVL_TYPE_STRUCT : LLVL_TYPE_UNION;
                consume(TOKEN_IDENTIFIER, "Expected type name", "Use: save 3 struct MyStruct for items");
                elem_type_name = previous()->value;
                size_expr = first;
            } else {
                size_expr = first;
                int mult = 0;
                if (llvl_consume_unit_multiplier(&mult) && mult != 1) {
                    size_expr = expr_binary(OP_MUL, size_expr, expr_literal(mult));
                }
            }
        }

        consume(TOKEN_FOR, "Expected 'for'", "Use: save 4mb for buffer");
        consume(TOKEN_IDENTIFIER, "Expected buffer name", "Use: save 4mb for buffer");
        char* name = previous()->value;
        return node_with_location(ast_llvl_save(name, size_expr, has_type, elem_kind, elem_type_name), stmt_line, stmt_column);
    }

    if (match(TOKEN_GROW) || match(TOKEN_SHRINK)) {
        LlvlResizeOp op = previous()->type == TOKEN_GROW ? LLVL_RESIZE_GROW : LLVL_RESIZE_SHRINK;
        consume(TOKEN_IDENTIFIER, "Expected buffer name", "Use: grow buffer to 8mb");
        char* name = previous()->value;
        consume(TOKEN_TO, "Expected 'to'", "Use: grow buffer to 8mb");
        Expr* size = llvl_parse_size_expr();
        return node_with_location(ast_llvl_resize(name, size, op, 0, LLVL_TYPE_BYTE, NULL), stmt_line, stmt_column);
    }

    if (match(TOKEN_RESIZE)) {
        consume(TOKEN_IDENTIFIER, "Expected buffer name", "Use: resize buffer to 64 bytes");
        char* name = previous()->value;
        consume(TOKEN_TO, "Expected 'to'", "Use: resize buffer to 64 bytes");

        Expr* size_expr = NULL;
        int has_type = 0;
        LlvlTypeKind elem_kind = LLVL_TYPE_BYTE;
        char* elem_type_name = NULL;

        Expr* first = expression();
        if (match(TOKEN_OF)) {
            if (!llvl_parse_type_spec(&elem_kind, &elem_type_name)) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Expected type after 'of'",
                    "Valid types: int, float, byte, struct <Name>, union <Name>.",
                    "Example: resize numbers to 20 of int"
                );
                return NULL;
            }
            has_type = 1;
            size_expr = first;
        } else if (match(TOKEN_STRUCT) || match(TOKEN_UNION)) {
            has_type = 1;
            elem_kind = previous()->type == TOKEN_STRUCT ? LLVL_TYPE_STRUCT : LLVL_TYPE_UNION;
            consume(TOKEN_IDENTIFIER, "Expected type name", "Use: resize items to 3 struct MyStruct");
            elem_type_name = previous()->value;
            size_expr = first;
        } else if (check(TOKEN_TYPE_INT) || check(TOKEN_TYPE_FLOAT)) {
            has_type = 1;
            if (match(TOKEN_TYPE_INT)) elem_kind = LLVL_TYPE_INT;
            else if (match(TOKEN_TYPE_FLOAT)) elem_kind = LLVL_TYPE_FLOAT;
            size_expr = first;
        } else {
            size_expr = first;
            int mult = 0;
            if (llvl_consume_unit_multiplier(&mult) && mult != 1) {
                size_expr = expr_binary(OP_MUL, size_expr, expr_literal(mult));
            }
        }

        return node_with_location(
            ast_llvl_resize(name, size_expr, LLVL_RESIZE_ANY, has_type, elem_kind, elem_type_name),
            stmt_line,
            stmt_column
        );
    }

    if (match(TOKEN_ATOMIC)) {
        if (match(TOKEN_READ_KEYWORD)) {
            consume(TOKEN_FROM, "Expected 'from'", "Example: atomic read from(location)");
            consume(TOKEN_LPAREN, "Expected '('", "Example: atomic read from(location)");
            Expr* target = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close atomic read with ')'");
            Expr* read_expr = expr_with_loc(expr_llvl_atomic_read(target), stmt_line, stmt_column);
            return node_with_location(ast_expr_stmt(read_expr), stmt_line, stmt_column);
        }
        if (match(TOKEN_WRITE_KEYWORD)) {
            consume(TOKEN_VALUE, "Expected 'value'", "Example: atomic write value 1 to(location)");
            Expr* value = expression();
            consume(TOKEN_TO, "Expected 'to'", "Example: atomic write value 1 to(location)");
            consume(TOKEN_LPAREN, "Expected '('", "Example: atomic write value 1 to(location)");
            Expr* address = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close atomic write with ')'");
            return node_with_location(ast_llvl_atomic_op(address, value, LLVL_ATOMIC_WRITE), stmt_line, stmt_column);
        }
        if (match(TOKEN_ADD)) {
            Expr* value = expression();
            consume(TOKEN_TO, "Expected 'to'", "Example: atomic add 5 to(location)");
            consume(TOKEN_LPAREN, "Expected '('", "Example: atomic add 5 to(location)");
            Expr* address = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close atomic add with ')'");
            return node_with_location(ast_llvl_atomic_op(address, value, LLVL_ATOMIC_ADD), stmt_line, stmt_column);
        }
        if (match(TOKEN_SUBTRACT)) {
            Expr* value = expression();
            consume(TOKEN_FROM, "Expected 'from'", "Example: atomic subtract 2 from(location)");
            consume(TOKEN_LPAREN, "Expected '('", "Example: atomic subtract 2 from(location)");
            Expr* address = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close atomic subtract with ')'");
            return node_with_location(ast_llvl_atomic_op(address, value, LLVL_ATOMIC_SUB), stmt_line, stmt_column);
        }
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid atomic statement",
            "Expected read, write, add, or subtract after atomic.",
            "Examples: atomic read from(location), atomic write value 1 to(location)"
        );
        return NULL;
    }

    if (match(TOKEN_MARK)) {
        Expr* target = expression_no_cast();
        consume(TOKEN_AS, "Expected 'as'", "Example: mark location as volatile");
        consume(TOKEN_VOLATILE, "Expected 'volatile'", "Example: mark location as volatile");
        return node_with_location(ast_llvl_mark_volatile(target), stmt_line, stmt_column);
    }

    if (match(TOKEN_COPY)) {
        if (match(TOKEN_MEMORY)) {
            consume(TOKEN_FROM, "Expected 'from'", "Use: copy memory from(buffer) to(other) length(64)");
            consume(TOKEN_LPAREN, "Expected '('", "Use: copy memory from(buffer) to(other) length(64)");
            consume(TOKEN_IDENTIFIER, "Expected source buffer name", NULL);
            char* src = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);
            consume(TOKEN_TO, "Expected 'to'", NULL);
            consume(TOKEN_LPAREN, "Expected '('", NULL);
            consume(TOKEN_IDENTIFIER, "Expected destination buffer name", NULL);
            char* dest = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);

            if (!match(TOKEN_LENGTH)) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Missing length",
                    "copy memory needs an explicit length.",
                    "Use: copy memory from(buffer) to(other) length(64)"
                );
                return NULL;
            }

            Expr* size = NULL;
            if (match(TOKEN_LPAREN)) {
                size = llvl_parse_size_expr();
                consume(TOKEN_RPAREN, "Expected ')'", "Close length with ')'");
            } else {
                size = llvl_parse_size_expr();
            }

            return node_with_location(ast_llvl_copy(src, dest, size, 1), stmt_line, stmt_column);
        }

        if (check(TOKEN_IDENTIFIER) && check_next(TOKEN_TO)) {
            consume(TOKEN_IDENTIFIER, "Expected source buffer", "Use: copy buffer to newBuffer");
            char* src = previous()->value;
            consume(TOKEN_TO, "Expected 'to'", "Use: copy buffer to newBuffer");
            consume(TOKEN_IDENTIFIER, "Expected destination buffer", "Use: copy buffer to newBuffer");
            char* dest = previous()->value;
            return node_with_location(ast_llvl_copy(src, dest, NULL, 0), stmt_line, stmt_column);
        }

        Expr* size = llvl_parse_size_expr();
        consume(TOKEN_FROM, "Expected 'from'", "Use: copy 4 bytes from(buffer) to(otherBuffer)");
        consume(TOKEN_LPAREN, "Expected '('", "Use: copy 4 bytes from(buffer) to(otherBuffer)");
        consume(TOKEN_IDENTIFIER, "Expected source buffer name", NULL);
        char* src = previous()->value;
        consume(TOKEN_RPAREN, "Expected ')'", NULL);
        consume(TOKEN_TO, "Expected 'to'", NULL);
        consume(TOKEN_LPAREN, "Expected '('", NULL);
        consume(TOKEN_IDENTIFIER, "Expected destination buffer name", NULL);
        char* dest = previous()->value;
        consume(TOKEN_RPAREN, "Expected ')'", NULL);
        return node_with_location(ast_llvl_copy(src, dest, size, 1), stmt_line, stmt_column);
    }

    if (match(TOKEN_MOVE)) {
        if (match(TOKEN_MEMORY)) {
            consume(TOKEN_FROM, "Expected 'from'", "Use: move memory from(buffer) to(other) length(64)");
            consume(TOKEN_LPAREN, "Expected '('", "Use: move memory from(buffer) to(other) length(64)");
            consume(TOKEN_IDENTIFIER, "Expected source buffer name", NULL);
            char* src = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);
            consume(TOKEN_TO, "Expected 'to'", NULL);
            consume(TOKEN_LPAREN, "Expected '('", NULL);
            consume(TOKEN_IDENTIFIER, "Expected destination buffer name", NULL);
            char* dest = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);

            if (!match(TOKEN_LENGTH)) {
                parse_error(
                    peek()->line,
                    peek()->column,
                    "Missing length",
                    "move memory needs an explicit length.",
                    "Use: move memory from(buffer) to(other) length(64)"
                );
                return NULL;
            }

            Expr* size = NULL;
            if (match(TOKEN_LPAREN)) {
                size = llvl_parse_size_expr();
                consume(TOKEN_RPAREN, "Expected ')'", "Close length with ')'");
            } else {
                size = llvl_parse_size_expr();
            }

            return node_with_location(ast_llvl_copy(src, dest, size, 1), stmt_line, stmt_column);
        }

        consume(TOKEN_IDENTIFIER, "Expected source buffer", "Use: move buffer to temp");
        char* src = previous()->value;
        consume(TOKEN_TO, "Expected 'to'", "Use: move buffer to temp");
        consume(TOKEN_IDENTIFIER, "Expected destination buffer", "Use: move buffer to temp");
        char* dest = previous()->value;
        return node_with_location(ast_llvl_move(src, dest), stmt_line, stmt_column);
    }

    if (match(TOKEN_SEND)) {
        Expr* value = expression();
        consume(TOKEN_TO, "Expected 'to'", "Use: send 1 to pin 5");
        consume(TOKEN_PIN, "Expected 'pin'", "Use: send 1 to pin 5");
        Expr* pin = expression();
        return node_with_location(ast_llvl_pin_write(value, pin), stmt_line, stmt_column);
    }

    if (match(TOKEN_WAIT)) {
        Expr* duration = llvl_parse_time_expr();
        return node_with_location(ast_llvl_wait(duration), stmt_line, stmt_column);
    }

    if (match(TOKEN_REGISTER)) {
        consume(TOKEN_INTERRUPT, "Expected 'interrupt'", "Use: register interrupt 0x80 handler(myfunc)");
        Expr* interrupt_id = expression();
        consume(TOKEN_HANDLER, "Expected 'handler'", "Use: register interrupt 0x80 handler(myfunc)");
        consume(TOKEN_LPAREN, "Expected '('", "Use: handler(myfunc)");
        consume(TOKEN_IDENTIFIER, "Expected handler function name", "Use: handler(myfunc)");
        char* handler_name = previous()->value;
        consume(TOKEN_RPAREN, "Expected ')'", "Close handler with ')'");
        return node_with_location(ast_llvl_register_interrupt(interrupt_id, handler_name), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_SET)) {

        if (match(TOKEN_FIELD)) {
            consume(TOKEN_IDENTIFIER, "Expected field name", "Use: set field id of(item) to 42");
            char* field_name = previous()->value;

            consume(TOKEN_OF, "Expected 'of'", "Use: set field id of(item) to 42");
            consume(TOKEN_LPAREN, "Expected '('", "Use: set field id of(item) to 42");
            Expr* target = expression();
            consume(TOKEN_RPAREN, "Expected ')'", "Close field target with ')'");
            consume(TOKEN_TO, "Expected 'to'", "Use: set field id of(item) to 42");
            Expr* value = expression();

            return node_with_location(ast_llvl_set_field(field_name, target, value), stmt_line, stmt_column);
        }

        if (match(TOKEN_VALUE)) {
            if (match(TOKEN_OF)) {
                consume(TOKEN_LPAREN, "Expected '('", "Use: set value of(buffer) to 42");
                consume(TOKEN_IDENTIFIER, "Expected buffer name", "Use: set value of(buffer) to 42");
                char* name = previous()->value;
                consume(TOKEN_RPAREN, "Expected ')'", "Close buffer with ')'");
                consume(TOKEN_TO, "Expected 'to'", "Use: set value of(buffer) to 42");
                Expr* value = expression();
                return node_with_location(ast_llvl_set_value(name, value), stmt_line, stmt_column);
            }
            if (match(TOKEN_AT)) {
                consume(TOKEN_LPAREN, "Expected '('", "Use: set value at(location) to 10");
                Expr* address = expression();
                consume(TOKEN_RPAREN, "Expected ')'", "Close address with ')'");
                consume(TOKEN_TO, "Expected 'to'", "Use: set value at(location) to 10");
                Expr* value = expression();
                return node_with_location(ast_llvl_set_at(address, value), stmt_line, stmt_column);
            }
            parse_error(
                peek()->line,
                peek()->column,
                "Invalid set value syntax",
                "Expected 'of' or 'at' after value.",
                "Use: set value of(buffer) to 42"
            );
            return NULL;
        }

        if (match(TOKEN_BYTE)) {
            Expr* index = expression();
            consume(TOKEN_OF, "Expected 'of'", "Use: set byte 0 of(buffer) to 255");
            consume(TOKEN_LPAREN, "Expected '('", "Use: set byte 0 of(buffer) to 255");
            consume(TOKEN_IDENTIFIER, "Expected buffer name", NULL);
            char* name = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);
            consume(TOKEN_TO, "Expected 'to'", "Use: set byte 0 of(buffer) to 255");
            Expr* value = expression();
            return node_with_location(ast_llvl_set_byte(name, index, value), stmt_line, stmt_column);
        }

        if (match(TOKEN_BIT)) {
            Expr* index = expression();
            consume(TOKEN_OF, "Expected 'of'", "Use: set bit 1 of(buffer)");
            consume(TOKEN_LPAREN, "Expected '('", NULL);
            consume(TOKEN_IDENTIFIER, "Expected buffer name", NULL);
            char* name = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);
            return node_with_location(ast_llvl_bit_op(name, index, LLVL_BIT_SET), stmt_line, stmt_column);
        }

        if (match(TOKEN_ELEMENT)) {
            Expr* index = expression();

            consume(TOKEN_OF, "Expected 'of'", NULL);
            consume(TOKEN_LPAREN, "Expected '('", NULL);

            consume(TOKEN_IDENTIFIER, "Expected list name", NULL);
            char* name = previous()->value;

            consume(TOKEN_RPAREN, "Expected ')'", NULL);

            consume(TOKEN_TO, "Expected 'to'", NULL);

            Expr* value = expression();

            return node_with_location(ast_set_element(name, index, value), stmt_line, stmt_column);
        }

        consume(TOKEN_IDENTIFIER, "Expected variable name", "set x to 5");
        char* name = previous()->value;

        consume(TOKEN_TO, "Expected 'to'", "set x to 5");

        return node_with_location(ast_set(name, expression()), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_ADD)) {

        if (match(TOKEN_ITEM)) {
            Expr* key = expression();

            consume(TOKEN_COLON, "Expected ':' after key.", "");

            Expr* value = expression();

            consume(TOKEN_TO, "Expected 'to'.", "");

            consume(TOKEN_LPAREN, "Expected '(' after to.", "");

            consume(TOKEN_IDENTIFIER, "Expected dictionary name.", "");
            char* name = previous()->value;

            consume(TOKEN_RPAREN, "Expected ')'.", "");

            return node_with_location(ast_dict_add(name, key, value), stmt_line, stmt_column);
        }

        Expr* value = expression();

        consume(TOKEN_TO, "Expected 'to'", "");

        consume(TOKEN_LPAREN, "Expected '('", "");

        consume(TOKEN_IDENTIFIER, "Expected list name", NULL);
        char* name = previous()->value;

        consume(TOKEN_RPAREN, "Expected ')'", NULL);

        return node_with_location(ast_list_add(name, value), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_REMOVE)) {

        if (check(TOKEN_IDENTIFIER) && !check_next(TOKEN_FROM)) {
            consume(TOKEN_IDENTIFIER, "Expected buffer name", "Use: remove buffer");
            char* name = previous()->value;
            return node_with_location(ast_llvl_remove(name), stmt_line, stmt_column);
        }

        if (match(TOKEN_ITEM)) {
            Expr* key = expression();

            consume(TOKEN_FROM, "Expected 'from'", NULL);
            consume(TOKEN_LPAREN, "Expected '('", NULL);

            consume(TOKEN_IDENTIFIER, "Expected dictionary name", NULL);
            char* dict_name = previous()->value;

            consume(TOKEN_RPAREN, "Expected ')'", NULL);

            return node_with_location(ast_dict_remove(dict_name, key), stmt_line, stmt_column);
        }

        if (match(TOKEN_ELEMENT)) {

            Expr* index = expression();

            consume(TOKEN_FROM, "Expected 'from'", "");
            consume(TOKEN_LPAREN, "Expected '('", NULL);

            consume(TOKEN_IDENTIFIER, "Expected list name", NULL);
            char* name = previous()->value;

            consume(TOKEN_RPAREN, "Expected ')'", NULL);

            return node_with_location(ast_list_remove_element(name, index), stmt_line, stmt_column);
        }

        Expr* value = expression();

        consume(TOKEN_FROM, "Expected 'from'", "");
        consume(TOKEN_LPAREN, "Expected '('", NULL);

        consume(TOKEN_IDENTIFIER, "Expected list name", NULL);
        char* name = previous()->value;

        consume(TOKEN_RPAREN, "Expected ')'", NULL);

        return node_with_location(ast_list_remove(name, value), stmt_line, stmt_column);
    }

    if (match(TOKEN_FLIP)) {
        consume(TOKEN_BIT, "Expected 'bit'", "Use: flip bit 3 of(buffer)");
        Expr* index = expression();
        consume(TOKEN_OF, "Expected 'of'", "Use: flip bit 3 of(buffer)");
        consume(TOKEN_LPAREN, "Expected '('", NULL);
        consume(TOKEN_IDENTIFIER, "Expected buffer name", NULL);
        char* name = previous()->value;
        consume(TOKEN_RPAREN, "Expected ')'", NULL);
        return node_with_location(ast_llvl_bit_op(name, index, LLVL_BIT_FLIP), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_CLEAR)) {
        if (match(TOKEN_BIT)) {
            Expr* index = expression();
            consume(TOKEN_OF, "Expected 'of'", "Use: clear bit 2 of(buffer)");
            consume(TOKEN_LPAREN, "Expected '('", NULL);
            consume(TOKEN_IDENTIFIER, "Expected buffer name", NULL);
            char* name = previous()->value;
            consume(TOKEN_RPAREN, "Expected ')'", NULL);
            return node_with_location(ast_llvl_bit_op(name, index, LLVL_BIT_CLEAR), stmt_line, stmt_column);
        }

        consume(TOKEN_LPAREN, "Expected '('", NULL);

        consume(TOKEN_IDENTIFIER, "Expected collection name", NULL);
        char* name = previous()->value;

        consume(TOKEN_RPAREN, "Expected ')'", NULL);

        return node_with_location(ast_list_clear(name), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_FOR))
        return node_with_location(for_each_statement(), stmt_line, stmt_column);

    
    if (match(TOKEN_WHILE))
        return node_with_location(while_statement(), stmt_line, stmt_column);

    
    if (match(TOKEN_REPEAT))
        return node_with_location(repeat_statement(), stmt_line, stmt_column);

    
    if (match(TOKEN_PRINT)) {

        if (check(TOKEN_STRING)) {
            SichtTokenType next = tokens[current + 1].type;
            int continues_expression =
                next == TOKEN_PLUS ||
                next == TOKEN_MINUS ||
                next == TOKEN_STAR ||
                next == TOKEN_SLASH ||
                next == TOKEN_AS ||
                next == TOKEN_IS ||
                next == TOKEN_CONTAINS ||
                next == TOKEN_IN ||
                next == TOKEN_NOT ||
                next == TOKEN_AND ||
                next == TOKEN_OR;

            if (!continues_expression) {
                Token* t = advance();

                StringPart* parts = NULL;
                int part_count = 0;

                parse_interpolated_string(
                    t->value,
                    t->line,
                    t->column,
                    &parts,
                    &part_count
                );

                return node_with_location(ast_print_string(parts, part_count), stmt_line, stmt_column);
            }
        }

        return node_with_location(ast_print_expr(expression()), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_IF))
        return node_with_location(if_statement(), stmt_line, stmt_column);

    if (match(TOKEN_TRY))
        return node_with_location(try_statement(), stmt_line, stmt_column);

    
    if (match(TOKEN_MATCH))
        return node_with_location(match_statement(), stmt_line, stmt_column);

    if (check(TOKEN_NEXT) && check_next(TOKEN_FROM)) {
        Expr* expr = expression();
        return node_with_location(ast_expr_stmt(expr), stmt_line, stmt_column);
    }

    
    if (match(TOKEN_NEXT))
        return node_with_location(ast_next(), stmt_line, stmt_column);

    
    if (match(TOKEN_EXIT))
        return node_with_location(ast_exit(), stmt_line, stmt_column);

    

    Expr* expr = expression();

    if (!expr) {
        parse_error(
            peek()->line,
            peek()->column,
            "Invalid statement",
            "Could not parse statement.",
            "Valid statements: set, print, if, try, while, return, write/append file, load library, load file, take ..."
        );
        return NULL;
    }

    return node_with_location(ast_expr_stmt(expr), stmt_line, stmt_column);
}

static ASTNode* statement_safe(void) {
    ASTNode* node = statement();
    if (parser_panic_mode) {
        synchronize();
        return NULL;
    }
    return node;
}

static void ast_add_safe(ASTNode* block) {
    if (!block)
        return;
    ASTNode* stmt = statement_safe();
    if (stmt)
        ast_add(block, stmt);
}



ASTNode* parse(Token* toks, int count) {
    tokens = toks;
    current = 0;
    parser_error_count = 0;
    parser_panic_mode = 0;
    (void)count;

    consume(TOKEN_START, "Missing 'start'", "Programs begin with 'start'");
    Token* start_tok = previous();

    ASTNode* program = ast_program();
    if (check(TOKEN_LLVL) && peek()->line == start_tok->line) {
        advance();
        program->llvl_mode = 1;
    }

    while (!check(TOKEN_END) && !check(TOKEN_ENDLLVL) && !is_at_end())
        ast_add_safe(program);

    if (match(TOKEN_ENDLLVL)) {
        if (parser_error_count > 0)
            return NULL;
        return program;
    }

    consume(TOKEN_END, "Missing 'end'", "Programs end with 'end'");
    if (program->llvl_mode && match(TOKEN_LLVL)) {
        if (parser_error_count > 0)
            return NULL;
        return program;
    }
    if (parser_error_count > 0)
        return NULL;
    return program;
}


