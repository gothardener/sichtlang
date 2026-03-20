#ifndef SICHT_DEBUGGER_H
#define SICHT_DEBUGGER_H

int debugger_start(int port);
void debugger_shutdown(void);
int debugger_is_enabled(void);
void debugger_on_statement(
    const char* file,
    int line,
    int column,
    const char* stack_json,
    const char* locals_json
);
void debugger_notify_terminated(int exit_code);

#endif
