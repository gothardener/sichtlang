#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>

#include "debugger.h"
#include "source.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define DBG_MAX_BREAKPOINTS 2048
#define DBG_LINE_MAX 4096

typedef struct {
    char* file;
    int line;
} DebugBreakpoint;

static DebugBreakpoint breakpoints[DBG_MAX_BREAKPOINTS];
static int breakpoint_count = 0;

static int debug_enabled = 0;
static int debug_step_mode = 0;
static int debug_stop_on_entry = 1;

#ifdef _WIN32
static SOCKET dbg_server = INVALID_SOCKET;
static SOCKET dbg_client = INVALID_SOCKET;
#else
static int dbg_server = -1;
static int dbg_client = -1;
#endif

static void free_breakpoints(void) {
    for (int i = 0; i < breakpoint_count; i++) {
        free(breakpoints[i].file);
        breakpoints[i].file = NULL;
    }
    breakpoint_count = 0;
}

static char* normalize_path_dup(const char* in) {
    if (!in)
        in = "";
    size_t len = strlen(in);
    if (len > SIZE_MAX - 1)
        return NULL;
    char* out = (char*)malloc(len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '\\')
            c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
        out[i] = c;
    }
    out[len] = '\0';
    return out;
}

static int breakpoint_matches(const char* file, int line) {
    if (!file || !file[0] || line <= 0)
        return 0;
    char* norm_file = normalize_path_dup(file);
    if (!norm_file)
        return 0;
    int matched = 0;
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].line == line && breakpoints[i].file &&
            strcmp(breakpoints[i].file, norm_file) == 0) {
            matched = 1;
            break;
        }
    }
    free(norm_file);
    return matched;
}

static int send_line(const char* text) {
    if (!text) return 0;
    size_t len = strlen(text);
    if (len == 0) return 1;
#ifdef _WIN32
    if (dbg_client == INVALID_SOCKET) return 0;
    size_t offset = 0;
    while (offset < len) {
        int sent = send(dbg_client, text + offset, (int)(len - offset), 0);
        if (sent == SOCKET_ERROR || sent <= 0) {
            debugger_shutdown();
            return 0;
        }
        offset += (size_t)sent;
    }
#else
    if (dbg_client < 0) return 0;
    size_t offset = 0;
    while (offset < len) {
        ssize_t sent = send(dbg_client, text + offset, len - offset, 0);
        if (sent <= 0) {
            debugger_shutdown();
            return 0;
        }
        offset += (size_t)sent;
    }
#endif
    return 1;
}

static int send_json_event(const char* fmt, ...) {
    char buffer[DBG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n <= 0 || (size_t)n >= sizeof(buffer)) return 0;
    strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
    return send_line(buffer);
}

static int recv_line(char* buffer, size_t size) {
    if (!buffer || size == 0) return 0;
    buffer[0] = '\0';
    size_t pos = 0;
    int truncated = 0;
    for (;;) {
        char c = '\0';
#ifdef _WIN32
        if (dbg_client == INVALID_SOCKET) return 0;
        int got = recv(dbg_client, &c, 1, 0);
        if (got <= 0) {
            debugger_shutdown();
            return 0;
        }
#else
        if (dbg_client < 0) return 0;
        ssize_t got = recv(dbg_client, &c, 1, 0);
        if (got <= 0) {
            debugger_shutdown();
            return 0;
        }
#endif
        if (c == '\n') break;
        if (c == '\r') continue;
        if (pos + 1 < size) {
            buffer[pos++] = c;
        } else {
            truncated = 1;
        }
    }
    buffer[pos] = '\0';
    if (truncated)
        return 0;
    return pos > 0;
}

static int json_get_string(const char* line, const char* key, char* out, size_t out_size) {
    if (!line || !key || !out || out_size == 0) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* at = strstr(line, needle);
    if (!at) return 0;
    at = strchr(at + strlen(needle), ':');
    if (!at) return 0;
    at++;
    while (*at && isspace((unsigned char)*at)) at++;
    if (*at != '"') return 0;
    at++;
    size_t pos = 0;
    while (*at && *at != '"' && pos + 1 < out_size) {
        if (*at == '\\' && at[1]) {
            at++;
            switch (*at) {
                case '"': out[pos++] = '"'; break;
                case '\\': out[pos++] = '\\'; break;
                case '/': out[pos++] = '/'; break;
                case 'n': out[pos++] = '\n'; break;
                case 'r': out[pos++] = '\r'; break;
                case 't': out[pos++] = '\t'; break;
                case 'b': out[pos++] = '\b'; break;
                case 'f': out[pos++] = '\f'; break;
                default: out[pos++] = *at; break;
            }
            at++;
            continue;
        }
        out[pos++] = *at++;
    }
    out[pos] = '\0';
    return pos > 0;
}

static int json_get_lines(const char* line, int* out, int max_out) {
    if (!line || !out || max_out <= 0) return 0;
    const char* at = strstr(line, "\"lines\"");
    if (!at) return 0;
    at = strchr(at, '[');
    if (!at) return 0;
    at++;
    int count = 0;
    while (*at && *at != ']') {
        while (*at && !isdigit((unsigned char)*at) && *at != '-' && *at != ']') at++;
        if (*at == ']') break;
        char* endptr = NULL;
        long v = strtol(at, &endptr, 10);
        if (endptr == at) break;
        if (v < INT_MIN || v > INT_MAX) {
            at = endptr;
            continue;
        }
        if (count < max_out) out[count++] = (int)v;
        at = endptr;
    }
    return count;
}

static void debugger_set_breakpoints_internal(const char* file, const int* lines, int count) {
    if (!file || !file[0])
        return;
    char* norm_file = normalize_path_dup(file);
    if (!norm_file)
        return;

    for (int i = 0; i < breakpoint_count; ) {
        if (breakpoints[i].file && strcmp(breakpoints[i].file, norm_file) == 0) {
            free(breakpoints[i].file);
            for (int j = i; j < breakpoint_count - 1; j++)
                breakpoints[j] = breakpoints[j + 1];
            breakpoint_count--;
            continue;
        }
        i++;
    }

    if (!lines || count <= 0) {
        free(norm_file);
        return;
    }

    int dropped = 0;
    for (int i = 0; i < count; i++) {
        if (lines[i] <= 0)
            continue;
        if (breakpoint_count >= DBG_MAX_BREAKPOINTS) {
            dropped++;
            continue;
        }
        if (breakpoint_matches(norm_file, lines[i]))
            continue;
        {
            size_t len = strlen(norm_file);
            if (len > SIZE_MAX - 1)
                continue;
            len += 1;
            char* dup = (char*)malloc(len);
            if (!dup)
                continue;
            memcpy(dup, norm_file, len);
            breakpoints[breakpoint_count].file = dup;
        }
        breakpoints[breakpoint_count].line = lines[i];
        breakpoint_count++;
    }

    if (dropped > 0) {
        if (debug_enabled) {
            send_json_event(
                "{\"event\":\"warning\",\"message\":\"Breakpoint limit reached; %d breakpoint(s) dropped.\"}",
                dropped
            );
        }
        fprintf(stderr, "Debugger: breakpoint limit reached; %d breakpoint(s) dropped.\n", dropped);
    }

    free(norm_file);
}

static void handle_debug_command(const char* line) {
    char cmd[64];
    if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) return;

    if (strcmp(cmd, "set_breakpoints") == 0) {
        char file[1024];
        int lines[512];
        int line_count = 0;
        if (!json_get_string(line, "file", file, sizeof(file))) return;
        line_count = json_get_lines(line, lines, (int)(sizeof(lines) / sizeof(lines[0])));
        debugger_set_breakpoints_internal(file, lines, line_count);
    }
}

static void wait_for_debug_command(void) {
    char line[DBG_LINE_MAX];
    for (;;) {
        if (!recv_line(line, sizeof(line))) {
            debug_enabled = 0;
            return;
        }
        char cmd[64];
        if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) {
            continue;
        }
        if (strcmp(cmd, "continue") == 0) {
            return;
        }
        if (strcmp(cmd, "step") == 0 || strcmp(cmd, "next") == 0) {
            debug_step_mode = 1;
            return;
        }
        if (strcmp(cmd, "disconnect") == 0) {
            debug_enabled = 0;
            return;
        }
        if (strcmp(cmd, "set_breakpoints") == 0) {
            handle_debug_command(line);
            continue;
        }
    }
}

int debugger_start(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif

#ifdef _WIN32
    dbg_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (dbg_server == INVALID_SOCKET) return 0;
#else
    dbg_server = socket(AF_INET, SOCK_STREAM, 0);
    if (dbg_server < 0) return 0;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    int opt = 1;
#ifdef _WIN32
    setsockopt(dbg_server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(dbg_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (bind(dbg_server, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        debugger_shutdown();
        return 0;
    }

    if (listen(dbg_server, 1) != 0) {
        debugger_shutdown();
        return 0;
    }

#ifdef _WIN32
    dbg_client = accept(dbg_server, NULL, NULL);
    if (dbg_client == INVALID_SOCKET) {
        debugger_shutdown();
        return 0;
    }
#else
    dbg_client = accept(dbg_server, NULL, NULL);
    if (dbg_client < 0) {
        debugger_shutdown();
        return 0;
    }
#endif

    debug_enabled = 1;
    debug_step_mode = 0;
    debug_stop_on_entry = 1;
    free_breakpoints();

    send_json_event("{\"event\":\"ready\"}");
    return 1;
}

void debugger_shutdown(void) {
    free_breakpoints();
    debug_enabled = 0;
    debug_step_mode = 0;
    debug_stop_on_entry = 0;

#ifdef _WIN32
    if (dbg_client != INVALID_SOCKET) {
        closesocket(dbg_client);
        dbg_client = INVALID_SOCKET;
    }
    if (dbg_server != INVALID_SOCKET) {
        closesocket(dbg_server);
        dbg_server = INVALID_SOCKET;
    }
    WSACleanup();
#else
    if (dbg_client >= 0) {
        close(dbg_client);
        dbg_client = -1;
    }
    if (dbg_server >= 0) {
        close(dbg_server);
        dbg_server = -1;
    }
#endif
}

int debugger_is_enabled(void) {
    return debug_enabled != 0;
}

void debugger_on_statement(
    const char* file,
    int line,
    int column,
    const char* stack_json,
    const char* locals_json
) {
    if (!debug_enabled) return;
    if (line <= 0) return;

    int should_stop = 0;
    const char* reason = "breakpoint";
    if (debug_stop_on_entry) {
        should_stop = 1;
        reason = "entry";
        debug_stop_on_entry = 0;
    } else if (debug_step_mode) {
        should_stop = 1;
        reason = "step";
        debug_step_mode = 0;
    } else if (breakpoint_matches(file, line)) {
        should_stop = 1;
        reason = "breakpoint";
    }

    if (!should_stop) return;

    char* norm_file = normalize_path_dup(file ? file : "");
    if (!norm_file) {
        norm_file = (char*)malloc(1);
        if (norm_file)
            norm_file[0] = '\0';
    }
    send_json_event(
        "{\"event\":\"stopped\",\"reason\":\"%s\",\"file\":\"%s\",\"line\":%d,\"column\":%d,\"stack\":%s,\"locals\":%s}",
        reason,
        norm_file ? norm_file : "",
        line,
        column,
        (stack_json && stack_json[0]) ? stack_json : "[]",
        (locals_json && locals_json[0]) ? locals_json : "[]"
    );

    wait_for_debug_command();
    if (norm_file)
        free(norm_file);
}

void debugger_notify_terminated(int exit_code) {
    if (!debug_enabled) return;
    send_json_event("{\"event\":\"terminated\",\"exitCode\":%d}", exit_code);
}

