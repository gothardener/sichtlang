#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "lexer.h"
#include "lex_error.h"

typedef struct {
    Token* tokens;
    int count;
    int capacity;
} LexerState;

static char* sicht_strdup(const char* s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "Lexer string exceeded maximum supported size.\n");
        exit(1);
    }
    char* out = malloc(len + 1);
    if (!out) {
        fprintf(stderr, "Out of memory in lexer string allocation.\n");
        exit(1);
    }
    memcpy(out, s, len + 1);
    return out;
}

static void lexer_push(LexerState* st, Token t) {
    if (st->count >= st->capacity) {
        int new_capacity = st->capacity ? st->capacity * 2 : 64;
        if (new_capacity < st->capacity) {
            fprintf(stderr, "Token stream too large.\n");
            exit(1);
        }

        if ((size_t)new_capacity > SIZE_MAX / sizeof(Token)) {
            fprintf(stderr, "Token stream too large.\n");
            exit(1);
        }
        Token* new_tokens = realloc(st->tokens, (size_t)new_capacity * sizeof(Token));
        if (!new_tokens) {
            fprintf(stderr, "Out of memory while growing token stream.\n");
            exit(1);
        }

        st->capacity = new_capacity;
        st->tokens = new_tokens;
    }
    st->tokens[st->count++] = t;
}

Token* lex(const char* src, int* out_count) {
    LexerState st = {0};

    int i = 0;
    int line = 1;
    int column = 1;

    /* Allow UTF-8 BOM at the start of files. */
    if (src[0] && src[1] && src[2] &&
        (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        i = 3;
    }

    while (src[i]) {
        char c = src[i];

        if (c == '\n') {
            line++;
            column = 1;
            i++;
            continue;
        }

        if (c == '#') {
            while (src[i] && src[i] != '\n') {
                i++;
                column++;
            }
            continue;
        }

        if (isspace((unsigned char)c)) {
            column++;
            i++;
            continue;
        }

        int start_col = column;

        if (isdigit((unsigned char)c)) {
            char buf[64];
            int j = 0;
            int has_dot = 0;

            while (isdigit((unsigned char)src[i]) || src[i] == '.') {
                if (src[i] == '.') {
                    if (has_dot) {
                        lex_error(
                            line,
                            column,
                            "Invalid number",
                            "A number cannot contain more than one dot.",
                            "Use a valid integer or float."
                        );
                    }
                    has_dot = 1;
                }
                if (j >= (int)sizeof(buf) - 1) {
                    lex_error(
                        line,
                        column,
                        "Invalid number",
                        "Number literal is too long.",
                        "Use shorter numeric literals."
                    );
                }
                buf[j++] = src[i++];
                column++;
            }

            buf[j] = '\0';

            lexer_push(&st, (Token){
                has_dot ? TOKEN_FLOAT : TOKEN_INTEGER,
                sicht_strdup(buf),
                line,
                start_col
            });
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            char buf[64];
            int j = 0;
            int last_was_dot = 0;

            while (isalnum((unsigned char)src[i]) || src[i] == '_' || src[i] == '.') {
                if (src[i] == '.') {
                    char next = src[i + 1];
                    if (last_was_dot || !(isalnum((unsigned char)next) || next == '_')) {
                        lex_error(
                            line,
                            column,
                            "Invalid identifier",
                            "Dot-separated names must use non-empty segments.",
                            "Use names like module.name (not module..name or module.)."
                        );
                    }
                    last_was_dot = 1;
                } else {
                    last_was_dot = 0;
                }

                if (j >= (int)sizeof(buf) - 1) {
                    lex_error(
                        line,
                        column,
                        "Invalid identifier",
                        "Identifier is too long.",
                        "Use shorter names."
                    );
                }
                buf[j++] = src[i++];
                column++;
            }

            if (j > 0 && buf[j - 1] == '.') {
                lex_error(
                    line,
                    column,
                    "Invalid identifier",
                    "Identifier cannot end with a dot.",
                    "Use names like module.name."
                );
            }

            buf[j] = '\0';

            SichtTokenType type =
                strcmp(buf, "start") == 0      ? TOKEN_START :
                strcmp(buf, "end") == 0        ? TOKEN_END :
                strcmp(buf, "llvl") == 0       ? TOKEN_LLVL :
                strcmp(buf, "enable") == 0     ? TOKEN_ENABLE :
                strcmp(buf, "disable") == 0    ? TOKEN_DISABLE :
                strcmp(buf, "bounds") == 0     ? TOKEN_BOUNDS :
                (strcmp(buf, "check") == 0 || strcmp(buf, "checks") == 0)
                                                 ? TOKEN_CHECK :
                strcmp(buf, "pointer") == 0    ? TOKEN_POINTER :
                strcmp(buf, "endllvl") == 0    ? TOKEN_ENDLLVL :
                strcmp(buf, "if") == 0         ? TOKEN_IF :
                strcmp(buf, "try") == 0        ? TOKEN_TRY :
                strcmp(buf, "endtry") == 0     ? TOKEN_ENDTRY :
                strcmp(buf, "as") == 0         ? TOKEN_AS :
                strcmp(buf, "then") == 0       ? TOKEN_THEN :
                strcmp(buf, "otherwise") == 0  ? TOKEN_OTHERWISE :
                strcmp(buf, "endif") == 0      ? TOKEN_ENDIF :
                strcmp(buf, "while") == 0      ? TOKEN_WHILE :
                strcmp(buf, "next") == 0       ? TOKEN_NEXT :
                strcmp(buf, "continue") == 0   ? TOKEN_NEXT :
                strcmp(buf, "exit") == 0       ? TOKEN_EXIT :
                strcmp(buf, "repeat") == 0     ? TOKEN_REPEAT :
                strcmp(buf, "endrepeat") == 0  ? TOKEN_ENDREPEAT :
                strcmp(buf, "endwhile") == 0   ? TOKEN_ENDWHILE :
                strcmp(buf, "times") == 0      ? TOKEN_TIMES :
                strcmp(buf, "infinite") == 0   ? TOKEN_INFINITE :
                strcmp(buf, "set") == 0        ? TOKEN_SET :
                strcmp(buf, "element") == 0    ? TOKEN_ELEMENT :
                strcmp(buf, "to") == 0         ? TOKEN_TO :
                strcmp(buf, "print") == 0      ? TOKEN_PRINT :
                strcmp(buf, "cast") == 0       ? TOKEN_CAST :
                strcmp(buf, "input") == 0      ? TOKEN_INPUT :
                (strcmp(buf, "int") == 0 || strcmp(buf, "integer") == 0)
                                                 ? TOKEN_TYPE_INT :
                strcmp(buf, "float") == 0      ? TOKEN_TYPE_FLOAT :
                strcmp(buf, "bool") == 0       ? TOKEN_TYPE_BOOL :
                strcmp(buf, "string") == 0     ? TOKEN_TYPE_STRING :
                strcmp(buf, "is") == 0         ? TOKEN_IS :
                strcmp(buf, "not") == 0        ? TOKEN_NOT :
                strcmp(buf, "equal") == 0      ? TOKEN_EQUAL :
                strcmp(buf, "greater") == 0    ? TOKEN_GREATER :
                strcmp(buf, "less") == 0       ? TOKEN_LESS :
                strcmp(buf, "add") == 0        ? TOKEN_ADD :
                strcmp(buf, "than") == 0       ? TOKEN_THAN :
                strcmp(buf, "and") == 0        ? TOKEN_AND :
                strcmp(buf, "or") == 0         ? TOKEN_OR :
                strcmp(buf, "create") == 0     ? TOKEN_CREATE :
                strcmp(buf, "type") == 0       ? TOKEN_TYPE_KEYWORD :
                strcmp(buf, "endtype") == 0    ? TOKEN_ENDTYPE :
                strcmp(buf, "function") == 0   ? TOKEN_FUNCTION :
                strcmp(buf, "endfunction") == 0 ? TOKEN_ENDFUNCTION :
                strcmp(buf, "default") == 0    ? TOKEN_DEFAULT :
                strcmp(buf, "for") == 0        ? TOKEN_FOR :
                strcmp(buf, "each") == 0       ? TOKEN_EACH :
                strcmp(buf, "endfor") == 0     ? TOKEN_ENDFOR :
                strcmp(buf, "match") == 0      ? TOKEN_MATCH :
                strcmp(buf, "case") == 0       ? TOKEN_CASE :
                strcmp(buf, "endmatch") == 0   ? TOKEN_ENDMATCH :
                strcmp(buf, "load") == 0       ? TOKEN_LOAD :
                strcmp(buf, "library") == 0    ? TOKEN_LIBRARY :
                strcmp(buf, "read") == 0       ? TOKEN_READ_KEYWORD :
                strcmp(buf, "write") == 0      ? TOKEN_WRITE_KEYWORD :
                strcmp(buf, "append") == 0     ? TOKEN_APPEND :
                strcmp(buf, "file") == 0       ? TOKEN_FILE :
                strcmp(buf, "offer") == 0      ? TOKEN_OFFER :
                strcmp(buf, "take") == 0       ? TOKEN_TAKE :
                strcmp(buf, "yield") == 0      ? TOKEN_YIELD :
                strcmp(buf, "save") == 0       ? TOKEN_SAVE :
                strcmp(buf, "grow") == 0       ? TOKEN_GROW :
                strcmp(buf, "shrink") == 0     ? TOKEN_SHRINK :
                strcmp(buf, "copy") == 0       ? TOKEN_COPY :
                strcmp(buf, "move") == 0       ? TOKEN_MOVE :
                strcmp(buf, "resize") == 0     ? TOKEN_RESIZE :
                strcmp(buf, "memory") == 0     ? TOKEN_MEMORY :
                strcmp(buf, "value") == 0      ? TOKEN_VALUE :
                (strcmp(buf, "byte") == 0 || strcmp(buf, "bytes") == 0)
                                                 ? TOKEN_BYTE :
                strcmp(buf, "bit") == 0        ? TOKEN_BIT :
                strcmp(buf, "field") == 0      ? TOKEN_FIELD :
                strcmp(buf, "flip") == 0       ? TOKEN_FLIP :
                strcmp(buf, "place") == 0      ? TOKEN_PLACE :
                strcmp(buf, "at") == 0         ? TOKEN_AT :
                strcmp(buf, "send") == 0       ? TOKEN_SEND :
                strcmp(buf, "pin") == 0        ? TOKEN_PIN :
                strcmp(buf, "port") == 0       ? TOKEN_PORT :
                strcmp(buf, "wait") == 0       ? TOKEN_WAIT :
                strcmp(buf, "register") == 0   ? TOKEN_REGISTER :
                strcmp(buf, "interrupt") == 0  ? TOKEN_INTERRUPT :
                strcmp(buf, "handler") == 0    ? TOKEN_HANDLER :
                strcmp(buf, "atomic") == 0     ? TOKEN_ATOMIC :
                strcmp(buf, "mark") == 0       ? TOKEN_MARK :
                strcmp(buf, "volatile") == 0   ? TOKEN_VOLATILE :
                strcmp(buf, "subtract") == 0   ? TOKEN_SUBTRACT :
                strcmp(buf, "shift") == 0      ? TOKEN_SHIFT :
                strcmp(buf, "left") == 0       ? TOKEN_LEFT :
                strcmp(buf, "right") == 0      ? TOKEN_RIGHT :
                strcmp(buf, "by") == 0         ? TOKEN_BY :
                strcmp(buf, "struct") == 0     ? TOKEN_STRUCT :
                strcmp(buf, "endstruct") == 0  ? TOKEN_ENDSTRUCT :
                strcmp(buf, "union") == 0      ? TOKEN_UNION :
                strcmp(buf, "endunion") == 0   ? TOKEN_ENDUNION :
                strcmp(buf, "enum") == 0       ? TOKEN_ENUM :
                strcmp(buf, "endenum") == 0    ? TOKEN_ENDENUM :
                strcmp(buf, "bitfield") == 0   ? TOKEN_BITFIELD :
                strcmp(buf, "endbitfield") == 0 ? TOKEN_ENDBITFIELD :
                strcmp(buf, "xor") == 0        ? TOKEN_XOR :
                (strcmp(buf, "return") == 0 || strcmp(buf, "give") == 0)
                                                 ? TOKEN_RETURN :
                strcmp(buf, "remove") == 0     ? TOKEN_REMOVE :
                strcmp(buf, "from") == 0       ? TOKEN_FROM :
                strcmp(buf, "contains") == 0   ? TOKEN_CONTAINS :
                strcmp(buf, "index") == 0      ? TOKEN_INDEX :
                strcmp(buf, "in") == 0         ? TOKEN_IN :
                strcmp(buf, "sort") == 0       ? TOKEN_SORT :
                strcmp(buf, "reverse") == 0    ? TOKEN_REVERSE :
                strcmp(buf, "dictionary") == 0 ? TOKEN_DICTIONARY :
                strcmp(buf, "get") == 0        ? TOKEN_GET :
                strcmp(buf, "item") == 0       ? TOKEN_ITEM :
                strcmp(buf, "clear") == 0      ? TOKEN_CLEAR :
                strcmp(buf, "uppercase") == 0  ? TOKEN_UPPERCASE :
                strcmp(buf, "lowercase") == 0  ? TOKEN_LOWERCASE :
                strcmp(buf, "trim") == 0       ? TOKEN_TRIM :
                strcmp(buf, "character") == 0  ? TOKEN_CHARACTER :
                strcmp(buf, "length") == 0     ? TOKEN_LENGTH :
                strcmp(buf, "of") == 0         ? TOKEN_OF :
                (strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0 ||
                 strcmp(buf, "on") == 0 || strcmp(buf, "off") == 0)
                                                 ? TOKEN_BOOLEAN
                                                 : TOKEN_IDENTIFIER;

            const char* out_value = buf;
            if (type == TOKEN_BOOLEAN) {
                if (strcmp(buf, "on") == 0)
                    out_value = "true";
                else if (strcmp(buf, "off") == 0)
                    out_value = "false";
            }

            lexer_push(&st, (Token){
                type,
                sicht_strdup(out_value),
                line,
                start_col
            });
            continue;
        }

        if (c == '"') {
            i++;
            column++;

            char buf[256];
            int j = 0;

            while (src[i] && src[i] != '"') {
                if (src[i] == '\\') {
                    char next = src[i + 1];
                    char out_ch = 0;
                    if (next == '"')
                        out_ch = '"';
                    else if (next == '\\')
                        out_ch = '\\';
                    else if (next == 'n')
                        out_ch = '\n';
                    else if (next == 'r')
                        out_ch = '\r';
                    else if (next == 't')
                        out_ch = '\t';

                    if (out_ch) {
                        if (j >= (int)sizeof(buf) - 1) {
                            lex_error(
                                line,
                                column,
                                "String too long",
                                "String literal is too long.",
                                "Use shorter strings."
                            );
                        }
                        buf[j++] = out_ch;
                        i += 2;
                        column += 2;
                        continue;
                    }
                }
                if (src[i] == '\n') {
                    lex_error(
                        line,
                        column,
                        "Unterminated string",
                        "Strings cannot span multiple lines.",
                        "Close the string with a quote (\")."
                    );
                }
                if (j >= (int)sizeof(buf) - 1) {
                    lex_error(
                        line,
                        column,
                        "String too long",
                        "String literal is too long.",
                        "Use shorter strings."
                    );
                }
                buf[j++] = src[i++];
                column++;
            }

            if (src[i] != '"') {
                lex_error(
                    line,
                    column,
                    "Unterminated string",
                    "The string was never closed.",
                    "Add a closing quote (\")."
                );
            }

            i++;
            column++;
            buf[j] = '\0';

            lexer_push(&st, (Token){
                TOKEN_STRING,
                sicht_strdup(buf),
                line,
                start_col
            });
            continue;
        }

        switch (c) {
            case '+': lexer_push(&st, (Token){ TOKEN_PLUS, NULL, line, column }); break;
            case '-': lexer_push(&st, (Token){ TOKEN_MINUS, NULL, line, column }); break;
            case '*': lexer_push(&st, (Token){ TOKEN_STAR, NULL, line, column }); break;
            case '/': lexer_push(&st, (Token){ TOKEN_SLASH, NULL, line, column }); break;
            case '[': lexer_push(&st, (Token){ TOKEN_LBRACKET, NULL, line, column }); break;
            case ']': lexer_push(&st, (Token){ TOKEN_RBRACKET, NULL, line, column }); break;
            case '{': lexer_push(&st, (Token){ TOKEN_LBRACE, NULL, line, column }); break;
            case '}': lexer_push(&st, (Token){ TOKEN_RBRACE, NULL, line, column }); break;
            case ':': lexer_push(&st, (Token){ TOKEN_COLON, NULL, line, column }); break;
            case ',': lexer_push(&st, (Token){ TOKEN_COMMA, NULL, line, column }); break;
            case '(': lexer_push(&st, (Token){ TOKEN_LPAREN, NULL, line, column }); break;
            case ')': lexer_push(&st, (Token){ TOKEN_RPAREN, NULL, line, column }); break;
            default:
                lex_error(
                    line,
                    column,
                    "Invalid character",
                    "This character is not part of the language.",
                    "Remove it or replace it with valid syntax."
                );
        }

        i++;
        column++;
    }

    lexer_push(&st, (Token){ TOKEN_EOF, NULL, line, column });
    *out_count = st.count;
    return st.tokens;
}

void lex_free(Token* toks, int count) {
    if (!toks)
        return;

    for (int i = 0; i < count; i++)
        free(toks[i].value);

    free(toks);
}

const char* token_type_name(SichtTokenType type) {
    switch (type) {
        case TOKEN_INTEGER: return "TOKEN_INTEGER";
        case TOKEN_FLOAT: return "TOKEN_FLOAT";
        case TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TOKEN_STRING: return "TOKEN_STRING";
        case TOKEN_BOOLEAN: return "TOKEN_BOOLEAN";
        case TOKEN_IF: return "TOKEN_IF";
        case TOKEN_TRY: return "TOKEN_TRY";
        case TOKEN_ENDTRY: return "TOKEN_ENDTRY";
        case TOKEN_AS: return "TOKEN_AS";
        case TOKEN_OTHERWISE: return "TOKEN_OTHERWISE";
        case TOKEN_ENDIF: return "TOKEN_ENDIF";
        case TOKEN_AND: return "TOKEN_AND";
        case TOKEN_PLUS: return "TOKEN_PLUS";
        case TOKEN_MINUS: return "TOKEN_MINUS";
        case TOKEN_STAR: return "TOKEN_STAR";
        case TOKEN_SLASH: return "TOKEN_SLASH";
        case TOKEN_THEN: return "TOKEN_THEN";
        case TOKEN_IS: return "TOKEN_IS";
        case TOKEN_NOT: return "TOKEN_NOT";
        case TOKEN_EQUAL: return "TOKEN_EQUAL";
        case TOKEN_OR: return "TOKEN_OR";
        case TOKEN_GREATER: return "TOKEN_GREATER";
        case TOKEN_LESS: return "TOKEN_LESS";
        case TOKEN_THAN: return "TOKEN_THAN";
        case TOKEN_WHILE: return "TOKEN_WHILE";
        case TOKEN_REPEAT: return "TOKEN_REPEAT";
        case TOKEN_ENDWHILE: return "TOKEN_ENDWHILE";
        case TOKEN_TIMES: return "TOKEN_TIMES";
        case TOKEN_LBRACKET: return "TOKEN_LBRACKET";
        case TOKEN_RBRACKET: return "TOKEN_RBRACKET";
        case TOKEN_COMMA: return "TOKEN_COMMA";
        case TOKEN_INFINITE: return "TOKEN_INFINITE";
        case TOKEN_LPAREN: return "TOKEN_LPAREN";
        case TOKEN_RPAREN: return "TOKEN_RPAREN";
        case TOKEN_TYPE_STRING: return "TOKEN_TYPE_STRING";
        case TOKEN_TYPE_BOOL: return "TOKEN_TYPE_BOOL";
        case TOKEN_TYPE_INT: return "TOKEN_TYPE_INT";
        case TOKEN_TYPE_FLOAT: return "TOKEN_TYPE_FLOAT";
        case TOKEN_CAST: return "TOKEN_CAST";
        case TOKEN_SET: return "TOKEN_SET";
        case TOKEN_TO: return "TOKEN_TO";
        case TOKEN_PRINT: return "TOKEN_PRINT";
        case TOKEN_START: return "TOKEN_START";
        case TOKEN_END: return "TOKEN_END";
        case TOKEN_LLVL: return "TOKEN_LLVL";
        case TOKEN_ENDLLVL: return "TOKEN_ENDLLVL";
        case TOKEN_ENABLE: return "TOKEN_ENABLE";
        case TOKEN_DISABLE: return "TOKEN_DISABLE";
        case TOKEN_BOUNDS: return "TOKEN_BOUNDS";
        case TOKEN_CHECK: return "TOKEN_CHECK";
        case TOKEN_POINTER: return "TOKEN_POINTER";
        case TOKEN_INPUT: return "TOKEN_INPUT";
        case TOKEN_UPPERCASE: return "TOKEN_UPPERCASE";
        case TOKEN_LOWERCASE: return "TOKEN_LOWERCASE";
        case TOKEN_TRIM: return "TOKEN_TRIM";
        case TOKEN_CHARACTER: return "TOKEN_CHARACTER";
        case TOKEN_LENGTH: return "TOKEN_LENGTH";
        case TOKEN_OF: return "TOKEN_OF";
        case TOKEN_ENDREPEAT: return "TOKEN_ENDREPEAT";
        case TOKEN_NEXT: return "TOKEN_NEXT";
        case TOKEN_EXIT: return "TOKEN_EXIT";
        case TOKEN_ELEMENT: return "TOKEN_ELEMENT";
        case TOKEN_ADD: return "TOKEN_ADD";
        case TOKEN_REMOVE: return "TOKEN_REMOVE";
        case TOKEN_FROM: return "TOKEN_FROM";
        case TOKEN_CONTAINS: return "TOKEN_CONTAINS";
        case TOKEN_CLEAR: return "TOKEN_CLEAR";
        case TOKEN_INDEX: return "TOKEN_INDEX";
        case TOKEN_IN: return "TOKEN_IN";
        case TOKEN_SORT: return "TOKEN_SORT";
        case TOKEN_REVERSE: return "TOKEN_REVERSE";
        case TOKEN_DICTIONARY: return "TOKEN_DICTIONARY";
        case TOKEN_LBRACE: return "TOKEN_LBRACE";
        case TOKEN_RBRACE: return "TOKEN_RBRACE";
        case TOKEN_COLON: return "TOKEN_COLON";
        case TOKEN_GET: return "TOKEN_GET";
        case TOKEN_ITEM: return "TOKEN_ITEM";
        case TOKEN_CREATE: return "TOKEN_CREATE";
        case TOKEN_TYPE_KEYWORD: return "TOKEN_TYPE_KEYWORD";
        case TOKEN_ENDTYPE: return "TOKEN_ENDTYPE";
        case TOKEN_FUNCTION: return "TOKEN_FUNCTION";
        case TOKEN_ENDFUNCTION: return "TOKEN_ENDFUNCTION";
        case TOKEN_DEFAULT: return "TOKEN_DEFAULT";
        case TOKEN_FOR: return "TOKEN_FOR";
        case TOKEN_EACH: return "TOKEN_EACH";
        case TOKEN_ENDFOR: return "TOKEN_ENDFOR";
        case TOKEN_MATCH: return "TOKEN_MATCH";
        case TOKEN_CASE: return "TOKEN_CASE";
        case TOKEN_ENDMATCH: return "TOKEN_ENDMATCH";
        case TOKEN_LOAD: return "TOKEN_LOAD";
        case TOKEN_LIBRARY: return "TOKEN_LIBRARY";
        case TOKEN_READ_KEYWORD: return "TOKEN_READ_KEYWORD";
        case TOKEN_WRITE_KEYWORD: return "TOKEN_WRITE_KEYWORD";
        case TOKEN_APPEND: return "TOKEN_APPEND";
        case TOKEN_FILE: return "TOKEN_FILE";
        case TOKEN_OFFER: return "TOKEN_OFFER";
        case TOKEN_TAKE: return "TOKEN_TAKE";
        case TOKEN_RETURN: return "TOKEN_RETURN";
        case TOKEN_YIELD: return "TOKEN_YIELD";
        case TOKEN_SAVE: return "TOKEN_SAVE";
        case TOKEN_GROW: return "TOKEN_GROW";
        case TOKEN_SHRINK: return "TOKEN_SHRINK";
        case TOKEN_COPY: return "TOKEN_COPY";
        case TOKEN_MOVE: return "TOKEN_MOVE";
        case TOKEN_RESIZE: return "TOKEN_RESIZE";
        case TOKEN_MEMORY: return "TOKEN_MEMORY";
        case TOKEN_VALUE: return "TOKEN_VALUE";
        case TOKEN_BYTE: return "TOKEN_BYTE";
        case TOKEN_BIT: return "TOKEN_BIT";
        case TOKEN_FIELD: return "TOKEN_FIELD";
        case TOKEN_FLIP: return "TOKEN_FLIP";
        case TOKEN_PLACE: return "TOKEN_PLACE";
        case TOKEN_AT: return "TOKEN_AT";
        case TOKEN_SEND: return "TOKEN_SEND";
        case TOKEN_PIN: return "TOKEN_PIN";
        case TOKEN_PORT: return "TOKEN_PORT";
        case TOKEN_WAIT: return "TOKEN_WAIT";
        case TOKEN_REGISTER: return "TOKEN_REGISTER";
        case TOKEN_INTERRUPT: return "TOKEN_INTERRUPT";
        case TOKEN_HANDLER: return "TOKEN_HANDLER";
        case TOKEN_ATOMIC: return "TOKEN_ATOMIC";
        case TOKEN_MARK: return "TOKEN_MARK";
        case TOKEN_VOLATILE: return "TOKEN_VOLATILE";
        case TOKEN_SUBTRACT: return "TOKEN_SUBTRACT";
        case TOKEN_SHIFT: return "TOKEN_SHIFT";
        case TOKEN_LEFT: return "TOKEN_LEFT";
        case TOKEN_RIGHT: return "TOKEN_RIGHT";
        case TOKEN_BY: return "TOKEN_BY";
        case TOKEN_STRUCT: return "TOKEN_STRUCT";
        case TOKEN_ENDSTRUCT: return "TOKEN_ENDSTRUCT";
        case TOKEN_UNION: return "TOKEN_UNION";
        case TOKEN_ENDUNION: return "TOKEN_ENDUNION";
        case TOKEN_ENUM: return "TOKEN_ENUM";
        case TOKEN_ENDENUM: return "TOKEN_ENDENUM";
        case TOKEN_BITFIELD: return "TOKEN_BITFIELD";
        case TOKEN_ENDBITFIELD: return "TOKEN_ENDBITFIELD";
        case TOKEN_XOR: return "TOKEN_XOR";
        case TOKEN_EOF: return "TOKEN_EOF";
    }
    return "TOKEN_UNKNOWN";
}



