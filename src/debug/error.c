#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "source.h"

static void print_caret(int col) {
    int safe_col = col < 1 ? 1 : col;
    for (int i = 1; i < safe_col; i++)
        fputc(' ', stderr);
    fputs("^\n", stderr);
}

static const char* fallback_fix_hint(const char* stage) {
    if (stage && strcmp(stage, "Lexer") == 0)
        return "Use only valid Sicht characters and close strings with quotes.";
    if (stage && strcmp(stage, "Parser") == 0)
        return "Follow the documented statement pattern and close all blocks (endif/endwhile/endfunction/...).";
    return "Check the surrounding code and apply the expected syntax pattern.";
}

static int should_exit_for_stage(const char* stage) {
    if (stage && strcmp(stage, "Parser") == 0)
        return 0;
    return 1;
}

void error_report(
    const char* stage,
    int line,
    int column,
    const char* title,
    const char* message,
    const char* hint
) {
    const char* used_stage = stage && stage[0] ? stage : "Unknown";
    const char* used_title = title && title[0] ? title : "Error";
    const char* used_message = message && message[0] ? message : "No additional details were provided.";
    const char* used_hint = (hint && hint[0]) ? hint : fallback_fix_hint(stage);
    const char* source_label = source_get_label();

    const char* src_line = source_get_line(line);
    fprintf(stderr, "\n[%s Error]\n", used_stage);
    if (line > 0 && column > 0) {
        if (source_label && source_label[0]) {
            fprintf(stderr, "Location: %s:%d:%d\n", source_label, line, column);
        } else {
            fprintf(stderr, "Location: %d:%d\n", line, column);
        }
    }
    fprintf(stderr, "Problem: %s\n", used_title);

    if (line > 0 && column > 0 && src_line && src_line[0]) {
        fprintf(stderr, "\nSource:\n");
        fprintf(stderr, "  %s\n  ", src_line);
        print_caret(column);
    }

    fprintf(stderr, "\nWhat Happened:\n%s\n", used_message);
    fprintf(stderr, "\nHow To Fix:\n%s\n\n", used_hint);

    if (should_exit_for_stage(used_stage))
        exit(1);
}

