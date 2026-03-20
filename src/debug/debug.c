#include <stdio.h>
#include <stdlib.h>
#include "debug.h"

void debug_error(
    ErrorCategory category,
    const char* title,
    const char* message,
    const char* hint
) {
    (void)category;
    fprintf(stderr, "\n %s\n", title);
    fprintf(stderr, "   %s\n", message);

    if (hint)
        fprintf(stderr, "\n   ðŸ’¡ %s\n", hint);

    exit(1);
}

