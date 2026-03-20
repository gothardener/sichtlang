#ifndef _WIN32
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef void* SichtHMODULE;
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(
    SichtHMODULE hModule,
    char* lpFilename,
    unsigned long nSize
);
__declspec(dllimport) unsigned long __stdcall GetFullPathNameA(
    const char* lpFileName,
    unsigned long nBufferLength,
    char* lpBuffer,
    char** lpFilePart
);
#endif

#include "arena.h"
#include "ast.h"
#include "gc.h"
#include "interpreter.h"
#include "lexer.h"
#include "native_rt.h"
#include "parser.h"
#include "llvm_backend.h"
#include "windows_autoinstall.h"
#include "debugger.h"
#include "compat.h"
#include "error.h"
#include "source.h"

static char* g_source = NULL;
static Token* g_tokens = NULL;
static int g_token_count = 0;
static const char* g_sicht_version = "v1.0.0";
static const char* g_sicht_api_version = "1.0";
static const char* g_program_path = NULL;

typedef enum {
    CLI_LEGACY = 0,
    CLI_RUN,
    CLI_COMPILE,
    CLI_CHECK,
    CLI_REPL,
    CLI_DEBUG
} CliMode;

static const char* REPL_KEYWORDS[] = {
    "set", "print", "if", "otherwise", "endif", "try", "endtry",
    "while", "endwhile", "repeat", "times", "endrepeat",
    "for each", "endfor", "match", "case", "endmatch",
    "create function", "endfunction", "return", "yield",
    "create type", "endtype",
    "create library", "load library", "load file", "offer", "take",
    "add", "remove", "clear", "cast", "next from",
    "write file", "append file", "read file",
    "start", "end"
};

static const char* repl_history_path(void) {
    const char* env = getenv("SICHT_REPL_HISTORY");
    if (env && env[0])
        return env;
    return ".cache/sicht_repl_history";
}

static int starts_with_prefix(const char* text, const char* prefix) {
    if (!prefix || prefix[0] == '\0')
        return 1;
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int cmp_string_ptr(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

static int add_unique_str(const char** out, int count, int max_count, const char* value) {
    if (!value || value[0] == '\0')
        return count;

    for (int i = 0; i < count; i++) {
        if (strcmp(out[i], value) == 0)
            return count;
    }

    if (count < max_count)
        out[count++] = value;
    return count;
}

static char* default_output_path_with_extension(const char* input_path, const char* extension);
static int resolve_current_executable_path(const char* argv0, char* out, size_t out_size);
static void choose_runtime_source_root(const char* program_path, char* out, size_t out_size);

static char* sicht_strdup(const char* s) {
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "Out of memory while duplicating string.\n");
        exit(1);
    }
    len += 1;
    char* out = malloc(len);
    if (!out) {
        fprintf(stderr, "Out of memory while duplicating string.\n");
        exit(1);
    }
    memcpy(out, s, len);
    return out;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        exit(1);
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        exit(1);
    }
    long size = ftell(f);
    if (size < 0) {
        perror("ftell");
        fclose(f);
        exit(1);
    }
    if ((unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
        fprintf(stderr, "Input file is too large.\n");
        fclose(f);
        exit(1);
    }
    rewind(f);

    char* buffer = malloc((size_t)size + 1);
    if (!buffer) {
        perror("malloc");
        fclose(f);
        exit(1);
    }

    size_t read_count = fread(buffer, 1, (size_t)size, f);
    if (read_count != (size_t)size) {
        perror("fread");
        fclose(f);
        free(buffer);
        exit(1);
    }
    buffer[size] = '\0';
    if (size >= 3) {
        unsigned char b0 = (unsigned char)buffer[0];
        unsigned char b1 = (unsigned char)buffer[1];
        unsigned char b2 = (unsigned char)buffer[2];
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            memmove(buffer, buffer + 3, (size_t)size - 3);
            size -= 3;
            buffer[size] = '\0';
        }
    }

    fclose(f);
    return buffer;
}

static char* read_stdin(void) {
    size_t cap = 8192;
    size_t len = 0;
    char* buffer = (char*)malloc(cap);
    if (!buffer) {
        fprintf(stderr, "Out of memory while reading stdin.\n");
        exit(1);
    }

    for (;;) {
        size_t remaining = cap - len;
        if (remaining < 4096) {
            if (cap > SIZE_MAX / 2) {
                free(buffer);
                fprintf(stderr, "Out of memory while reading stdin.\n");
                exit(1);
            }
            size_t new_cap = cap * 2;
            char* grown = (char*)realloc(buffer, new_cap);
            if (!grown) {
                free(buffer);
                fprintf(stderr, "Out of memory while reading stdin.\n");
                exit(1);
            }
            buffer = grown;
            cap = new_cap;
            remaining = cap - len;
        }
        size_t got = fread(buffer + len, 1, remaining, stdin);
        if (got > 0)
            len += got;
        if (got == 0)
            break;
    }

    if (len + 1 > cap) {
        if (len > SIZE_MAX - 1) {
            free(buffer);
            fprintf(stderr, "Out of memory while finalizing stdin buffer.\n");
            exit(1);
        }
        char* grown = (char*)realloc(buffer, len + 1);
        if (!grown) {
            free(buffer);
            fprintf(stderr, "Out of memory while finalizing stdin buffer.\n");
            exit(1);
        }
        buffer = grown;
        cap = len + 1;
    }
    buffer[len] = '\0';
    if (len >= 3) {
        unsigned char b0 = (unsigned char)buffer[0];
        unsigned char b1 = (unsigned char)buffer[1];
        unsigned char b2 = (unsigned char)buffer[2];
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            memmove(buffer, buffer + 3, len - 3);
            len -= 3;
            buffer[len] = '\0';
        }
    }
    return buffer;
}

static char* read_file_soft(const char* path, char* err, size_t err_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not open `%s`.", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not seek `%s`.", path);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not read size for `%s`.", path);
        return NULL;
    }

    if ((unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
        fclose(f);
        if (err && err_size > 0)
            snprintf(err, err_size, "File `%s` is too large.", path);
        return NULL;
    }

    rewind(f);
    char* buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        if (err && err_size > 0)
            snprintf(err, err_size, "Out of memory while reading `%s`.", path);
        return NULL;
    }

    size_t read_count = fread(buffer, 1, (size_t)size, f);
    fclose(f);

    if (read_count != (size_t)size) {
        free(buffer);
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not read full content of `%s`.", path);
        return NULL;
    }

    buffer[size] = '\0';
    if (size >= 3) {
        unsigned char b0 = (unsigned char)buffer[0];
        unsigned char b1 = (unsigned char)buffer[1];
        unsigned char b2 = (unsigned char)buffer[2];
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            memmove(buffer, buffer + 3, (size_t)size - 3);
            size -= 3;
            buffer[size] = '\0';
        }
    }
    return buffer;
}

static int write_text_file_soft(const char* path, const char* text, char* err, size_t err_size) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not open `%s` for writing.", path);
        return 0;
    }

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    if (written != len) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not write full content to `%s`.", path);
        return 0;
    }

    return 1;
}

static int dir_exists(const char* path) {
    if (!path || path[0] == '\0')
        return 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}

static int file_exists(const char* path) {
    if (!path || path[0] == '\0')
        return 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISREG(st.st_mode);
}

static int path_join(char* out, size_t out_size, const char* a, const char* b) {
    if (!out || out_size == 0 || !a || !b)
        return 0;
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n > 0 && (size_t)n < out_size;
}

static int get_cwd(char* out, size_t out_size) {
    if (!out || out_size == 0)
        return 0;
#ifdef _WIN32
    return _getcwd(out, (int)out_size) != NULL;
#else
    return getcwd(out, out_size) != NULL;
#endif
}

static int dirname_from_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0)
        return 0;
    size_t len = strlen(path);
    if (len + 1 > out_size)
        return 0;
    memcpy(out, path, len + 1);

    char* slash = strrchr(out, '/');
    char* bslash = strrchr(out, '\\');
    char* cut = slash;
    if (!cut || (bslash && bslash > cut))
        cut = bslash;
    if (!cut)
        return 0;
    *cut = '\0';
    return out[0] != '\0';
}

static int basename_is(const char* path, const char* name) {
    if (!path || !name)
        return 0;
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
#ifdef _WIN32
    return _stricmp(base, name) == 0;
#else
    return strcmp(base, name) == 0;
#endif
}

static int resolve_project_entry(
    const char* input_path,
    char* out_path,
    size_t out_size,
    char* out_root,
    size_t out_root_size,
    char* err,
    size_t err_size
) {
    if (!input_path || !out_path || out_size == 0)
        return 0;

    if (!dir_exists(input_path))
        return 0;

    const char* candidates[] = { "main.si", "app.si" };
    char candidate_path[4096];
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!path_join(candidate_path, sizeof(candidate_path), input_path, candidates[i]))
            continue;
        if (file_exists(candidate_path)) {
            snprintf(out_path, out_size, "%s", candidate_path);
            if (out_root && out_root_size > 0)
                snprintf(out_root, out_root_size, "%s", input_path);
            return 1;
        }
    }

    if (err && err_size > 0) {
        snprintf(
            err,
            err_size,
            "No entry file found in `%s` (expected main.si or app.si).",
            input_path
        );
    }
    return -1;
}

static int find_comct_root_from(const char* start_dir, char* out, size_t out_size) {
    if (!start_dir || !out || out_size == 0)
        return 0;

    char cur[4096];
    char parent[4096];
    snprintf(cur, sizeof(cur), "%s", start_dir);

    for (;;) {
        if (basename_is(cur, "COMCT")) {
            char libs_dir[4096];
            if (path_join(libs_dir, sizeof(libs_dir), cur, "libs") && dir_exists(libs_dir)) {
                snprintf(out, out_size, "%s", cur);
                return 1;
            }
        }

        {
            char child[4096];
            if (path_join(child, sizeof(child), cur, "COMCT") && dir_exists(child)) {
                char libs_dir[4096];
                if (path_join(libs_dir, sizeof(libs_dir), child, "libs") && dir_exists(libs_dir)) {
                    snprintf(out, out_size, "%s", child);
                    return 1;
                }
            }
        }

        if (!dirname_from_path(cur, parent, sizeof(parent)))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }

    return 0;
}

static int find_lib_root_from(const char* start_dir, char* out, size_t out_size) {
    if (!start_dir || !out || out_size == 0)
        return 0;

    char cur[4096];
    char parent[4096];
    snprintf(cur, sizeof(cur), "%s", start_dir);

    for (;;) {
        char libs_dir[4096];
        if (path_join(libs_dir, sizeof(libs_dir), cur, "libs") && dir_exists(libs_dir)) {
            snprintf(out, out_size, "%s", cur);
            return 1;
        }

        if (!dirname_from_path(cur, parent, sizeof(parent)))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }

    return 0;
}

static int resolve_current_executable_path(const char* argv0, char* out, size_t out_size) {
    if (!out || out_size < 4u)
        return 0;
    out[0] = '\0';
#ifdef _WIN32
    unsigned long got = GetModuleFileNameA(NULL, out, (unsigned long)out_size);
    if (got > 0 && got < (unsigned long)out_size) {
        out[got] = '\0';
        return 1;
    }
    if (!argv0 || argv0[0] == '\0')
        return 0;
    return _fullpath(out, argv0, out_size) != NULL;
#else
    ssize_t got = readlink("/proc/self/exe", out, out_size - 1u);
    if (got > 0 && (size_t)got < out_size - 1u) {
        out[got] = '\0';
        return 1;
    }
    if (!argv0 || argv0[0] == '\0')
        return 0;
    return realpath(argv0, out) != NULL;
#endif
}

static int has_runtime_sources_at(const char* root) {
    if (!root || root[0] == '\0')
        return 0;

    char probe[1024];
    int n = 0;
    FILE* f = NULL;

    /* New layout: src/core + src/include */
    n = snprintf(probe, sizeof(probe), "%s/src/core/arena.c", root);
    if (n > 0 && (size_t)n < sizeof(probe)) {
        f = fopen(probe, "rb");
        if (f) {
            fclose(f);
            n = snprintf(probe, sizeof(probe), "%s/src/include/arena.h", root);
            if (n > 0 && (size_t)n < sizeof(probe)) {
                f = fopen(probe, "rb");
                if (f) {
                    fclose(f);
                    return 1;
                }
            }
        }
    }

    /* Legacy layout: src + headers */
    n = snprintf(probe, sizeof(probe), "%s/src/arena.c", root);
    if (n < 0 || (size_t)n >= sizeof(probe))
        return 0;
    f = fopen(probe, "rb");
    if (!f)
        return 0;
    fclose(f);

    n = snprintf(probe, sizeof(probe), "%s/headers/arena.h", root);
    if (n < 0 || (size_t)n >= sizeof(probe))
        return 0;
    f = fopen(probe, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void choose_runtime_source_root(const char* program_path, char* out, size_t out_size) {
    if (!out || out_size == 0u)
        return;
    out[0] = '\0';

    const char* env_root = getenv("SICHT_RUNTIME_ROOT");
    if (env_root && env_root[0] != '\0') {
        if (has_runtime_sources_at(env_root)) {
            snprintf(out, out_size, "%s", env_root);
            return;
        }
    }

    if (program_path && program_path[0] != '\0') {
        size_t len = strlen(program_path);
        while (len > 0u && program_path[len - 1u] != '/' && program_path[len - 1u] != '\\')
            len--;
        if (len > 0u && len < out_size) {
            memcpy(out, program_path, len);
            out[len] = '\0';
            while (len > 0u && (out[len - 1u] == '/' || out[len - 1u] == '\\')) {
                out[len - 1u] = '\0';
                len--;
            }
            if (has_runtime_sources_at(out))
                return;
        }
    }

    {
        char cwd[4096];
        char comct_root[4096];
        if (get_cwd(cwd, sizeof(cwd)) &&
            find_comct_root_from(cwd, comct_root, sizeof(comct_root)) &&
            has_runtime_sources_at(comct_root)) {
            snprintf(out, out_size, "%s", comct_root);
            return;
        }
    }

    if (program_path && program_path[0] != '\0') {
        char exe_dir[4096];
        char comct_root[4096];
        if (dirname_from_path(program_path, exe_dir, sizeof(exe_dir)) &&
            find_comct_root_from(exe_dir, comct_root, sizeof(comct_root)) &&
            has_runtime_sources_at(comct_root)) {
            snprintf(out, out_size, "%s", comct_root);
            return;
        }
    }

    if (has_runtime_sources_at(".")) {
        snprintf(out, out_size, ".");
        return;
    }

#ifndef _WIN32
    {
        const char* system_candidates[] = {
            "/usr/local/share/sicht",
            "/usr/share/sicht",
            "/usr/local/lib/sicht",
            "/usr/lib/sicht"
        };
        for (size_t i = 0; i < (sizeof(system_candidates) / sizeof(system_candidates[0])); i++) {
            if (has_runtime_sources_at(system_candidates[i])) {
                snprintf(out, out_size, "%s", system_candidates[i]);
                return;
            }
        }
    }
#endif

    out[0] = '\0';
}

static void maybe_set_runtime_root_env(const char* runtime_root) {
    if (!runtime_root || runtime_root[0] == '\0')
        return;
    const char* existing = getenv("SICHT_RUNTIME_ROOT");
    if (existing && existing[0] != '\0')
        return;
#ifdef _WIN32
    _putenv_s("SICHT_RUNTIME_ROOT", runtime_root);
#else
    setenv("SICHT_RUNTIME_ROOT", runtime_root, 0);
#endif
}

static void maybe_set_lib_root_env(const char* program_path, const char* runtime_root) {
    const char* existing = getenv("SICHT_LIB_ROOT");
    if (existing && existing[0] != '\0')
        return;

    char probe[4096];
    char comct_root[4096];

    if (runtime_root && runtime_root[0] != '\0') {
        if (path_join(probe, sizeof(probe), runtime_root, "libs") && dir_exists(probe)) {
#ifdef _WIN32
            _putenv_s("SICHT_LIB_ROOT", runtime_root);
#else
            setenv("SICHT_LIB_ROOT", runtime_root, 0);
#endif
            return;
        }
    }

    {
        char cwd[4096];
        if (get_cwd(cwd, sizeof(cwd)) && find_comct_root_from(cwd, comct_root, sizeof(comct_root))) {
#ifdef _WIN32
            _putenv_s("SICHT_LIB_ROOT", comct_root);
#else
            setenv("SICHT_LIB_ROOT", comct_root, 0);
#endif
            return;
        }
    }

    char exe_dir[4096];
    if (program_path && program_path[0] != '\0' &&
        dirname_from_path(program_path, exe_dir, sizeof(exe_dir))) {
        if (find_comct_root_from(exe_dir, comct_root, sizeof(comct_root))) {
#ifdef _WIN32
            _putenv_s("SICHT_LIB_ROOT", comct_root);
#else
            setenv("SICHT_LIB_ROOT", comct_root, 0);
#endif
            return;
        }
        if (path_join(probe, sizeof(probe), exe_dir, "libs") && dir_exists(probe)) {
#ifdef _WIN32
            _putenv_s("SICHT_LIB_ROOT", exe_dir);
#else
            setenv("SICHT_LIB_ROOT", exe_dir, 0);
#endif
            return;
        }
    }

    if (path_join(probe, sizeof(probe), ".", "libs") && dir_exists(probe)) {
#ifdef _WIN32
        _putenv_s("SICHT_LIB_ROOT", ".");
#else
        setenv("SICHT_LIB_ROOT", ".", 0);
#endif
    }
}

static void maybe_set_project_root_env(const char* file_path) {
    if (!file_path || file_path[0] == '\0')
        return;

    char file_dir[4096];
    if (!dirname_from_path(file_path, file_dir, sizeof(file_dir)))
        return;

    char project_root[4096];
    if (!find_lib_root_from(file_dir, project_root, sizeof(project_root))) {
#ifdef _WIN32
        _putenv_s("SICHT_PROJECT_ROOT", file_dir);
#else
        setenv("SICHT_PROJECT_ROOT", file_dir, 1);
#endif
        return;
    }

#ifdef _WIN32
    _putenv_s("SICHT_PROJECT_ROOT", project_root);
#else
    setenv("SICHT_PROJECT_ROOT", project_root, 1);
#endif
}

static void cleanup_runtime(void) {
    if (g_tokens) {
        lex_free(g_tokens, g_token_count);
        g_tokens = NULL;
        g_token_count = 0;
    }

    free(g_source);
    g_source = NULL;

    interpreter_reset();
    gc_shutdown();
    arena_free_all();
}

static void print_usage_run(void) {
    fprintf(stderr, "Usage: sicht run [--tokens] [--ast] [--trace] [--compat <version>] <file|directory>\n");
}

static void print_usage_compile(void) {
    fprintf(stderr, "Usage: sicht compile [--tokens] [--ast] [--out file] [--exe] [--compat <version>] <file|directory>\n");
    fprintf(stderr, "Note: compile builds a native executable via LLVM/clang.\n");
    fprintf(stderr, "Removed flags: `--backend`, `--plan`, `--exe-out`, `--toolchain`, `--emit-bytecode`, `--engine`.\n");
}

static void print_usage_check(void) {
    fprintf(stderr, "Usage: sicht check [--tokens] [--ast] [--compat <version>] <file|directory>\n");
}

static void print_usage_repl(void) {
    fprintf(stderr, "Usage: sicht repl [--tokens] [--ast] [--trace] [--compat <version>]\n");
}

static void print_usage_debug(void) {
    fprintf(stderr, "Usage: sicht debug [--port <number>] [--trace] [--stdin] [--label <path>] [--compat <version>] <file|directory>\n");
}

static void print_usage(void) {
    fprintf(stderr, "Sicht %s (api %s, compat %s)\n\n", g_sicht_version, g_sicht_api_version, compat_current_version());
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  sicht [file]\n");
    fprintf(stderr, "  sicht run [options] <file|directory>\n");
    fprintf(stderr, "  sicht compile [options] <file|directory>\n");
    fprintf(stderr, "  sicht check [options] <file|directory>\n");
    fprintf(stderr, "  sicht repl [options]\n");
    fprintf(stderr, "  sicht debug [options] <file|directory>\n");
    fprintf(stderr, "  sicht version\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Try `sicht <command> --help` for command-specific help.\n");
    fprintf(stderr, "Legacy mode remains supported: `sicht <file>` or `sicht`.\n");
}

static void print_usage_for_mode(CliMode mode) {
    switch (mode) {
        case CLI_RUN:
            print_usage_run();
            return;
        case CLI_COMPILE:
            print_usage_compile();
            return;
        case CLI_CHECK:
            print_usage_check();
            return;
        case CLI_REPL:
            print_usage_repl();
            return;
        case CLI_DEBUG:
            print_usage_debug();
            return;
        case CLI_LEGACY:
        default:
            print_usage();
            return;
    }
}

static void dump_tokens(const Token* tokens, int count) {
    for (int i = 0; i < count; i++) {
        const char* value = tokens[i].value ? tokens[i].value : "";
        printf("%4d  %3d:%-3d  %-20s  %s\n",
            i,
            tokens[i].line,
            tokens[i].column,
            token_type_name(tokens[i].type),
            value);
    }
}

static void run_source(
    const char* source,
    const char* source_label,
    int show_tokens,
    int show_ast,
    int trace,
    int incremental
) {
    source_set_label(source_label ? source_label : "<input>");
    source_load(source);

    int token_count = 0;
    Token* tokens = lex(source, &token_count);

    if (show_tokens)
        dump_tokens(tokens, token_count);

    ASTNode* program = parse(tokens, token_count);
    if (!program) {
        lex_free(tokens, token_count);
        arena_free_all();
        arena_init();
        exit(1);
    }

    if (show_ast)
        ast_dump(program);

    interpreter_set_trace(trace);
    if (incremental)
        execute_incremental(program);
    else
        execute(program);

    lex_free(tokens, token_count);
    arena_free_all();
    arena_init();
}

static void check_source_ast(
    const char* source,
    const char* input_path,
    int show_tokens,
    int show_ast
) {
    source_set_label(input_path ? input_path : "<check>");
    source_load(source);

    int token_count = 0;
    Token* tokens = lex(source, &token_count);

    if (show_tokens)
        dump_tokens(tokens, token_count);

    ASTNode* program = parse(tokens, token_count);
    if (!program) {
        lex_free(tokens, token_count);
        arena_free_all();
        arena_init();
        exit(1);
    }
    if (show_ast)
        ast_dump(program);

    printf("Check OK.\n");
    lex_free(tokens, token_count);
    arena_free_all();
    arena_init();
}


static int path_char_equal(char a, char b) {
    unsigned char ua = (unsigned char)a;
    unsigned char ub = (unsigned char)b;
    if (ua == '\\')
        ua = '/';
    if (ub == '\\')
        ub = '/';
    if (ua >= 'A' && ua <= 'Z')
        ua = (unsigned char)(ua - 'A' + 'a');
    if (ub >= 'A' && ub <= 'Z')
        ub = (unsigned char)(ub - 'A' + 'a');
    return ua == ub;
}

static int path_equal_loose(const char* a, const char* b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (!path_char_equal(*a, *b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int path_full_or_copy(const char* input, char* out, size_t out_size) {
    if (!input || !out || out_size == 0)
        return 0;
#ifdef _WIN32
    unsigned long got = GetFullPathNameA(input, (unsigned long)out_size, out, NULL);
    if (got == 0u || got >= (unsigned long)out_size)
        return 0;
    return 1;
#else
    char* resolved = realpath(input, NULL);
    if (!resolved)
        return 0;
    size_t len = strlen(resolved);
    if (len + 1 > out_size) {
        free(resolved);
        return 0;
    }
    memcpy(out, resolved, len + 1);
    free(resolved);
    return 1;
#endif
}

static void ensure_distinct_paths_or_error(
    const char* input_path,
    const char* output_path,
    const char* what
) {
    if (!input_path || !output_path)
        return;
    {
        char norm_in[4096];
        char norm_out[4096];
        if (path_full_or_copy(input_path, norm_in, sizeof(norm_in)) &&
            path_full_or_copy(output_path, norm_out, sizeof(norm_out))) {
            if (!path_equal_loose(norm_in, norm_out))
                return;
        } else {
            if (!path_equal_loose(input_path, output_path))
                return;
        }
    }

    char hint[256];
    snprintf(
        hint,
        sizeof(hint),
        "Choose a different output file for %s so the input source is not overwritten.",
        what ? what : "compile output"
    );
    error_report(
        "Compiler",
        0,
        0,
        "Output path conflicts with input path",
        "Input and output paths must be different.",
        hint
    );
}

static char* default_output_path_with_extension(const char* input_path, const char* extension) {
    if (!input_path || !extension) {
        fprintf(stderr, "Missing path data for output filename generation.\n");
        exit(1);
    }

    size_t len = strlen(input_path);
    const char* dot = strrchr(input_path, '.');
    size_t stem_len = dot ? (size_t)(dot - input_path) : len;
    size_t ext_len = strlen(extension);
    if (stem_len > SIZE_MAX - ext_len - 1) {
        fprintf(stderr, "Output path is too large.\n");
        exit(1);
    }
    size_t out_len = stem_len + ext_len;

    char* out = malloc(out_len + 1);
    if (!out) {
        fprintf(stderr, "Out of memory while building output path.\n");
        exit(1);
    }

    memcpy(out, input_path, stem_len);
    out[stem_len] = '\0';
    strcat(out, extension);
    return out;
}

static void compile_source_llvm_exe(
    const char* source,
    const char* input_path,
    const char* runtime_root,
    const char* exe_output_path,
    int show_tokens,
    int show_ast
) {
    source_set_label(input_path ? input_path : "<compile-llvm-exe>");
    source_load(source);

    int token_count = 0;
    Token* tokens = lex(source, &token_count);
    if (show_tokens)
        dump_tokens(tokens, token_count);

    ASTNode* program = parse(tokens, token_count);
    if (!program) {
        lex_free(tokens, token_count);
        arena_free_all();
        arena_init();
        exit(1);
    }
    if (show_ast)
        ast_dump(program);

    char err[512] = {0};
    if (!llvm_backend_build_exe_from_source(runtime_root, source, input_path, program, exe_output_path, err, sizeof(err))) {
        error_report(
            "LLVM",
            0,
            0,
            "LLVM executable build failed",
            err[0] ? err : "Could not build executable through LLVM/clang native pipeline.",
            "Ensure runtime sources exist and bundled LLVM is complete (clang + sysroot), or set SICHT_LLVM_CLANG/SICHT_LLVM_SYSROOT."
        );
    }

    printf("Compile OK (backend=llvm, native exe mode).\n");
    printf("Built executable: %s\n", exe_output_path);

    lex_free(tokens, token_count);
    arena_free_all();
    arena_init();
}

static void append_to_chunk(char** chunk, size_t* len, size_t* cap, const char* text) {
    size_t add_len = strlen(text);
    if (*len > SIZE_MAX - add_len - 1) {
        fprintf(stderr, "REPL input too large.\n");
        exit(1);
    }
    size_t needed = *len + add_len + 1;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap : 256;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) {
                fprintf(stderr, "REPL input too large.\n");
                exit(1);
            }
            new_cap *= 2;
        }
        char* grown = realloc(*chunk, new_cap);
        if (!grown) {
            perror("realloc");
            exit(1);
        }
        *chunk = grown;
        *cap = new_cap;
    }
    memcpy(*chunk + *len, text, add_len + 1);
    *len += add_len;
}

static void execute_repl_chunk(
    const char* chunk,
    int show_tokens,
    int show_ast,
    int trace
) {
    size_t chunk_len = strlen(chunk);
    if (chunk_len > SIZE_MAX - 12) {
        fprintf(stderr, "REPL input too large.\n");
        exit(1);
    }
    size_t wrapped_len = chunk_len + 12;
    char* wrapped = malloc(wrapped_len);
    if (!wrapped) {
        perror("malloc");
        exit(1);
    }

    snprintf(wrapped, wrapped_len, "start\n%send\n", chunk);
    run_source(wrapped, "<repl>", show_tokens, show_ast, trace, 1);
    free(wrapped);
}

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int starts_kw(const char* line, const char* kw) {
    size_t n = strlen(kw);
    if (strncmp(line, kw, n) != 0)
        return 0;
    return line[n] == '\0' || line[n] == ' ' || line[n] == '\t';
}

static int repl_line_depth_delta(const char* raw_line) {
    const char* line = skip_ws(raw_line);
    if (*line == '\0' || *line == '#')
        return 0;

    if (starts_kw(line, "create function") || starts_kw(line, "create type"))
        return 1;
    if (starts_kw(line, "if"))
        return 1;
    if (starts_kw(line, "try"))
        return 1;
    if (starts_kw(line, "while"))
        return 1;
    if (starts_kw(line, "repeat"))
        return 1;
    if (starts_kw(line, "for each"))
        return 1;
    if (starts_kw(line, "match"))
        return 1;

    if (starts_kw(line, "endif"))
        return -1;
    if (starts_kw(line, "endtry"))
        return -1;
    if (starts_kw(line, "endwhile"))
        return -1;
    if (starts_kw(line, "endrepeat"))
        return -1;
    if (starts_kw(line, "endfor"))
        return -1;
    if (starts_kw(line, "endmatch"))
        return -1;
    if (starts_kw(line, "endfunction"))
        return -1;
    if (starts_kw(line, "endtype"))
        return -1;

    return 0;
}

static char* trim_ws_inplace(char* s) {
    while (*s && isspace((unsigned char)*s))
        s++;

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';

    return s;
}

static char* strip_quotes_inplace(char* s) {
    size_t len = strlen(s);
    if (len >= 2) {
        if ((s[0] == '"' && s[len - 1] == '"') ||
            (s[0] == '\'' && s[len - 1] == '\'')) {
            s[len - 1] = '\0';
            return s + 1;
        }
    }
    return s;
}

static char* read_line_dynamic(FILE* f) {
    size_t cap = 256;
    size_t len = 0;
    char* line = malloc(cap);
    if (!line)
        return NULL;

    int c = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\r')
            continue;
        if (c == '\n')
            break;

        if (len + 1 >= cap) {
            if (cap > SIZE_MAX / 2) {
                free(line);
                return NULL;
            }
            cap *= 2;
            char* grown = realloc(line, cap);
            if (!grown) {
                free(line);
                return NULL;
            }
            line = grown;
        }
        line[len++] = (char)c;
    }

    if (c == EOF && len == 0) {
        free(line);
        return NULL;
    }

    line[len] = '\0';
    return line;
}

static char* history_escape(const char* text) {
    size_t len = strlen(text);
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\\' || c == '\n' || c == '\r') {
            if (out_len > SIZE_MAX - 2)
                return NULL;
            out_len += 2;
        } else {
            if (out_len > SIZE_MAX - 1)
                return NULL;
            out_len += 1;
        }
    }

    if (out_len > SIZE_MAX - 1)
        return NULL;
    char* out = malloc(out_len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\\') {
            out[j++] = '\\';
            out[j++] = '\\';
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

static char* history_unescape(const char* text) {
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1)
        return NULL;
    char* out = malloc(len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\\' && i + 1 < len) {
            i++;
            if (text[i] == 'n')
                out[j++] = '\n';
            else if (text[i] == 'r')
                out[j++] = '\r';
            else
                out[j++] = text[i];
        } else {
            out[j++] = text[i];
        }
    }

    out[j] = '\0';
    return out;
}

static void history_push(char** history, int* history_count, int history_max, const char* chunk) {
    if (!history || !history_count || !chunk || chunk[0] == '\0')
        return;

    char* copy = sicht_strdup(chunk);
    if (*history_count < history_max) {
        history[(*history_count)++] = copy;
        return;
    }

    free(history[0]);
    for (int i = 1; i < history_max; i++)
        history[i - 1] = history[i];
    history[history_max - 1] = copy;
}

static void history_load_file(const char* path, char** history, int* history_count, int history_max) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return;

    char* line = NULL;
    while ((line = read_line_dynamic(f)) != NULL) {
        if (line[0] != '\0') {
            char* decoded = history_unescape(line);
            if (decoded) {
                if (*history_count < history_max) {
                    history[(*history_count)++] = decoded;
                } else {
                    free(history[0]);
                    for (int i = 1; i < history_max; i++)
                        history[i - 1] = history[i];
                    history[history_max - 1] = decoded;
                }
            }
        }
        free(line);
    }

    fclose(f);
}

static void history_save_file(const char* path, char** history, int history_count) {
    FILE* f = fopen(path, "wb");
    if (!f)
        return;

    for (int i = 0; i < history_count; i++) {
        char* escaped = history_escape(history[i]);
        if (!escaped)
            continue;
        fputs(escaped, f);
        fputc('\n', f);
        free(escaped);
    }
    fclose(f);
}

static void ensure_history_dir(const char* path) {
    if (!path || !path[0])
        return;
    const char* slash = strrchr(path, '/');
    const char* back = strrchr(path, '\\');
    const char* sep = slash;
    if (back && (!sep || back > sep))
        sep = back;
    if (!sep)
        return;
    size_t len = (size_t)(sep - path);
    if (len == 0 || len >= 512)
        return;
    char dir[512];
    memcpy(dir, path, len);
    dir[len] = '\0';
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

static void free_arg_tokens(char** tokens, int count) {
    if (!tokens)
        return;
    for (int i = 0; i < count; i++)
        free(tokens[i]);
    free(tokens);
}

static int tokenize_arg_line(const char* input, char*** out_tokens, int* out_count, char* err, size_t err_size) {
    if (!out_tokens || !out_count) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Invalid tokenization output pointers.");
        return 0;
    }

    *out_tokens = NULL;
    *out_count = 0;
    if (!input || input[0] == '\0')
        return 1;

    int cap = 8;
    int count = 0;
    char** tokens = malloc((size_t)cap * sizeof(char*));
    if (!tokens) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Out of memory while tokenizing command arguments.");
        return 0;
    }

    const char* p = input;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        const char* start = p;
        char quote = '\0';
        if (*p == '"' || *p == '\'') {
            quote = *p;
            start = ++p;
            while (*p && *p != quote)
                p++;
            if (*p != quote) {
                free_arg_tokens(tokens, count);
                if (err && err_size > 0)
                    snprintf(err, err_size, "Unclosed quote in command arguments.");
                return 0;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }

        size_t len = (size_t)(p - start);
        char* tok = malloc(len + 1);
        if (!tok) {
            free_arg_tokens(tokens, count);
            if (err && err_size > 0)
                snprintf(err, err_size, "Out of memory while tokenizing command arguments.");
            return 0;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';

        if (count >= cap) {
            if (cap > INT_MAX / 2) {
                free(tok);
                free_arg_tokens(tokens, count);
                if (err && err_size > 0)
                    snprintf(err, err_size, "Out of memory while tokenizing command arguments.");
                return 0;
            }
            int next = cap * 2;
            if ((size_t)next > SIZE_MAX / sizeof(char*)) {
                free(tok);
                free_arg_tokens(tokens, count);
                if (err && err_size > 0)
                    snprintf(err, err_size, "Out of memory while tokenizing command arguments.");
                return 0;
            }
            char** grown = realloc(tokens, (size_t)next * sizeof(char*));
            if (!grown) {
                free(tok);
                free_arg_tokens(tokens, count);
                if (err && err_size > 0)
                    snprintf(err, err_size, "Out of memory while tokenizing command arguments.");
                return 0;
            }
            tokens = grown;
            cap = next;
        }
        tokens[count++] = tok;

        if (quote && *p == quote)
            p++;
    }

    *out_tokens = tokens;
    *out_count = count;
    return 1;
}

static int repl_run_cli_subcommand(const char* subcommand, const char* raw_args) {
    const char* exe = (g_program_path && g_program_path[0]) ? g_program_path : "sicht.exe";
    char err[256] = {0};
    char** tokens = NULL;
    int token_count = 0;
    if (!tokenize_arg_line(raw_args, &tokens, &token_count, err, sizeof(err))) {
        printf("%s\n", err[0] ? err : "Could not parse command arguments.");
        return 0;
    }

    int argv_count = 2 + token_count;
    char** child_argv = malloc((size_t)(argv_count + 1) * sizeof(char*));
    if (!child_argv) {
        free_arg_tokens(tokens, token_count);
        printf("Out of memory while preparing command.\n");
        return 0;
    }

    child_argv[0] = (char*)exe;
    child_argv[1] = (char*)subcommand;
    for (int i = 0; i < token_count; i++)
        child_argv[2 + i] = tokens[i];
    child_argv[argv_count] = NULL;

    int status = -1;
#ifdef _WIN32
    status = (int)_spawnvp(_P_WAIT, exe, (const char* const*)child_argv);
#else
    char cmd[8192];
    size_t off = 0;
    int wrote = snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, subcommand);
    if (wrote < 0 || (size_t)wrote >= sizeof(cmd)) {
        printf("Command line is too long.\n");
        free(child_argv);
        free_arg_tokens(tokens, token_count);
        return 0;
    }
    off = (size_t)wrote;
    for (int i = 0; i < token_count; i++) {
        wrote = snprintf(cmd + off, sizeof(cmd) - off, " \"%s\"", child_argv[2 + i]);
        if (wrote < 0 || (size_t)wrote >= sizeof(cmd) - off) {
            printf("Command line is too long.\n");
            free(child_argv);
            free_arg_tokens(tokens, token_count);
            return 0;
        }
        off += (size_t)wrote;
    }
    status = system(cmd);
#endif

    if (status == -1) {
        printf("Could not start `%s %s` (%s).\n", exe, subcommand, strerror(errno));
    } else if (status != 0) {
        printf("`%s %s` failed with exit code %d.\n", exe, subcommand, status);
    }

    free(child_argv);
    free_arg_tokens(tokens, token_count);
    return status == 0;
}

static void repl_print_completions(const char* prefix) {
    const int max_items = 512;
    const char* items[512];
    int count = 0;

    for (int i = 0; i < (int)(sizeof(REPL_KEYWORDS) / sizeof(REPL_KEYWORDS[0])); i++) {
        if (starts_with_prefix(REPL_KEYWORDS[i], prefix))
            count = add_unique_str(items, count, max_items, REPL_KEYWORDS[i]);
    }

    const char* runtime_symbols[512];
    int runtime_count = interpreter_collect_symbols(prefix, runtime_symbols, max_items);
    for (int i = 0; i < runtime_count; i++)
        count = add_unique_str(items, count, max_items, runtime_symbols[i]);

    if (count == 0) {
        printf("No completions for `%s`.\n", prefix);
        return;
    }

    qsort(items, (size_t)count, sizeof(const char*), cmp_string_ptr);
    if (count == 1) {
        printf("Completion: %s\n", items[0]);
        return;
    }

    printf("Completions (%d):\n", count);
    for (int i = 0; i < count; i++)
        printf("  %s\n", items[i]);
}

static void run_repl(int show_tokens, int show_ast, int trace) {
    char line[2048];
    char* chunk = NULL;
    size_t chunk_len = 0;
    size_t chunk_cap = 0;
    int block_depth = 0;
    int paste_mode = 0;
    char* history[512] = {0};
    int history_count = 0;
    const int history_max = (int)(sizeof(history) / sizeof(history[0]));
    const char* history_path = repl_history_path();
    interpreter_reset();
    interpreter_set_trace(trace);
    history_load_file(history_path, history, &history_count, history_max);

    printf("Sicht REPL\n");
    printf("Auto-exec runs when a statement/block is complete.\n");
    printf("Commands: :help, :reset, :history, :vars, :funcs, :type <expr>, :ast <expr>, :complete <prefix>, :load <file>, :save <file>, :compile <args>, :check <file>, :paste, :endpaste, :quit\n");
    printf("History file: %s\n", history_path);

    for (;;) {
        if (paste_mode)
            printf("paste> ");
        else
            printf("%s", chunk_len == 0 ? "sicht> " : "...> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            if (chunk_len > 0) {
                history_push(history, &history_count, history_max, chunk);
                execute_repl_chunk(chunk, show_tokens, show_ast, trace);
            }
            printf("\n");
            break;
        }

        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line[--line_len] = '\0';

        if (paste_mode) {
            if (strcmp(line, ":endpaste") == 0) {
                paste_mode = 0;
                if (chunk_len > 0) {
                    history_push(history, &history_count, history_max, chunk);
                    execute_repl_chunk(chunk, show_tokens, show_ast, trace);
                    chunk_len = 0;
                    block_depth = 0;
                    if (chunk)
                        chunk[0] = '\0';
                }
                continue;
            }
            append_to_chunk(&chunk, &chunk_len, &chunk_cap, line);
            append_to_chunk(&chunk, &chunk_len, &chunk_cap, "\n");
            block_depth += repl_line_depth_delta(line);
            if (block_depth < 0)
                block_depth = 0;
            continue;
        }

        if (chunk_len == 0 && line[0] == ':') {
            char* body = trim_ws_inplace(line + 1);
            char* arg = body;
            while (*arg && !isspace((unsigned char)*arg))
                arg++;
            if (*arg) {
                *arg = '\0';
                arg = trim_ws_inplace(arg + 1);
            } else {
                arg = body + strlen(body);
            }

            if (strcmp(body, "quit") == 0) {
                if (arg[0] != '\0') {
                    printf("Usage: :quit\n");
                    continue;
                }
                break;
            }

            if (strcmp(body, "help") == 0) {
                printf(":help                 Show commands\n");
                printf(":reset                Reset interpreter state\n");
                printf(":history              Show executed chunks (persistent)\n");
                printf(":vars                 Show variables with runtime types\n");
                printf(":funcs                Show available functions\n");
                printf(":type <expr>          Evaluate expression and print runtime type\n");
                printf(":ast <expr>           Show expression AST\n");
                printf(":complete <prefix>    Show autocomplete suggestions\n");
                printf(":load <file>          Execute .si file into current REPL state\n");
                printf(":save <file>          Save current chunk (or last chunk) to file\n");
                printf(":compile <args>       Run compiler command from REPL (examples: :compile app.si, :compile --exe app.si)\n");
                printf(":check <file>         Run checker command from REPL\n");
                printf(":paste                Start multiline paste mode\n");
                printf(":endpaste             End paste mode and execute\n");
                printf(":quit                 Exit REPL\n");
                continue;
            }

            if (strcmp(body, "history") == 0) {
                for (int i = 0; i < history_count; i++)
                    printf("[%d]\n%s\n", i + 1, history[i]);
                continue;
            }

            if (strcmp(body, "paste") == 0) {
                paste_mode = 1;
                block_depth = 0;
                printf("Paste mode on. Use :endpaste to execute.\n");
                continue;
            }

            if (strcmp(body, "endpaste") == 0) {
                printf("Paste mode is not active.\n");
                continue;
            }

            if (strcmp(body, "reset") == 0) {
                interpreter_reset();
                interpreter_set_trace(trace);
                arena_free_all();
                arena_init();
                printf("State reset.\n");
                continue;
            }

            if (strcmp(body, "vars") == 0) {
                interpreter_dump_vars(stdout);
                continue;
            }

            if (strcmp(body, "funcs") == 0) {
                interpreter_dump_functions(stdout);
                continue;
            }

            if (strcmp(body, "complete") == 0 || strcmp(body, "autocomplete") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :complete <prefix>\n");
                    continue;
                }
                repl_print_completions(arg);
                continue;
            }

            if (strcmp(body, "type") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :type <expression>\n");
                    continue;
                }
                source_set_label("<repl-expr>");
                source_load(arg);
                Expr* expr = parse_expression_snippet(arg, 0, 0);
                printf("%s\n", interpreter_eval_expr_type(expr));
                continue;
            }

            if (strcmp(body, "ast") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :ast <expression>\n");
                    continue;
                }
                source_set_label("<repl-expr>");
                source_load(arg);
                Expr* expr = parse_expression_snippet(arg, 0, 0);
                expr_dump(expr);
                continue;
            }

            if (strcmp(body, "load") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :load <file>\n");
                    continue;
                }
                char* path = strip_quotes_inplace(arg);
                char err[256] = {0};
                char* loaded = read_file_soft(path, err, sizeof(err));
                if (!loaded) {
                    printf("%s\n", err[0] ? err : "Could not load file.");
                    continue;
                }
                const char* first = skip_ws(loaded);
                if (starts_kw(first, "start"))
                    run_source(loaded, path, show_tokens, show_ast, trace, 1);
                else
                    execute_repl_chunk(loaded, show_tokens, show_ast, trace);
                history_push(history, &history_count, history_max, loaded);
                free(loaded);
                continue;
            }

            if (strcmp(body, "save") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :save <file>\n");
                    continue;
                }

                const char* to_save = NULL;
                if (chunk_len > 0 && chunk && chunk[0] != '\0')
                    to_save = chunk;
                else if (history_count > 0)
                    to_save = history[history_count - 1];

                if (!to_save) {
                    printf("Nothing to save yet.\n");
                    continue;
                }

                char* path = strip_quotes_inplace(arg);
                char err[256] = {0};
                if (!write_text_file_soft(path, to_save, err, sizeof(err))) {
                    printf("%s\n", err[0] ? err : "Could not save file.");
                    continue;
                }

                printf("Saved %zu bytes to %s\n", strlen(to_save), path);
                continue;
            }

            if (strcmp(body, "compile") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :compile <args>\n");
                    printf("Examples: :compile app.si | :compile --exe app.si\n");
                    continue;
                }
                (void)repl_run_cli_subcommand("compile", arg);
                continue;
            }

            if (strcmp(body, "check") == 0) {
                if (arg[0] == '\0') {
                    printf("Usage: :check <file>\n");
                    continue;
                }
                (void)repl_run_cli_subcommand("check", arg);
                continue;
            }

            printf("Unknown command: :%s\n", body);
            continue;
        }

        if (line_len == 0) {
            if (chunk_len == 0)
                continue;
            history_push(history, &history_count, history_max, chunk);
            execute_repl_chunk(chunk, show_tokens, show_ast, trace);
            chunk_len = 0;
            block_depth = 0;
            if (chunk)
                chunk[0] = '\0';
            continue;
        }

        append_to_chunk(&chunk, &chunk_len, &chunk_cap, line);
        append_to_chunk(&chunk, &chunk_len, &chunk_cap, "\n");
        block_depth += repl_line_depth_delta(line);
        if (block_depth < 0)
            block_depth = 0;

        if (block_depth == 0) {
            history_push(history, &history_count, history_max, chunk);
            execute_repl_chunk(chunk, show_tokens, show_ast, trace);
            chunk_len = 0;
            if (chunk)
                chunk[0] = '\0';
        }
    }

    ensure_history_dir(history_path);
    history_save_file(history_path, history, history_count);
    for (int i = 0; i < history_count; i++)
        free(history[i]);
    free(chunk);
}

int main(int argc, char** argv) {
    int show_tokens = 0;
    int show_ast = 0;
    int trace = 0;
    int exe_mode = 0;
    int debug_port = 4711;
    int debug_use_stdin = 0;
    const char* debug_label = NULL;
    CliMode mode = CLI_LEGACY;
    const char* file_path = NULL;
    char file_path_buf[4096];
    size_t file_path_len = 0;
    const char* compile_out_path = NULL;
    const char* compat_arg = NULL;
    int arg_start = 1;
    const char** script_argv = NULL;
    int script_argc = 0;
    char resolved_program_path[4096];
    char runtime_source_root[4096];

    if (resolve_current_executable_path((argc > 0 && argv) ? argv[0] : NULL, resolved_program_path, sizeof(resolved_program_path)))
        g_program_path = resolved_program_path;
    else
        g_program_path = (argc > 0 && argv && argv[0] && argv[0][0]) ? argv[0] : "sicht.exe";

    windows_autoinstall_if_needed(g_program_path);
    choose_runtime_source_root(g_program_path, runtime_source_root, sizeof(runtime_source_root));
    maybe_set_runtime_root_env(runtime_source_root);
    maybe_set_lib_root_env(g_program_path, runtime_source_root);

    if (argc > 1 && argv[1][0] != '-') {
        if (strcmp(argv[1], "run") == 0) {
            mode = CLI_RUN;
            arg_start = 2;
        } else if (strcmp(argv[1], "compile") == 0) {
            mode = CLI_COMPILE;
            arg_start = 2;
        } else if (strcmp(argv[1], "build") == 0) {
            fprintf(stderr, "`build` was removed. Use `compile`.\n");
            return 1;
        } else if (strcmp(argv[1], "check") == 0) {
            mode = CLI_CHECK;
            arg_start = 2;
        } else if (strcmp(argv[1], "repl") == 0) {
            mode = CLI_REPL;
            arg_start = 2;
        } else if (strcmp(argv[1], "debug") == 0) {
            mode = CLI_DEBUG;
            arg_start = 2;
        } else if (strcmp(argv[1], "version") == 0) {
            printf("Sicht %s (api %s, compat %s)\n", g_sicht_version, g_sicht_api_version, compat_current_version());
            return 0;
        } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return 0;
        }
    } else if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    } else if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("Sicht %s (api %s, compat %s)\n", g_sicht_version, g_sicht_api_version, compat_current_version());
        return 0;
    }

    for (int i = arg_start; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            if (!file_path) {
                fprintf(stderr, "Missing input file before --.\n");
                return 1;
            }
            script_argv = (const char**)&argv[i + 1];
            script_argc = argc - (i + 1);
            break;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage_for_mode(mode);
            return 0;
        }
        if (strcmp(argv[i], "--compat") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value after --compat.\n");
                print_usage_for_mode(mode);
                return 1;
            }
            compat_arg = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--compat=", 9) == 0) {
            compat_arg = argv[i] + 9;
            continue;
        }
        if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = 1;
            continue;
        }
        if (strcmp(argv[i], "--ast") == 0) {
            show_ast = 1;
            continue;
        }
        if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
            continue;
        }
        if (mode == CLI_DEBUG && strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value after --port.\n");
                print_usage_debug();
                return 1;
            }
            {
                char* end = NULL;
                errno = 0;
                long port = strtol(argv[++i], &end, 10);
                if (errno != 0 || !end || *end != '\0' || port < 1 || port > 65535) {
                    fprintf(stderr, "Invalid port value.\n");
                    print_usage_debug();
                    return 1;
                }
                debug_port = (int)port;
            }
            continue;
        }
        if (mode == CLI_DEBUG && strncmp(argv[i], "--port=", 7) == 0) {
            {
                char* end = NULL;
                errno = 0;
                long port = strtol(argv[i] + 7, &end, 10);
                if (errno != 0 || !end || *end != '\0' || port < 1 || port > 65535) {
                    fprintf(stderr, "Invalid port value.\n");
                    print_usage_debug();
                    return 1;
                }
                debug_port = (int)port;
            }
            continue;
        }
        if (mode == CLI_DEBUG && strcmp(argv[i], "--stdin") == 0) {
            debug_use_stdin = 1;
            continue;
        }
        if (mode == CLI_DEBUG && strcmp(argv[i], "--label") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value after --label.\n");
                print_usage_debug();
                return 1;
            }
            debug_label = argv[++i];
            continue;
        }
        if (mode == CLI_DEBUG && strncmp(argv[i], "--label=", 8) == 0) {
            debug_label = argv[i] + 8;
            continue;
        }
        if (strcmp(argv[i], "--emit-bytecode") == 0) {
            fprintf(stderr, "`--emit-bytecode` was removed along with the VM backend.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--llvm") == 0) {
            fprintf(stderr, "`--llvm` was removed. Use `--exe` for native builds.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--vm") == 0) {
            fprintf(stderr, "`--vm` was removed along with the VM backend.\n");
            print_usage();
            return 1;
        }
        if (strcmp(argv[i], "--backend") == 0) {
            fprintf(stderr, "`--backend` was removed. Use default compile mode or `--exe`.\n");
            print_usage_compile();
            return 1;
        }
        if (strncmp(argv[i], "--backend=", 10) == 0) {
            fprintf(stderr, "`--backend` was removed. Use default compile mode or `--exe`.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value after --out.\n");
                print_usage_compile();
                return 1;
            }
            compile_out_path = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--out=", 6) == 0) {
            compile_out_path = argv[i] + 6;
            continue;
        }
        if (strcmp(argv[i], "--plan") == 0) {
            fprintf(stderr, "`--plan` was removed from the compiler CLI.\n");
            print_usage_compile();
            return 1;
        }
        if (strncmp(argv[i], "--plan=", 7) == 0) {
            fprintf(stderr, "`--plan` was removed from the compiler CLI.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--exe") == 0) {
            exe_mode = 1;
            continue;
        }
        if (strcmp(argv[i], "--exe-out") == 0) {
            fprintf(stderr, "`--exe-out` was removed. Use `--out <file.exe>` with `--exe`.\n");
            print_usage_compile();
            return 1;
        }
        if (strncmp(argv[i], "--exe=", 6) == 0) {
            fprintf(stderr, "`--exe=<file>` was removed. Use `--exe --out <file.exe>`.\n");
            print_usage_compile();
            return 1;
        }
        if (strncmp(argv[i], "--exe-out=", 10) == 0) {
            fprintf(stderr, "`--exe-out` was removed. Use `--exe --out <file.exe>`.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--toolchain") == 0) {
            fprintf(stderr, "`--toolchain` was removed. `.exe` builds now use LLVM (clang) only.\n");
            print_usage_compile();
            return 1;
        }
        if (strncmp(argv[i], "--toolchain=", 12) == 0) {
            fprintf(stderr, "`--toolchain` was removed. `.exe` builds now use LLVM (clang) only.\n");
            print_usage_compile();
            return 1;
        }
        if (strcmp(argv[i], "--engine") == 0 || strncmp(argv[i], "--engine=", 9) == 0) {
            fprintf(stderr, "`--engine` was removed along with the VM backend.\n");
            print_usage();
            return 1;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "Unknown flag: %s\n", argv[i]);
            print_usage_for_mode(mode);
            return 1;
        }
        {
            size_t token_len = strlen(argv[i]);
            if (!file_path) {
                if (token_len >= sizeof(file_path_buf)) {
                    fprintf(stderr, "Input path is too long.\n");
                    return 1;
                }
                memcpy(file_path_buf, argv[i], token_len + 1);
                file_path_len = token_len;
                file_path = file_path_buf;
            } else {
                script_argv = (const char**)&argv[i];
                script_argc = argc - i;
                break;
            }
        }
    }

    rt_cli_set_args(script_argc, script_argv);

    {
        char compat_err[128];
        if (!compat_set_requested(compat_arg, compat_err, sizeof(compat_err))) {
            fprintf(stderr, "%s\n", compat_err);
            return 1;
        }
        const char* compat = compat_requested_version();
        if (compat && compat[0]) {
#ifdef _WIN32
            _putenv_s("SICHT_COMPAT", compat);
#else
            setenv("SICHT_COMPAT", compat, 1);
#endif
        }
    }

    char resolved_path[4096];
    char resolved_root[4096];
    char resolve_err[256];
    int resolved_project = 0;
    if (file_path && file_path[0] != '\0') {
        int resolved = resolve_project_entry(
            file_path,
            resolved_path,
            sizeof(resolved_path),
            resolved_root,
            sizeof(resolved_root),
            resolve_err,
            sizeof(resolve_err)
        );
        if (resolved < 0) {
            fprintf(stderr, "%s\n", resolve_err);
            return 1;
        }
        if (resolved > 0) {
            file_path = resolved_path;
            file_path_len = strlen(resolved_path);
            resolved_project = 1;
        }
    }

    if (file_path && file_path[0] != '\0')
        maybe_set_project_root_env(file_path);
    if (resolved_project) {
#ifdef _WIN32
        _putenv_s("SICHT_PROJECT_ROOT", resolved_root);
#else
        setenv("SICHT_PROJECT_ROOT", resolved_root, 1);
#endif
    }

    arena_init();
    gc_init();
    if (atexit(cleanup_runtime) != 0) {
        fprintf(stderr, "Failed to register cleanup handler.\n");
        return 1;
    }

    if (mode != CLI_COMPILE && mode != CLI_CHECK) {
        if (exe_mode || (compile_out_path && compile_out_path[0] != '\0')) {
            fprintf(stderr, "`--out` and `--exe` are compile-mode options.\n");
            fprintf(stderr, "Use: sicht compile ...\n");
            return 1;
        }
    }

    if (mode == CLI_CHECK) {
        if (!file_path) {
            fprintf(stderr, "Check mode expects an input file.\n");
            print_usage_check();
            return 1;
        }
        if (exe_mode || (compile_out_path && compile_out_path[0] != '\0')) {
            fprintf(stderr, "`sicht check` does not accept compile output flags.\n");
            fprintf(stderr, "Use `sicht compile` for native build outputs.\n");
            print_usage_check();
            return 1;
        }
        g_source = read_file(file_path);
        check_source_ast(g_source, file_path, show_tokens, show_ast);
        return 0;
    }

    if (mode == CLI_COMPILE) {
        if (!file_path) {
            fprintf(stderr, "Compile mode expects an input file.\n");
            print_usage_compile();
            return 1;
        }

        g_source = read_file(file_path);
        char* generated_exe = NULL;
        const char* exe_out = compile_out_path;
        if (!exe_out || exe_out[0] == '\0') {
            generated_exe = default_output_path_with_extension(file_path, ".exe");
            exe_out = generated_exe;
        }
        ensure_distinct_paths_or_error(file_path, exe_out, "executable output");
        compile_source_llvm_exe(
            g_source,
            file_path,
            runtime_source_root,
            exe_out,
            show_tokens,
            show_ast
        );
        free(generated_exe);
        return 0;
    }

    if (mode == CLI_REPL) {
        if (file_path) {
            fprintf(stderr, "REPL mode does not accept an input file.\n");
            print_usage_repl();
            return 1;
        }
        run_repl(show_tokens, show_ast, trace);
        return 0;
    }

    if (mode == CLI_DEBUG) {
        if (!debug_use_stdin && !file_path) {
            fprintf(stderr, "Debug mode expects an input file.\n");
            print_usage_debug();
            return 1;
        }
        if (debug_port <= 0 || debug_port > 65535) {
            fprintf(stderr, "Invalid debug port: %d\n", debug_port);
            return 1;
        }
        if (debug_use_stdin) {
            g_source = read_stdin();
        } else {
            g_source = read_file(file_path);
        }

        if (!debugger_start(debug_port)) {
            fprintf(stderr, "Failed to start debugger on port %d.\n", debug_port);
            return 1;
        }
        trace = 1;
        if (debug_label && debug_label[0] != '\0') {
            run_source(g_source, debug_label, show_tokens, show_ast, trace, 0);
        } else if (file_path) {
            run_source(g_source, file_path, show_tokens, show_ast, trace, 0);
        } else {
            run_source(g_source, "<stdin>", show_tokens, show_ast, trace, 0);
        }
        debugger_notify_terminated(0);
        debugger_shutdown();
        return 0;
    }

    if (mode == CLI_RUN && !file_path) {
        fprintf(stderr, "Run mode expects an input file.\n");
        print_usage_run();
        return 1;
    }

    if (!file_path) {
        run_repl(show_tokens, show_ast, trace);
        return 0;
    }

    g_source = read_file(file_path);
    run_source(g_source, file_path, show_tokens, show_ast, trace, 0);
    return 0;
}








