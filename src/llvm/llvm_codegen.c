#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "llvm_codegen.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "value.h"

typedef struct {
    char* text;
    int length;
    int id;
} StrConst;

typedef struct {
    StrConst* items;
    int count;
    int capacity;
} StrTable;

typedef struct {
    const char** names;
    int count;
    int capacity;
} SymbolTable;

typedef struct {
    const char* name;
    char* ir_name;
    const ASTNode* node;
    int param_count;
    int required_param_count;
    int is_generator;
    char* gen_ir_name;
    const char* owner_library;
    int imported;
} FunctionEntry;

typedef struct {
    const char* name;
    const ASTNode* node;
    int field_count;
} TypeEntry;

typedef struct {
    const char* name;
    LlvlTypeKind kind;
    const char* type_name;
    int array_len;
    size_t offset;
    size_t size;
} LlvlFieldEntry;

typedef struct {
    const char* name;
    int is_union;
    int field_count;
    LlvlFieldEntry* fields;
    size_t size;
} LlvlTypeEntry;

typedef struct {
    SymbolTable locals;
    unsigned char* int_known;
    unsigned char* float_known;
    char** local_values;
    char** local_i32;
    char** local_f64;
    int local_count;
    const char* ret_ptr;
    const char* ret_label;
    const char* gen_out_list;
    int terminated;
    int in_function;
    int is_generator;
    int has_return;
    const char* owner_library;
    int root_count;
} FunctionCtx;

typedef struct {
    char* library_name;
    char* symbol_name;
} LibraryOffer;

typedef struct {
    char* name;
    char* path;
    ASTNode* program;
    int init_id;
} LibraryModule;

typedef struct {
    char* path;
    ASTNode* program;
    int init_id;
} FileModule;

typedef struct {
    FILE* out;
    int temp_id;
    int label_id;
    int ok;
    char* err;
    size_t err_size;
    StrTable strings;
    SymbolTable symbols;
    FunctionEntry* functions;
    int function_count;
    int function_capacity;
    TypeEntry* types;
    int type_count;
    int type_capacity;
    LlvlTypeEntry* llvl_types;
    int llvl_type_count;
    int llvl_type_capacity;
    FunctionCtx fn;
    const char* source_label;
    unsigned char* int_known;
    unsigned char* float_known;
    const char* loop_break_labels[64];
    const char* loop_continue_labels[64];
    int loop_depth;
    int llvl_depth;
    int program_llvl_mode;
    const char* try_envs[32];
    int try_depth;
    const char* current_loading_library;
    FILE* entry_alloca_out;
    LibraryOffer library_offers[1024];
    int library_offer_count;
    char* loaded_libraries[128];
    int loaded_library_count;
    char* loading_libraries[128];
    int loading_library_count;
    char* loading_files[128];
    int loading_file_count;
    LibraryModule* libraries;
    int library_count;
    int library_capacity;
    FileModule* files;
    int file_count;
    int file_capacity;
} LLVMGen;

static FunctionEntry* find_function_any(LLVMGen* g, const char* name);
static FunctionEntry* find_function_in_library(LLVMGen* g, const char* library_name, const char* name);

static void cg_set_err(LLVMGen* g, const char* fmt, ...) {
    if (!g || !g->ok || !g->err || g->err_size == 0)
        return;
    g->ok = 0;
    va_list args;
    va_start(args, fmt);
    vsnprintf(g->err, g->err_size, fmt, args);
    va_end(args);
}

static void cg_set_err_loc(LLVMGen* g, int line, int column, const char* fmt, ...) {
    if (!g || !g->ok || !g->err || g->err_size == 0)
        return;
    g->ok = 0;
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (g->source_label && g->source_label[0] && line > 0 && column > 0) {
        snprintf(g->err, g->err_size, "%s (at %s:%d:%d)", msg, g->source_label, line, column);
    } else if (line > 0 && column > 0) {
        snprintf(g->err, g->err_size, "%s (at %d:%d)", msg, line, column);
    } else {
        snprintf(g->err, g->err_size, "%s", msg);
    }
}

static char* cg_strdup(const char* s) {
    size_t len = strlen(s);
    if (len > SIZE_MAX - 1)
        return NULL;
    len += 1;
    char* out = (char*)malloc(len);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    return out;
}

static void sym_add(SymbolTable* table, const char* name) {
    if (!table || !name || !name[0])
        return;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->names[i], name) == 0)
            return;
    }
    if (table->count >= table->capacity) {
        int new_cap = 64;
        if (table->capacity > 0) {
            if (table->capacity > INT_MAX / 2)
                return;
            new_cap = table->capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(char*))
            return;
        const char** grown = (const char**)realloc(table->names, (size_t)new_cap * sizeof(char*));
        if (!grown)
            return;
        table->names = grown;
        table->capacity = new_cap;
    }
    table->names[table->count++] = name;
}

static int sym_index(const SymbolTable* table, const char* name) {
    if (!table || !name)
        return -1;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->names[i], name) == 0)
            return i;
    }
    return -1;
}

static int has_si_extension(const char* name) {
    if (!name)
        return 0;
    size_t len = strlen(name);
    return (len >= 3 && strcmp(name + len - 3, ".si") == 0);
}

static int is_absolute_module_path(const char* name) {
    if (!name || !name[0])
        return 0;
#ifdef _WIN32
    if (isalpha((unsigned char)name[0]) && name[1] == ':')
        return 1;
#endif
    return name[0] == '/' || name[0] == '\\';
}

static int is_bare_library_name(const char* name) {
    if (!name || !name[0])
        return 0;
    if (is_absolute_module_path(name))
        return 0;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static int normalize_module_path(LLVMGen* g, const char* input, char* out, size_t out_size) {
    if (!input || !out || out_size == 0) {
        cg_set_err(g, "Invalid module path input.");
        return 0;
    }

    size_t len = strlen(input);
    if (len > SIZE_MAX - 1) {
        cg_set_err(g, "Module path is too long.");
        return 0;
    }
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        cg_set_err(g, "Out of memory while normalizing module path.");
        return 0;
    }
    for (size_t i = 0; i <= len; i++)
        buf[i] = (input[i] == '\\') ? '/' : input[i];

    if (len >= 3 && strcmp(buf + len - 3, ".si") == 0)
        buf[len - 3] = '\0';

    char prefix[16] = {0};
    char* cursor = buf;
    if (isalpha((unsigned char)cursor[0]) && cursor[1] == ':') {
        prefix[0] = cursor[0];
        prefix[1] = ':';
        prefix[2] = '\0';
        cursor += 2;
        if (*cursor == '/')
            cursor++;
    } else if (*cursor == '/') {
        prefix[0] = '/';
        prefix[1] = '\0';
        cursor++;
    }

    char** segments = NULL;
    size_t seg_count = 0;
    size_t seg_cap = 0;

    char* token = strtok(cursor, "/");
    while (token) {
        if (strcmp(token, ".") == 0) {
            token = strtok(NULL, "/");
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (seg_count > 0 && strcmp(segments[seg_count - 1], "..") != 0) {
                seg_count--;
            } else if (prefix[0] == '\0') {
                if (seg_count >= seg_cap) {
                    size_t next = seg_cap ? seg_cap * 2 : 64;
                    if (next < seg_cap || next > SIZE_MAX / sizeof(char*)) {
                        free(segments);
                        free(buf);
                        cg_set_err(g, "Out of memory while normalizing module path.");
                        return 0;
                    }
                    char** grown = (char**)realloc(segments, sizeof(char*) * next);
                    if (!grown) {
                        free(segments);
                        free(buf);
                        cg_set_err(g, "Out of memory while normalizing module path.");
                        return 0;
                    }
                    segments = grown;
                    seg_cap = next;
                }
                segments[seg_count++] = token;
            }
            token = strtok(NULL, "/");
            continue;
        }
        if (seg_count >= seg_cap) {
            size_t next = seg_cap ? seg_cap * 2 : 64;
            if (next < seg_cap || next > SIZE_MAX / sizeof(char*)) {
                free(segments);
                free(buf);
                cg_set_err(g, "Out of memory while normalizing module path.");
                return 0;
            }
            char** grown = (char**)realloc(segments, sizeof(char*) * next);
            if (!grown) {
                free(segments);
                free(buf);
                cg_set_err(g, "Out of memory while normalizing module path.");
                return 0;
            }
            segments = grown;
            seg_cap = next;
        }
        segments[seg_count++] = token;
        token = strtok(NULL, "/");
    }

    size_t pos = 0;
    if (prefix[0]) {
        size_t pfx_len = strlen(prefix);
        if (pfx_len >= out_size) {
            free(segments);
            free(buf);
            cg_set_err(g, "Module path too long.");
            return 0;
        }
        memcpy(out + pos, prefix, pfx_len);
        pos += pfx_len;
        if (prefix[0] != '/' && pos + 1 < out_size)
            out[pos++] = '/';
    }

    for (size_t i = 0; i < seg_count; i++) {
        size_t part_len = strlen(segments[i]);
        if (pos + part_len + 2 >= out_size) {
            free(segments);
            free(buf);
            cg_set_err(g, "Module path too long.");
            return 0;
        }
        memcpy(out + pos, segments[i], part_len);
        pos += part_len;
        if (i + 1 < seg_count)
            out[pos++] = '/';
    }
    out[pos] = '\0';

    free(segments);
    free(buf);
    return 1;
}

static int resolve_library_name(LLVMGen* g, const char* request, char* out, size_t out_size) {
    char normalized_request[512];
    if (!normalize_module_path(g, request, normalized_request, sizeof(normalized_request)))
        return 0;
    if (is_absolute_module_path(normalized_request) || !g->current_loading_library) {
        snprintf(out, out_size, "%s", normalized_request);
        return 1;
    }

    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s", g->current_loading_library);
    char* slash = strrchr(base_dir, '/');
    if (slash)
        *slash = '\0';
    else
        base_dir[0] = '\0';

    if (base_dir[0] == '\0') {
        snprintf(out, out_size, "%s", normalized_request);
        return 1;
    }

    char joined[512];
    if (strlen(base_dir) + 1 + strlen(normalized_request) >= sizeof(joined)) {
        cg_set_err(g, "Module path too long.");
        return 0;
    }
    joined[0] = '\0';
    strncat(joined, base_dir, sizeof(joined) - strlen(joined) - 1);
    strncat(joined, "/", sizeof(joined) - strlen(joined) - 1);
    strncat(joined, normalized_request, sizeof(joined) - strlen(joined) - 1);
    return normalize_module_path(g, joined, out, out_size);
}

static int resolve_file_name(LLVMGen* g, const char* request, char* out, size_t out_size) {
    char normalized_request[512];
    if (!normalize_module_path(g, request, normalized_request, sizeof(normalized_request)))
        return 0;
    if (is_absolute_module_path(normalized_request)) {
        snprintf(out, out_size, "%s", normalized_request);
        return 1;
    }

    const char* label = source_get_label();
    if (!label || !label[0] || label[0] == '<') {
        snprintf(out, out_size, "%s", normalized_request);
        return 1;
    }

    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s", label);
    for (int i = 0; base_dir[i]; i++) {
        if (base_dir[i] == '\\')
            base_dir[i] = '/';
    }
    char* slash = strrchr(base_dir, '/');
    if (slash)
        *slash = '\0';
    else
        base_dir[0] = '\0';

    if (base_dir[0] == '\0') {
        snprintf(out, out_size, "%s", normalized_request);
        return 1;
    }

    char joined[512];
    if (strlen(base_dir) + 1 + strlen(normalized_request) >= sizeof(joined)) {
        cg_set_err(g, "File path too long.");
        return 0;
    }
    joined[0] = '\0';
    strncat(joined, base_dir, sizeof(joined) - strlen(joined) - 1);
    strncat(joined, "/", sizeof(joined) - strlen(joined) - 1);
    strncat(joined, normalized_request, sizeof(joined) - strlen(joined) - 1);
    return normalize_module_path(g, joined, out, out_size);
}

static int build_rooted_path(char* out, size_t out_size, const char* root, const char* relative) {
    if (!out || out_size == 0 || !relative)
        return 0;
    if (!root || root[0] == '\0') {
        int wrote = snprintf(out, out_size, "%s", relative);
        return wrote > 0 && (size_t)wrote < out_size;
    }
    size_t root_len = strlen(root);
    int need_sep = root_len > 0 && root[root_len - 1] != '/' && root[root_len - 1] != '\\';
    int wrote = snprintf(out, out_size, "%s%s%s", root, need_sep ? "/" : "", relative);
    return wrote > 0 && (size_t)wrote < out_size;
}

static int join_path_simple(char* out, size_t out_size, const char* base, const char* child) {
    if (!out || out_size == 0)
        return 0;
    if (!base || base[0] == '\0') {
        int wrote = snprintf(out, out_size, "%s", child ? child : "");
        return wrote >= 0 && (size_t)wrote < out_size;
    }
    int need_sep = 1;
    size_t len = strlen(base);
    if (len > 0 && (base[len - 1] == '/' || base[len - 1] == '\\'))
        need_sep = 0;
    int wrote = snprintf(out, out_size, "%s%s%s", base, need_sep ? "/" : "", child ? child : "");
    return wrote >= 0 && (size_t)wrote < out_size;
}

#ifdef _WIN32
static int win_path_copy(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0)
        return 0;
    size_t len = strlen(src);
    if (len + 1 > dst_size)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        dst[i] = (c == '/') ? '\\' : c;
    }
    dst[len] = '\0';
    return 1;
}
#endif

static int find_library_recursive(const char* base_dir, const char* target_file, char* out, size_t out_size, int depth) {
    if (!base_dir || !target_file || !out || out_size == 0 || depth <= 0)
        return 0;
#ifdef _WIN32
    char pattern[512];
    if (!join_path_simple(pattern, sizeof(pattern), base_dir, "*"))
        return 0;
    char win_pattern[512];
    if (!win_path_copy(pattern, win_pattern, sizeof(win_pattern)))
        return 0;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(win_pattern, &data);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char* name = data.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.')
            continue;
        char child[512];
        if (!join_path_simple(child, sizeof(child), base_dir, name))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find_library_recursive(child, target_file, out, out_size, depth - 1)) {
                FindClose(h);
                return 1;
            }
        } else {
            if (strcmp(name, target_file) == 0) {
                snprintf(out, out_size, "%s", child);
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return 0;
#else
    DIR* dir = opendir(base_dir);
    if (!dir)
        return 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.')
            continue;
        char child[512];
        if (!join_path_simple(child, sizeof(child), base_dir, name))
            continue;
        struct stat st;
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (find_library_recursive(child, target_file, out, out_size, depth - 1)) {
                closedir(dir);
                return 1;
            }
        } else if (strcmp(name, target_file) == 0) {
            snprintf(out, out_size, "%s", child);
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
#endif
}

static void record_search_attempt(char attempts[][512], int* count, int max, const char* path) {
    if (!attempts || !count || max <= 0 || !path || !path[0])
        return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(attempts[i], path) == 0)
            return;
    }
    if (*count >= max)
        return;
    snprintf(attempts[*count], 512, "%s", path);
    (*count)++;
}

static void build_search_hint(char* out, size_t out_size, char attempts[][512], int count) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!attempts || count <= 0) return;
    size_t pos = 0;
    int wrote = snprintf(out, out_size, "Searched %d path%s:", count, count == 1 ? "" : "s");
    if (wrote < 0 || (size_t)wrote >= out_size)
        return;
    pos = (size_t)wrote;
    for (int i = 0; i < count; i++) {
        if (pos + 3 >= out_size)
            break;
        out[pos++] = '\n';
        out[pos++] = ' ';
        out[pos++] = ' ';
        out[pos] = '\0';
        size_t remaining = out_size - pos;
        int n = snprintf(out + pos, remaining, "%s", attempts[i]);
        if (n < 0 || (size_t)n >= remaining)
            break;
        pos += (size_t)n;
    }
}

static int build_library_file_path(LLVMGen* g, const char* library_name, char* out, size_t out_size) {
    size_t name_len = strlen(library_name);
    if (has_si_extension(library_name)) {
        if (name_len + 1 > out_size) {
            cg_set_err(g, "Module path too long.");
            if (out_size > 0)
                out[0] = '\0';
            return 0;
        }
        memcpy(out, library_name, name_len + 1);
        return 1;
    }
    if (name_len + 4 > out_size) {
        cg_set_err(g, "Module path too long.");
        if (out_size > 0)
            out[0] = '\0';
        return 0;
    }
    memcpy(out, library_name, name_len);
    out[name_len] = '.';
    out[name_len + 1] = 's';
    out[name_len + 2] = 'i';
    out[name_len + 3] = '\0';
    return 1;
}

static const char* library_basename(const char* name) {
    if (!name)
        return "";
    const char* slash = strrchr(name, '/');
    if (slash)
        return slash + 1;
    return name;
}

static int library_name_matches_declaration(const char* loaded_name, const char* declared_name) {
    if (!loaded_name || !declared_name)
        return 0;
    if (strcmp(loaded_name, declared_name) == 0)
        return 1;
    const char* base = library_basename(loaded_name);
    if (has_si_extension(base)) {
        size_t len = strlen(base);
        if (len >= 3 && strncmp(base, declared_name, len - 3) == 0 && declared_name[len - 3] == '\0')
            return 1;
    }
    return strcmp(base, declared_name) == 0;
}

static char* read_text_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if ((unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

static int is_library_loaded(LLVMGen* g, const char* name) {
    if (!g || !name)
        return 0;
    for (int i = 0; i < g->loaded_library_count; i++) {
        if (strcmp(g->loaded_libraries[i], name) == 0)
            return 1;
    }
    return 0;
}

static int is_library_loading(LLVMGen* g, const char* name) {
    if (!g || !name)
        return 0;
    for (int i = 0; i < g->loading_library_count; i++) {
        if (strcmp(g->loading_libraries[i], name) == 0)
            return 1;
    }
    return 0;
}

static int is_file_loading(LLVMGen* g, const char* path) {
    if (!g || !path)
        return 0;
    for (int i = 0; i < g->loading_file_count; i++) {
        if (strcmp(g->loading_files[i], path) == 0)
            return 1;
    }
    return 0;
}

static void push_library_loading(LLVMGen* g, const char* name) {
    if (!g || !name)
        return;
    if (g->loading_library_count >= 128) {
        cg_set_err(g, "Too many nested library loads.");
        return;
    }
    char* dup = cg_strdup(name);
    if (!dup) {
        cg_set_err(g, "Out of memory while tracking library loads.");
        return;
    }
    g->loading_libraries[g->loading_library_count++] = dup;
}

static void pop_library_loading(LLVMGen* g) {
    if (!g || g->loading_library_count <= 0)
        return;
    g->loading_library_count--;
    free(g->loading_libraries[g->loading_library_count]);
    g->loading_libraries[g->loading_library_count] = NULL;
}

static void push_file_loading(LLVMGen* g, const char* path) {
    if (!g || !path)
        return;
    if (g->loading_file_count >= 128) {
        cg_set_err(g, "Too many nested file loads.");
        return;
    }
    char* dup = cg_strdup(path);
    if (!dup) {
        cg_set_err(g, "Out of memory while tracking file loads.");
        return;
    }
    g->loading_files[g->loading_file_count++] = dup;
}

static void pop_file_loading(LLVMGen* g) {
    if (!g || g->loading_file_count <= 0)
        return;
    g->loading_file_count--;
    free(g->loading_files[g->loading_file_count]);
    g->loading_files[g->loading_file_count] = NULL;
}

static void mark_library_loaded(LLVMGen* g, const char* name) {
    if (!g || !name)
        return;
    if (g->loaded_library_count >= 128) {
        cg_set_err(g, "Loaded library limit reached.");
        return;
    }
    char* dup = cg_strdup(name);
    if (!dup) {
        cg_set_err(g, "Out of memory while tracking loaded libraries.");
        return;
    }
    g->loaded_libraries[g->loaded_library_count++] = dup;
}

static int is_library_offered(LLVMGen* g, const char* library_name, const char* symbol_name) {
    if (!g || !library_name || !symbol_name)
        return 0;
    for (int i = 0; i < g->library_offer_count; i++) {
        if (strcmp(g->library_offers[i].library_name, library_name) != 0)
            continue;
        if (strcmp(g->library_offers[i].symbol_name, symbol_name) == 0)
            return 1;
    }
    return 0;
}

static void add_library_offer(LLVMGen* g, const char* library_name, const char* symbol_name) {
    if (!g || !library_name || !symbol_name)
        return;
    if (is_library_offered(g, library_name, symbol_name))
        return;
    if (g->library_offer_count >= 1024) {
        cg_set_err(g, "Library offer limit reached.");
        return;
    }
    char* lib_dup = cg_strdup(library_name);
    char* sym_dup = cg_strdup(symbol_name);
    if (!lib_dup || !sym_dup) {
        free(lib_dup);
        free(sym_dup);
        cg_set_err(g, "Out of memory while tracking library offers.");
        return;
    }
    g->library_offers[g->library_offer_count].library_name = lib_dup;
    g->library_offers[g->library_offer_count].symbol_name = sym_dup;
    g->library_offer_count++;
}

static int library_module_index(LLVMGen* g, const char* name) {
    if (!g || !name)
        return -1;
    for (int i = 0; i < g->library_count; i++) {
        if (strcmp(g->libraries[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int file_module_index(LLVMGen* g, const char* path) {
    if (!g || !path)
        return -1;
    for (int i = 0; i < g->file_count; i++) {
        if (strcmp(g->files[i].path, path) == 0)
            return i;
    }
    return -1;
}

static LibraryModule* add_library_module(LLVMGen* g, const char* name, const char* path, ASTNode* program) {
    if (!g || !name || !path || !program)
        return NULL;
    if (g->library_count >= g->library_capacity) {
        int new_cap = 16;
        if (g->library_capacity > 0) {
            if (g->library_capacity > INT_MAX / 2) {
                cg_set_err(g, "Library tracking exceeded supported size.");
                return NULL;
            }
            new_cap = g->library_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(LibraryModule)) {
            cg_set_err(g, "Library tracking exceeded supported size.");
            return NULL;
        }
        LibraryModule* grown = (LibraryModule*)realloc(g->libraries, (size_t)new_cap * sizeof(LibraryModule));
        if (!grown) {
            cg_set_err(g, "Out of memory while tracking libraries.");
            return NULL;
        }
        g->libraries = grown;
        g->library_capacity = new_cap;
    }
    LibraryModule* slot = &g->libraries[g->library_count++];
    slot->name = cg_strdup(name);
    slot->path = cg_strdup(path);
    if (!slot->name || !slot->path) {
        free(slot->name);
        free(slot->path);
        g->library_count--;
        cg_set_err(g, "Out of memory while tracking libraries.");
        return NULL;
    }
    slot->program = program;
    slot->init_id = g->library_count;
    return slot;
}

static FileModule* add_file_module(LLVMGen* g, const char* path, ASTNode* program) {
    if (!g || !path || !program)
        return NULL;
    if (g->file_count >= g->file_capacity) {
        int new_cap = 16;
        if (g->file_capacity > 0) {
            if (g->file_capacity > INT_MAX / 2) {
                cg_set_err(g, "File tracking exceeded supported size.");
                return NULL;
            }
            new_cap = g->file_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(FileModule)) {
            cg_set_err(g, "File tracking exceeded supported size.");
            return NULL;
        }
        FileModule* grown = (FileModule*)realloc(g->files, (size_t)new_cap * sizeof(FileModule));
        if (!grown) {
            cg_set_err(g, "Out of memory while tracking files.");
            return NULL;
        }
        g->files = grown;
        g->file_capacity = new_cap;
    }
    FileModule* slot = &g->files[g->file_count++];
    slot->path = cg_strdup(path);
    if (!slot->path) {
        g->file_count--;
        cg_set_err(g, "Out of memory while tracking files.");
        return NULL;
    }
    slot->program = program;
    slot->init_id = g->file_count;
    return slot;
}

static void validate_library_program(LLVMGen* g, ASTNode* program, const char* name) {
    if (!g || !program || !name)
        return;
    int found = 0;
    for (int i = 0; i < program->count; i++) {
        ASTNode* node = program->body[i];
        if (!node || node->type != AST_CREATE_LIBRARY)
            continue;
        found = 1;
        if (!library_name_matches_declaration(name, node->library_decl.name)) {
            cg_set_err(g, "Invalid library declaration: %s", name);
            return;
        }
    }
    if (!found) {
        cg_set_err(g, "Library `%s` must declare itself using `create library %s`.", name, name);
    }
}

static void scan_offers_ast(LLVMGen* g, const ASTNode* node, const char* library_name);
static void scan_loads_ast(LLVMGen* g, const ASTNode* node);
static void check_library_stmt_positions(LLVMGen* g, const ASTNode* node, int nested);

static LibraryModule* ensure_library_loaded(LLVMGen* g, const char* name) {
    if (!g || !name || !name[0])
        return NULL;

    char resolved_name[512];
    if (!resolve_library_name(g, name, resolved_name, sizeof(resolved_name)))
        return NULL;

    int existing_idx = library_module_index(g, resolved_name);
    if (existing_idx >= 0)
        return &g->libraries[existing_idx];

    if (is_library_loading(g, resolved_name)) {
        cg_set_err(g, "Circular import detected for library: %s", resolved_name);
        return NULL;
    }

    if (is_library_loaded(g, resolved_name)) {
        int idx = library_module_index(g, resolved_name);
        return (idx >= 0) ? &g->libraries[idx] : NULL;
    }

    const char* project_root = getenv("SICHT_PROJECT_ROOT");
    const char* lib_root = getenv("SICHT_LIB_ROOT");
    const char* runtime_root = getenv("SICHT_RUNTIME_ROOT");
    const char* root_prefixes[4];
    size_t prefix_count = 0;

    if (project_root && project_root[0])
        root_prefixes[prefix_count++] = project_root;
    if (lib_root && lib_root[0] && (!project_root || strcmp(lib_root, project_root) != 0))
        root_prefixes[prefix_count++] = lib_root;
    if (runtime_root && runtime_root[0] &&
        (!project_root || strcmp(runtime_root, project_root) != 0) &&
        (!lib_root || strcmp(runtime_root, lib_root) != 0))
        root_prefixes[prefix_count++] = runtime_root;
    root_prefixes[prefix_count++] = "";

    char path[512];
    char opened_path[512];
    opened_path[0] = '\0';
    char* source = NULL;
    char attempts[24][512];
    int attempt_count = 0;

    for (size_t p = 0; p < prefix_count && !source; p++) {
        char candidate_name[512];
        if (!build_rooted_path(candidate_name, sizeof(candidate_name), root_prefixes[p], resolved_name)) {
            cg_set_err(g, "Module path too long.");
            return NULL;
        }
        if (!build_library_file_path(g, candidate_name, path, sizeof(path)))
            return NULL;
        record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), path);
        source = read_text_file(path);
        if (source)
            snprintf(opened_path, sizeof(opened_path), "%s", path);
    }

    if (!source && strncmp(resolved_name, "libs/", 5) == 0) {
        const char* rest = resolved_name + 5;
        const char* alt_roots[] = { "libs/stdlib", "libs/third_party" };
        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < sizeof(alt_roots) / sizeof(alt_roots[0]) && !source; i++) {
                char alt_name[512];
                if (snprintf(alt_name, sizeof(alt_name), "%s/%s", alt_roots[i], rest) >= (int)sizeof(alt_name)) {
                    cg_set_err(g, "Module path too long.");
                    return NULL;
                }
                char candidate_name[512];
                if (!build_rooted_path(candidate_name, sizeof(candidate_name), root_prefixes[p], alt_name)) {
                    cg_set_err(g, "Module path too long.");
                    return NULL;
                }
                if (!build_library_file_path(g, candidate_name, path, sizeof(path)))
                    return NULL;
                record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), path);
                source = read_text_file(path);
                if (source)
                    snprintf(opened_path, sizeof(opened_path), "%s", path);
            }
        }
    }

    if (!source && is_bare_library_name(resolved_name)) {
        const char* search_roots[] = { "libs/stdlib", "libs/third_party", "libs" };
        const size_t root_count = sizeof(search_roots) / sizeof(search_roots[0]);
        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < root_count && !source; i++) {
                char base_root[512];
                if (!build_rooted_path(base_root, sizeof(base_root), root_prefixes[p], search_roots[i])) {
                    cg_set_err(g, "Module path too long.");
                    return NULL;
                }
                char candidate_relative[512];
                if (snprintf(candidate_relative, sizeof(candidate_relative), "%s/%s", base_root, resolved_name) >= (int)sizeof(candidate_relative)) {
                    cg_set_err(g, "Module path too long.");
                    return NULL;
                }
                char candidate_name[512];
                if (!resolve_file_name(g, candidate_relative, candidate_name, sizeof(candidate_name)))
                    return NULL;
                char candidate_path[512];
                if (!build_library_file_path(g, candidate_name, candidate_path, sizeof(candidate_path)))
                    return NULL;
                record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), candidate_path);
                source = read_text_file(candidate_path);
                if (source)
                    snprintf(opened_path, sizeof(opened_path), "%s", candidate_path);

                if (source)
                    continue;

                if (strcmp(candidate_name, candidate_relative) != 0) {
                    if (!build_library_file_path(g, candidate_relative, candidate_path, sizeof(candidate_path)))
                        return NULL;
                    record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), candidate_path);
                    source = read_text_file(candidate_path);
                    if (source)
                        snprintf(opened_path, sizeof(opened_path), "%s", candidate_path);
                }
            }
        }
    }

    if (!source && is_bare_library_name(resolved_name)) {
        const char* deep_roots[] = { "libs/stdlib", "libs/third_party" };
        const size_t deep_count = sizeof(deep_roots) / sizeof(deep_roots[0]);
        char target_file[512];
        if (has_si_extension(resolved_name)) {
            snprintf(target_file, sizeof(target_file), "%s", resolved_name);
        } else {
            snprintf(target_file, sizeof(target_file), "%s.si", resolved_name);
        }
        char found_path[512];
        for (size_t p = 0; p < prefix_count && !source; p++) {
            for (size_t i = 0; i < deep_count && !source; i++) {
                char base_root[512];
                if (!build_rooted_path(base_root, sizeof(base_root), root_prefixes[p], deep_roots[i])) {
                    cg_set_err(g, "Module path too long.");
                    return NULL;
                }
                char hint_path[512];
                if (snprintf(hint_path, sizeof(hint_path), "%s/**/%s", base_root, target_file) < (int)sizeof(hint_path)) {
                    record_search_attempt(attempts, &attempt_count, (int)(sizeof(attempts) / sizeof(attempts[0])), hint_path);
                }
                if (find_library_recursive(base_root, target_file, found_path, sizeof(found_path), 8)) {
                    source = read_text_file(found_path);
                    if (source)
                        snprintf(opened_path, sizeof(opened_path), "%s", found_path);
                }
            }
        }
    }

    if (!source) {
        char search_hint[2048];
        build_search_hint(search_hint, sizeof(search_hint), attempts, attempt_count);
        if (search_hint[0]) {
            cg_set_err(g, "Could not open library `%s`.\n%s", resolved_name, search_hint);
        } else {
            cg_set_err(g, "Could not open library `%s`.", resolved_name);
        }
        return NULL;
    }

    push_library_loading(g, resolved_name);

    int token_count = 0;
    char previous_label[512];
    snprintf(previous_label, sizeof(previous_label), "%s", source_get_label());
    source_set_label(opened_path);
    source_load(source);
    Token* library_tokens = lex(source, &token_count);
    ASTNode* library_program = parse(library_tokens, token_count);

    check_library_stmt_positions(g, library_program, 0);
    if (!g->ok) {
        lex_free(library_tokens, token_count);
        free(source);
        pop_library_loading(g);
        source_set_label(previous_label);
        return NULL;
    }

    validate_library_program(g, library_program, resolved_name);
    if (!g->ok) {
        lex_free(library_tokens, token_count);
        free(source);
        pop_library_loading(g);
        source_set_label(previous_label);
        return NULL;
    }

    mark_library_loaded(g, resolved_name);
    LibraryModule* module = add_library_module(g, resolved_name, opened_path, library_program);
    if (!module) {
        lex_free(library_tokens, token_count);
        free(source);
        pop_library_loading(g);
        source_set_label(previous_label);
        return NULL;
    }

    const char* saved_loading = g->current_loading_library;
    g->current_loading_library = module->name;
    scan_offers_ast(g, library_program, module->name);
    scan_loads_ast(g, library_program);
    g->current_loading_library = saved_loading;

    pop_library_loading(g);
    source_set_label(previous_label);

    lex_free(library_tokens, token_count);
    free(source);
    return module;
}

static FileModule* ensure_file_loaded(LLVMGen* g, const char* path) {
    if (!g || !path || !path[0])
        return NULL;

    char normalized_request[512];
    if (!normalize_module_path(g, path, normalized_request, sizeof(normalized_request)))
        return NULL;

    char resolved_name[512];
    if (!resolve_file_name(g, path, resolved_name, sizeof(resolved_name)))
        return NULL;

    char primary_path[512];
    if (!build_library_file_path(g, resolved_name, primary_path, sizeof(primary_path)))
        return NULL;

    if (is_file_loading(g, primary_path)) {
        cg_set_err(g, "Circular file load detected: %s", primary_path);
        return NULL;
    }

    const char* project_root = getenv("SICHT_PROJECT_ROOT");
    char final_path[512];
    char* source = read_text_file(primary_path);
    snprintf(final_path, sizeof(final_path), "%s", primary_path);

    if (!source && project_root && project_root[0] != '\0') {
        char project_path[512];
        if (!build_rooted_path(project_path, sizeof(project_path), project_root, normalized_request)) {
            cg_set_err(g, "File path too long.");
            return NULL;
        }
        if (!build_library_file_path(g, project_path, project_path, sizeof(project_path)))
            return NULL;
        if (is_file_loading(g, project_path)) {
            cg_set_err(g, "Circular file load detected: %s", project_path);
            return NULL;
        }
        source = read_text_file(project_path);
        if (source)
            snprintf(final_path, sizeof(final_path), "%s", project_path);
    }

    if (!source) {
        char fallback_name[512];
        char fallback_path[512];
        if (!normalize_module_path(g, path, fallback_name, sizeof(fallback_name)))
            return NULL;
        if (!build_library_file_path(g, fallback_name, fallback_path, sizeof(fallback_path)))
            return NULL;
        if (strcmp(fallback_path, primary_path) != 0) {
            if (is_file_loading(g, fallback_path)) {
                cg_set_err(g, "Circular file load detected: %s", fallback_path);
                return NULL;
            }
            source = read_text_file(fallback_path);
            if (source)
                snprintf(final_path, sizeof(final_path), "%s", fallback_path);
        }
    }

    if (!source) {
        cg_set_err(g, "Could not open file `%s`.", primary_path);
        return NULL;
    }

    int existing = file_module_index(g, final_path);
    if (existing >= 0) {
        free(source);
        return &g->files[existing];
    }

    push_file_loading(g, final_path);

    int token_count = 0;
    char previous_label[512];
    snprintf(previous_label, sizeof(previous_label), "%s", source_get_label());
    source_set_label(final_path);
    source_load(source);
    Token* file_tokens = lex(source, &token_count);
    ASTNode* file_program = parse(file_tokens, token_count);

    FileModule* module = add_file_module(g, final_path, file_program);
    if (!module) {
        lex_free(file_tokens, token_count);
        free(source);
        pop_file_loading(g);
        source_set_label(previous_label);
        return NULL;
    }

    scan_loads_ast(g, file_program);

    pop_file_loading(g);
    source_set_label(previous_label);

    lex_free(file_tokens, token_count);
    free(source);
    return module;
}

static void scan_offers_ast(LLVMGen* g, const ASTNode* node, const char* library_name) {
    if (!g || !node || !library_name || !g->ok)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->count; i++)
                scan_offers_ast(g, node->body[i], library_name);
            break;
        case AST_LIBRARY_OFFER:
            if (node->library_offer.name)
                add_library_offer(g, library_name, node->library_offer.name);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                scan_offers_ast(g, node->branches[i].block, library_name);
            scan_offers_ast(g, node->else_block, library_name);
            break;
        case AST_WHILE:
            scan_offers_ast(g, node->while_stmt.body, library_name);
            break;
        case AST_REPEAT:
            scan_offers_ast(g, node->repeat_stmt.body, library_name);
            break;
        case AST_FOR_EACH:
            scan_offers_ast(g, node->for_each_stmt.body, library_name);
            break;
        case AST_TRY:
            scan_offers_ast(g, node->try_stmt.try_block, library_name);
            scan_offers_ast(g, node->try_stmt.otherwise_block, library_name);
            break;
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++)
                scan_offers_ast(g, node->match_stmt.branches[i].block, library_name);
            scan_offers_ast(g, node->match_stmt.otherwise_block, library_name);
            break;
        case AST_FUNCTION:
            scan_offers_ast(g, node->function_decl.body, library_name);
            break;
        default:
            break;
    }
}

static void scan_loads_ast(LLVMGen* g, const ASTNode* node) {
    if (!g || !node || !g->ok)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->count; i++)
                scan_loads_ast(g, node->body[i]);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                scan_loads_ast(g, node->branches[i].block);
            scan_loads_ast(g, node->else_block);
            break;
        case AST_TRY:
            scan_loads_ast(g, node->try_stmt.try_block);
            scan_loads_ast(g, node->try_stmt.otherwise_block);
            break;
        case AST_WHILE:
            scan_loads_ast(g, node->while_stmt.body);
            break;
        case AST_REPEAT:
            scan_loads_ast(g, node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            scan_loads_ast(g, node->for_each_stmt.body);
            break;
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++)
                scan_loads_ast(g, node->match_stmt.branches[i].block);
            scan_loads_ast(g, node->match_stmt.otherwise_block);
            break;
        case AST_FUNCTION:
            scan_loads_ast(g, node->function_decl.body);
            break;
        case AST_LOAD_LIBRARY:
            ensure_library_loaded(g, node->library_load.name);
            break;
        case AST_LOAD_FILE:
            if (node->file_load.path && node->file_load.path->type == EXPR_STRING_LITERAL) {
                ensure_file_loaded(g, node->file_load.path->string_value);
            } else {
                cg_set_err_loc(g, node->line, node->column,
                    "LLVM backend requires a string literal in `load file`.");
            }
            break;
        default:
            break;
    }
}

static const char* library_stmt_name(ASTType type) {
    switch (type) {
        case AST_CREATE_LIBRARY: return "create library";
        case AST_LOAD_LIBRARY: return "load library";
        case AST_LOAD_FILE: return "load file";
        case AST_LIBRARY_OFFER: return "offer";
        case AST_LIBRARY_TAKE: return "take";
        case AST_LIBRARY_TAKE_ALL: return "take everything";
        default: return "library statement";
    }
}

static int is_library_stmt(ASTType type) {
    return type == AST_CREATE_LIBRARY ||
           type == AST_LOAD_LIBRARY ||
           type == AST_LOAD_FILE ||
           type == AST_LIBRARY_OFFER ||
           type == AST_LIBRARY_TAKE ||
           type == AST_LIBRARY_TAKE_ALL;
}

static void check_library_stmt_positions(LLVMGen* g, const ASTNode* node, int nested) {
    if (!g || !node || !g->ok)
        return;

    if (is_library_stmt(node->type) && nested) {
        const char* name = library_stmt_name(node->type);
        cg_set_err_loc(
            g,
            node->line,
            node->column,
            "LLVM backend resolves `%s` at compile time. Move it to the top level.",
            name
        );
        return;
    }

    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->count; i++)
                check_library_stmt_positions(g, node->body[i], nested);
            break;
        case AST_FUNCTION:
            check_library_stmt_positions(g, node->function_decl.body, 1);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                check_library_stmt_positions(g, node->branches[i].block, 1);
            check_library_stmt_positions(g, node->else_block, 1);
            break;
        case AST_TRY:
            check_library_stmt_positions(g, node->try_stmt.try_block, 1);
            check_library_stmt_positions(g, node->try_stmt.otherwise_block, 1);
            break;
        case AST_WHILE:
            check_library_stmt_positions(g, node->while_stmt.body, 1);
            break;
        case AST_REPEAT:
            check_library_stmt_positions(g, node->repeat_stmt.body, 1);
            break;
        case AST_FOR_EACH:
            check_library_stmt_positions(g, node->for_each_stmt.body, 1);
            break;
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++)
                check_library_stmt_positions(g, node->match_stmt.branches[i].block, 1);
            check_library_stmt_positions(g, node->match_stmt.otherwise_block, 1);
            break;
        default:
            break;
    }
}

static void import_symbol_from_library(LLVMGen* g, const char* symbol_name, const char* library_name, const char* alias_name) {
    if (!g || !symbol_name || !library_name)
        return;
    char resolved_name[512];
    if (!resolve_library_name(g, library_name, resolved_name, sizeof(resolved_name)))
        return;
    if (!is_library_loaded(g, resolved_name)) {
        cg_set_err(g, "Library `%s` is not loaded.", resolved_name);
        return;
    }
    int lib_idx = library_module_index(g, resolved_name);
    if (lib_idx < 0) {
        cg_set_err(g, "Library `%s` is not available in LLVM backend.", resolved_name);
        return;
    }
    LibraryModule* lib = &g->libraries[lib_idx];

    if (!is_library_offered(g, lib->name, symbol_name)) {
        cg_set_err(g, "Library `%s` did not offer `%s`.", lib->name, symbol_name);
        return;
    }

    FunctionEntry* fn = find_function_in_library(g, lib->name, symbol_name);
    if (fn) {
        if (!alias_name || strcmp(alias_name, symbol_name) == 0) {
            fn->imported = 1;
            return;
        }
        FunctionEntry* existing = find_function_any(g, alias_name);
        if (existing) {
            if (existing->node == fn->node)
                return;
            cg_set_err(g, "Cannot import `%s` as `%s` because it is already defined.", symbol_name, alias_name);
            return;
        }
        if (g->function_count >= g->function_capacity) {
            int new_cap = 32;
            if (g->function_capacity > 0) {
                if (g->function_capacity > INT_MAX / 2) {
                    cg_set_err(g, "Function table exceeded supported size.");
                    return;
                }
                new_cap = g->function_capacity * 2;
            }
            if ((size_t)new_cap > SIZE_MAX / sizeof(FunctionEntry)) {
                cg_set_err(g, "Function table exceeded supported size.");
                return;
            }
            FunctionEntry* grown = (FunctionEntry*)realloc(g->functions, (size_t)new_cap * sizeof(FunctionEntry));
            if (!grown) {
                cg_set_err(g, "Out of memory while registering imported functions.");
                return;
            }
            g->functions = grown;
            g->function_capacity = new_cap;
        }
        FunctionEntry* slot = &g->functions[g->function_count++];
        slot->name = alias_name;
        slot->ir_name = NULL;
        slot->node = fn->node;
        slot->param_count = fn->param_count;
        slot->required_param_count = fn->required_param_count;
        slot->is_generator = fn->is_generator;
        slot->gen_ir_name = NULL;
        slot->owner_library = NULL;
        slot->imported = 1;
        return;
    }

    if (sym_index(&g->symbols, symbol_name) >= 0) {
        if (alias_name) {
            if (find_function_any(g, alias_name)) {
                cg_set_err(g, "Cannot import value `%s` as `%s` because a function with that name exists.",
                    symbol_name, alias_name);
            }
        }
        return;
    }

    cg_set_err(g, "Library `%s` offered `%s`, but no matching function or value was found.", lib->name, symbol_name);
}

static void import_all_from_library(LLVMGen* g, const char* library_name) {
    if (!g || !library_name)
        return;
    char resolved_name[512];
    if (!resolve_library_name(g, library_name, resolved_name, sizeof(resolved_name)))
        return;
    if (!is_library_loaded(g, resolved_name)) {
        cg_set_err(g, "Library `%s` is not loaded.", resolved_name);
        return;
    }
    int lib_idx = library_module_index(g, resolved_name);
    if (lib_idx < 0) {
        cg_set_err(g, "Library `%s` is not available in LLVM backend.", resolved_name);
        return;
    }
    LibraryModule* lib = &g->libraries[lib_idx];
    int found = 0;
    for (int i = 0; i < g->library_offer_count; i++) {
        if (strcmp(g->library_offers[i].library_name, lib->name) != 0)
            continue;
        found = 1;
        import_symbol_from_library(g, g->library_offers[i].symbol_name, lib->name, NULL);
        if (!g->ok)
            return;
    }
    if (!found) {
        cg_set_err(g, "Library `%s` did not offer any symbols.", lib->name);
    }
}

static void resolve_imports_ast(LLVMGen* g, const ASTNode* node) {
    if (!g || !node || !g->ok)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->count; i++)
                resolve_imports_ast(g, node->body[i]);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                resolve_imports_ast(g, node->branches[i].block);
            resolve_imports_ast(g, node->else_block);
            break;
        case AST_TRY:
            resolve_imports_ast(g, node->try_stmt.try_block);
            resolve_imports_ast(g, node->try_stmt.otherwise_block);
            break;
        case AST_WHILE:
            resolve_imports_ast(g, node->while_stmt.body);
            break;
        case AST_REPEAT:
            resolve_imports_ast(g, node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            resolve_imports_ast(g, node->for_each_stmt.body);
            break;
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++)
                resolve_imports_ast(g, node->match_stmt.branches[i].block);
            resolve_imports_ast(g, node->match_stmt.otherwise_block);
            break;
        case AST_FUNCTION:
            resolve_imports_ast(g, node->function_decl.body);
            break;
        case AST_LIBRARY_TAKE:
            import_symbol_from_library(
                g,
                node->library_take.name,
                node->library_take.library_name,
                node->library_take.alias_name
            );
            break;
        case AST_LIBRARY_TAKE_ALL:
            import_all_from_library(g, node->library_take_all.library_name);
            break;
        default:
            break;
    }
}

static int local_index(LLVMGen* g, const char* name);
static char* emit_load_var_value(LLVMGen* g, const char* name);
static char* emit_load_var_int(LLVMGen* g, const char* name);
static char* emit_load_var_float(LLVMGen* g, const char* name);

static int function_param_index(const ASTNode* decl, const char* param_name) {
    if (!decl || !param_name)
        return -1;
    for (int i = 0; i < decl->function_decl.param_count; i++) {
        if (decl->function_decl.params[i] &&
            strcmp(decl->function_decl.params[i], param_name) == 0)
            return i;
    }
    return -1;
}

static void functions_add(LLVMGen* g, const ASTNode* node) {
    if (!g || !node || node->type != AST_FUNCTION || !node->function_decl.name)
        return;
    for (int i = 0; i < g->function_count; i++) {
        if (strcmp(g->functions[i].name, node->function_decl.name) == 0) {
            const char* existing_owner = g->functions[i].owner_library;
            const char* new_owner = g->current_loading_library;
            int same_owner = 0;
            if (!existing_owner && !new_owner)
                same_owner = 1;
            else if (existing_owner && new_owner && strcmp(existing_owner, new_owner) == 0)
                same_owner = 1;
            if (same_owner) {
                cg_set_err(g, "Duplicate function name in LLVM backend: %s", node->function_decl.name);
                return;
            }
        }
    }
    if (g->function_count >= g->function_capacity) {
        int new_cap = 32;
        if (g->function_capacity > 0) {
            if (g->function_capacity > INT_MAX / 2) {
                cg_set_err(g, "Function table exceeded supported size.");
                return;
            }
            new_cap = g->function_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(FunctionEntry)) {
            cg_set_err(g, "Function table exceeded supported size.");
            return;
        }
        FunctionEntry* grown = (FunctionEntry*)realloc(g->functions, (size_t)new_cap * sizeof(FunctionEntry));
        if (!grown) {
            cg_set_err(g, "Out of memory while registering functions.");
            return;
        }
        g->functions = grown;
        g->function_capacity = new_cap;
    }
    FunctionEntry* slot = &g->functions[g->function_count++];
    slot->name = node->function_decl.name;
    slot->ir_name = NULL;
    slot->node = node;
    slot->param_count = node->function_decl.param_count;
    slot->required_param_count = node->function_decl.required_param_count;
    slot->is_generator = 0;
    slot->gen_ir_name = NULL;
    slot->owner_library = g->current_loading_library;
    slot->imported = g->current_loading_library ? 0 : 1;
}

static int function_is_accessible(LLVMGen* g, const FunctionEntry* fn) {
    if (!g || !fn)
        return 0;
    if (!fn->owner_library)
        return 1;
    if (fn->imported)
        return 1;
    if (g->fn.owner_library && strcmp(g->fn.owner_library, fn->owner_library) == 0)
        return 1;
    if (g->current_loading_library && strcmp(g->current_loading_library, fn->owner_library) == 0)
        return 1;
    return 0;
}

static FunctionEntry* find_function_any(LLVMGen* g, const char* name) {
    if (!g || !name)
        return NULL;
    for (int i = 0; i < g->function_count; i++) {
        if (strcmp(g->functions[i].name, name) == 0)
            return &g->functions[i];
    }
    return NULL;
}

static FunctionEntry* find_function_in_library(LLVMGen* g, const char* library_name, const char* name) {
    if (!g || !library_name || !name)
        return NULL;
    for (int i = 0; i < g->function_count; i++) {
        FunctionEntry* fn = &g->functions[i];
        if (!fn->owner_library)
            continue;
        if (strcmp(fn->owner_library, library_name) != 0)
            continue;
        if (strcmp(fn->name, name) == 0)
            return fn;
    }
    return NULL;
}

static FunctionEntry* find_function(LLVMGen* g, const char* name) {
    if (!g || !name)
        return NULL;
    for (int i = 0; i < g->function_count; i++) {
        if (strcmp(g->functions[i].name, name) != 0)
            continue;
        if (function_is_accessible(g, &g->functions[i]))
            return &g->functions[i];
    }
    return NULL;
}

static void types_add(LLVMGen* g, const ASTNode* node) {
    if (!g || !node || node->type != AST_TYPE_DECL || !node->type_decl.name)
        return;
    for (int i = 0; i < g->type_count; i++) {
        if (strcmp(g->types[i].name, node->type_decl.name) == 0) {
            cg_set_err(g, "Duplicate type name in LLVM backend: %s", node->type_decl.name);
            return;
        }
    }
    if (g->type_count >= g->type_capacity) {
        int new_cap = 32;
        if (g->type_capacity > 0) {
            if (g->type_capacity > INT_MAX / 2) {
                cg_set_err(g, "Type table exceeded supported size.");
                return;
            }
            new_cap = g->type_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(TypeEntry)) {
            cg_set_err(g, "Type table exceeded supported size.");
            return;
        }
        TypeEntry* grown = (TypeEntry*)realloc(g->types, (size_t)new_cap * sizeof(TypeEntry));
        if (!grown) {
            cg_set_err(g, "Out of memory while registering types.");
            return;
        }
        g->types = grown;
        g->type_capacity = new_cap;
    }
    TypeEntry* slot = &g->types[g->type_count++];
    slot->name = node->type_decl.name;
    slot->node = node;
    slot->field_count = node->type_decl.field_count;
}

static TypeEntry* find_type(LLVMGen* g, const char* name) {
    if (!g || !name)
        return NULL;
    for (int i = 0; i < g->type_count; i++) {
        if (strcmp(g->types[i].name, name) == 0)
            return &g->types[i];
    }
    return NULL;
}

static LlvlTypeEntry* find_llvl_type(LLVMGen* g, const char* name) {
    if (!g || !name)
        return NULL;
    for (int i = 0; i < g->llvl_type_count; i++) {
        if (strcmp(g->llvl_types[i].name, name) == 0)
            return &g->llvl_types[i];
    }
    return NULL;
}

static size_t llvl_type_size_from_kind(LLVMGen* g, LlvlTypeKind kind, const char* type_name) {
    switch (kind) {
        case LLVL_TYPE_BYTE: return 1;
        case LLVL_TYPE_INT: return 4;
        case LLVL_TYPE_FLOAT: return 8;
        case LLVL_TYPE_STRUCT:
        case LLVL_TYPE_UNION: {
            LlvlTypeEntry* def = find_llvl_type(g, type_name);
            if (!def) {
                cg_set_err(g, "Unknown low-level type: %s", type_name ? type_name : "<null>");
                return 0;
            }
            return def->size;
        }
    }
    return 0;
}

static int llvl_kind_to_buffer_kind(LlvlTypeKind kind) {
    switch (kind) {
        case LLVL_TYPE_BYTE: return (int)BUFFER_ELEM_BYTE;
        case LLVL_TYPE_INT: return (int)BUFFER_ELEM_INT;
        case LLVL_TYPE_FLOAT: return (int)BUFFER_ELEM_FLOAT;
        case LLVL_TYPE_STRUCT: return (int)BUFFER_ELEM_STRUCT;
        case LLVL_TYPE_UNION: return (int)BUFFER_ELEM_UNION;
        default: return (int)BUFFER_ELEM_RAW;
    }
}

static void add_llvl_type(LLVMGen* g, const ASTNode* node, int is_union) {
    if (!g || !node)
        return;
    const char* name = is_union ? node->llvl_union_decl.name : node->llvl_struct_decl.name;
    int field_count = is_union ? node->llvl_union_decl.field_count : node->llvl_struct_decl.field_count;
    LlvlField* fields = is_union ? node->llvl_union_decl.fields : node->llvl_struct_decl.fields;

    if (!name)
        return;
    if (find_llvl_type(g, name)) {
        cg_set_err(g, "Duplicate low-level type: %s", name);
        return;
    }
    if (g->llvl_type_count >= g->llvl_type_capacity) {
        int new_cap = g->llvl_type_capacity > 0 ? g->llvl_type_capacity * 2 : 16;
        if (new_cap < g->llvl_type_capacity) {
            cg_set_err(g, "Low-level type table exceeded maximum size.");
            return;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(LlvlTypeEntry)) {
            cg_set_err(g, "Low-level type table exceeded maximum size.");
            return;
        }
        LlvlTypeEntry* grown = (LlvlTypeEntry*)realloc(g->llvl_types, (size_t)new_cap * sizeof(LlvlTypeEntry));
        if (!grown) {
            cg_set_err(g, "Out of memory while growing low-level type table.");
            return;
        }
        memset(grown + g->llvl_type_capacity, 0, (size_t)(new_cap - g->llvl_type_capacity) * sizeof(LlvlTypeEntry));
        g->llvl_types = grown;
        g->llvl_type_capacity = new_cap;
    }

    LlvlTypeEntry* slot = &g->llvl_types[g->llvl_type_count++];
    memset(slot, 0, sizeof(LlvlTypeEntry));
    slot->name = name;
    slot->is_union = is_union ? 1 : 0;
    slot->field_count = field_count;
    slot->size = 0;

    if (field_count <= 0 || !fields)
        return;

    slot->fields = (LlvlFieldEntry*)calloc((size_t)field_count, sizeof(LlvlFieldEntry));
    if (!slot->fields) {
        cg_set_err(g, "Out of memory while allocating low-level fields.");
        return;
    }

    size_t offset = 0;
    size_t max_size = 0;
    for (int i = 0; i < field_count; i++) {
        slot->fields[i].name = fields[i].name;
        slot->fields[i].kind = fields[i].kind;
        slot->fields[i].type_name = fields[i].type_name;
        slot->fields[i].array_len = fields[i].array_len;

        size_t base_size = llvl_type_size_from_kind(g, fields[i].kind, fields[i].type_name);
        size_t field_size = base_size;
        if (fields[i].array_len > 0) {
            if (base_size > 0 && (size_t)fields[i].array_len > SIZE_MAX / base_size) {
                cg_set_err(g, "Low-level field size overflow.");
                return;
            }
            field_size = base_size * (size_t)fields[i].array_len;
        }
        slot->fields[i].size = field_size;
        slot->fields[i].offset = is_union ? 0 : offset;

        if (!is_union) {
            if (offset > SIZE_MAX - field_size) {
                cg_set_err(g, "Low-level struct size overflow.");
                return;
            }
            offset += field_size;
        } else if (field_size > max_size) {
            max_size = field_size;
        }
    }

    slot->size = is_union ? max_size : offset;
}

static int type_field_index(const TypeEntry* type, const char* field_name) {
    if (!type || !type->node || !field_name)
        return -1;
    for (int i = 0; i < type->field_count; i++) {
        const char* field = type->node->type_decl.fields[i];
        if (field && strcmp(field, field_name) == 0)
            return i;
    }
    return -1;
}

static char* mangle_function_name(const char* name, const char* owner, int index) {
    if (!name)
        return NULL;
    size_t len = strlen(name);
    size_t owner_len = owner ? strlen(owner) : 0;
    if (len > SIZE_MAX - owner_len)
        return NULL;
    size_t total = len + owner_len;
    if (total > (SIZE_MAX - 32) / 3)
        return NULL;
    size_t max_len = total * 3 + 32;
    char* out = (char*)malloc(max_len);
    if (!out)
        return NULL;
    size_t off = 0;
    const char* prefix = "@sicht_fn_";
    size_t prefix_len = strlen(prefix);
    memcpy(out + off, prefix, prefix_len);
    off += prefix_len;
    if (owner && owner[0]) {
        for (size_t i = 0; i < owner_len; i++) {
            unsigned char c = (unsigned char)owner[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                out[off++] = (char)c;
            } else {
                static const char* hex = "0123456789ABCDEF";
                out[off++] = '_';
                out[off++] = hex[(c >> 4) & 0xF];
                out[off++] = hex[c & 0xF];
            }
        }
        out[off++] = '_';
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out[off++] = (char)c;
        } else {
            static const char* hex = "0123456789ABCDEF";
            out[off++] = '_';
            out[off++] = hex[(c >> 4) & 0xF];
            out[off++] = hex[c & 0xF];
        }
    }
    off += (size_t)snprintf(out + off, max_len - off, "_%d", index);
    out[off] = '\0';
    return out;
}

static int ast_contains_yield(const ASTNode* node) {
    if (!node)
        return 0;
    switch (node->type) {
        case AST_YIELD:
            return 1;
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++) {
                if (ast_contains_yield(node->body[i]))
                    return 1;
            }
            return 0;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++) {
                if (ast_contains_yield(node->branches[i].block))
                    return 1;
            }
            return ast_contains_yield(node->else_block);
        case AST_TRY:
            if (ast_contains_yield(node->try_stmt.try_block))
                return 1;
            return ast_contains_yield(node->try_stmt.otherwise_block);
        case AST_WHILE:
            return ast_contains_yield(node->while_stmt.body);
        case AST_REPEAT:
            return ast_contains_yield(node->repeat_stmt.body);
        case AST_FOR_EACH:
            return ast_contains_yield(node->for_each_stmt.body);
        case AST_MATCH:
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                if (ast_contains_yield(node->match_stmt.branches[i].block))
                    return 1;
            }
            return ast_contains_yield(node->match_stmt.otherwise_block);
        case AST_FUNCTION:
            return ast_contains_yield(node->function_decl.body);
        default:
            return 0;
    }
}

static char* str_dup_checked(const char* text) {
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1) {
        fprintf(stderr, "Out of memory while building string table.\n");
        exit(1);
    }
    len += 1;
    char* out = (char*)malloc(len);
    if (!out) {
        fprintf(stderr, "Out of memory while building string table.\n");
        exit(1);
    }
    memcpy(out, text, len);
    return out;
}

static void str_table_free(StrTable* table) {
    if (!table || !table->items)
        return;
    for (int i = 0; i < table->count; i++) {
        free(table->items[i].text);
        table->items[i].text = NULL;
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

static void str_add(StrTable* table, const char* text) {
    if (!table || !text)
        return;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].text, text) == 0)
            return;
    }
    if (table->count >= table->capacity) {
        int new_cap = 64;
        if (table->capacity > 0) {
            if (table->capacity > INT_MAX / 2)
                return;
            new_cap = table->capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(StrConst))
            return;
        StrConst* grown = (StrConst*)realloc(table->items, (size_t)new_cap * sizeof(StrConst));
        if (!grown)
            return;
        table->items = grown;
        table->capacity = new_cap;
    }
    StrConst* slot = &table->items[table->count++];
    slot->text = str_dup_checked(text);
    slot->length = (int)strlen(text) + 1;
    slot->id = table->count - 1;
}

static void collect_strings_expr(StrTable* table, const Expr* expr);
static char* build_print_string_line(const ASTNode* node) {
    if (!node || node->type != AST_PRINT_STRING)
        return NULL;
    size_t total = 0;
    for (int i = 0; i < node->part_count; i++) {
        StringPart* sp = &node->parts[i];
        if (sp->type != STR_TEXT)
            return NULL;
        if (sp->text) {
            size_t part_len = strlen(sp->text);
            if (part_len > SIZE_MAX - total)
                return NULL;
            total += part_len;
        }
    }
    if (total > SIZE_MAX - 2)
        return NULL;
    size_t len = total + 2;
    char* out = (char*)malloc(len);
    if (!out)
        return NULL;
    size_t off = 0;
    for (int i = 0; i < node->part_count; i++) {
        StringPart* sp = &node->parts[i];
        if (sp->text) {
            size_t n = strlen(sp->text);
            memcpy(out + off, sp->text, n);
            off += n;
        }
    }
    out[off++] = '\n';
    out[off] = '\0';
    return out;
}

static const ASTNode* unwrap_single_stmt_block(const ASTNode* node) {
    if (!node)
        return NULL;
    if (node->type == AST_BLOCK && node->count == 1)
        return node->body[0];
    return node;
}

static void collect_strings_ast(StrTable* table, const ASTNode* node) {
    if (!node || !table)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++)
                collect_strings_ast(table, node->body[i]);
            break;
        case AST_PRINT_STRING:
            {
                char* joined = build_print_string_line(node);
                if (joined) {
                    str_add(table, joined);
                    free(joined);
                }
            }
            for (int i = 0; i < node->part_count; i++) {
                StringPart* sp = &node->parts[i];
                if (sp->type == STR_TEXT && sp->text)
                    str_add(table, sp->text);
                if (sp->type == STR_EXPR)
                    collect_strings_expr(table, sp->expr);
            }
            break;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
        case AST_SET:
            if (node->expr)
                collect_strings_expr(table, node->expr);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++) {
                collect_strings_expr(table, node->branches[i].condition);
                collect_strings_ast(table, node->branches[i].block);
            }
            collect_strings_ast(table, node->else_block);
            break;
        case AST_WHILE:
            collect_strings_expr(table, node->while_stmt.condition);
            collect_strings_ast(table, node->while_stmt.body);
            break;
        case AST_REPEAT:
            collect_strings_expr(table, node->repeat_stmt.times);
            collect_strings_ast(table, node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            collect_strings_expr(table, node->for_each_stmt.iterable);
            collect_strings_ast(table, node->for_each_stmt.body);
            break;
        case AST_MATCH:
            collect_strings_expr(table, node->match_stmt.target);
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                collect_strings_expr(table, node->match_stmt.branches[i].value);
                collect_strings_ast(table, node->match_stmt.branches[i].block);
            }
            collect_strings_ast(table, node->match_stmt.otherwise_block);
            break;
        case AST_TRY:
            collect_strings_ast(table, node->try_stmt.try_block);
            collect_strings_ast(table, node->try_stmt.otherwise_block);
            break;
        case AST_RETURN:
            collect_strings_expr(table, node->return_stmt.value);
            break;
        case AST_YIELD:
            collect_strings_expr(table, node->expr);
            break;
        case AST_FILE_WRITE:
        case AST_FILE_APPEND:
            collect_strings_expr(table, node->file_io_stmt.path);
            collect_strings_expr(table, node->file_io_stmt.content);
            break;
        case AST_LLVL_SAVE:
            collect_strings_expr(table, node->llvl_save.size);
            if (node->llvl_save.has_type && node->llvl_save.elem_type_name)
                str_add(table, node->llvl_save.elem_type_name);
            break;
        case AST_LLVL_RESIZE:
            collect_strings_expr(table, node->llvl_resize.size);
            if (node->llvl_resize.has_type && node->llvl_resize.elem_type_name)
                str_add(table, node->llvl_resize.elem_type_name);
            break;
        case AST_LLVL_COPY:
            if (node->llvl_copy.has_size)
                collect_strings_expr(table, node->llvl_copy.size);
            break;
        case AST_LLVL_SET_VALUE:
            collect_strings_expr(table, node->llvl_set_value.value);
            break;
        case AST_LLVL_SET_BYTE:
            collect_strings_expr(table, node->llvl_set_byte.index);
            collect_strings_expr(table, node->llvl_set_byte.value);
            break;
        case AST_LLVL_BIT_OP:
            collect_strings_expr(table, node->llvl_bit_op.index);
            break;
        case AST_LLVL_SET_AT:
            collect_strings_expr(table, node->llvl_set_at.address);
            collect_strings_expr(table, node->llvl_set_at.value);
            break;
        case AST_LLVL_ATOMIC_OP:
            collect_strings_expr(table, node->llvl_atomic_op.address);
            collect_strings_expr(table, node->llvl_atomic_op.value);
            break;
        case AST_LLVL_MARK_VOLATILE:
            collect_strings_expr(table, node->llvl_mark_volatile.target);
            break;
        case AST_LLVL_SET_CHECK:
            break;
        case AST_LLVL_PORT_WRITE:
            collect_strings_expr(table, node->llvl_port_write.port);
            collect_strings_expr(table, node->llvl_port_write.value);
            break;
        case AST_LLVL_REGISTER_INTERRUPT:
            collect_strings_expr(table, node->llvl_register_interrupt.interrupt_id);
            if (node->llvl_register_interrupt.handler_name)
                str_add(table, node->llvl_register_interrupt.handler_name);
            break;
        case AST_LLVL_SET_FIELD:
            collect_strings_expr(table, node->llvl_set_field.target);
            collect_strings_expr(table, node->llvl_set_field.value);
            if (node->llvl_set_field.field_name)
                str_add(table, node->llvl_set_field.field_name);
            break;
        case AST_LLVL_STRUCT_DECL:
        case AST_LLVL_UNION_DECL: {
            const char* type_name = node->type == AST_LLVL_STRUCT_DECL
                ? node->llvl_struct_decl.name
                : node->llvl_union_decl.name;
            int field_count = node->type == AST_LLVL_STRUCT_DECL
                ? node->llvl_struct_decl.field_count
                : node->llvl_union_decl.field_count;
            LlvlField* fields = node->type == AST_LLVL_STRUCT_DECL
                ? node->llvl_struct_decl.fields
                : node->llvl_union_decl.fields;
            if (type_name)
                str_add(table, type_name);
            for (int i = 0; i < field_count; i++) {
                if (fields[i].name)
                    str_add(table, fields[i].name);
            }
            break;
        }
        case AST_LLVL_PIN_WRITE:
            collect_strings_expr(table, node->llvl_pin_write.value);
            collect_strings_expr(table, node->llvl_pin_write.pin);
            break;
        case AST_LLVL_WAIT:
            collect_strings_expr(table, node->llvl_wait.duration);
            break;
        case AST_LIST_ADD:
            collect_strings_expr(table, node->list_add.value);
            break;
        case AST_LIST_REMOVE:
            collect_strings_expr(table, node->list_remove.value);
            break;
        case AST_LIST_REMOVE_ELEMENT:
            collect_strings_expr(table, node->list_remove_element.index);
            break;
        case AST_LIST_CLEAR:
            break;
        case AST_SET_ELEMENT:
            collect_strings_expr(table, node->set_element.index);
            collect_strings_expr(table, node->set_element.value);
            break;
        case AST_DICT_ADD:
            collect_strings_expr(table, node->dict_add.key);
            collect_strings_expr(table, node->dict_add.value);
            break;
        case AST_DICT_REMOVE:
            collect_strings_expr(table, node->dict_remove.key);
            break;
        case AST_DICT_CONTAINS_ITEM:
            collect_strings_expr(table, node->dict_contains.key);
            break;
        case AST_DICT_CLEAR:
            break;
        case AST_FUNCTION:
            collect_strings_ast(table, node->function_decl.body);
            break;
        default:
            break;
    }
}

static void collect_strings_expr(StrTable* table, const Expr* expr) {
    if (!expr || !table)
        return;
    switch (expr->type) {
        case EXPR_STRING_LITERAL:
            if (expr->string_value)
                str_add(table, expr->string_value);
            break;
        case EXPR_CALL:
            for (int i = 0; i < expr->call_arg_count; i++)
                collect_strings_expr(table, expr->call_args[i]);
            break;
        case EXPR_INPUT:
            if (expr->input_prompt)
                str_add(table, expr->input_prompt);
            break;
        case EXPR_BUILTIN:
            collect_strings_expr(table, expr->builtin_arg);
            break;
        case EXPR_CHAR_AT:
            collect_strings_expr(table, expr->char_string);
            collect_strings_expr(table, expr->char_index);
            break;
        case EXPR_FILE_READ:
            collect_strings_expr(table, expr->file_read_path);
            break;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_ATOMIC_READ:
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            collect_strings_expr(table, expr->llvl_target);
            break;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            collect_strings_expr(table, expr->llvl_target);
            collect_strings_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_OFFSET:
            collect_strings_expr(table, expr->llvl_target);
            collect_strings_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_FIELD:
            collect_strings_expr(table, expr->llvl_target);
            if (expr->name)
                str_add(table, expr->name);
            break;
        case EXPR_LIST_COMPREHENSION:
            collect_strings_expr(table, expr->comp_iterable);
            collect_strings_expr(table, expr->comp_filter);
            collect_strings_expr(table, expr->comp_result);
            break;
        case EXPR_LIST:
            for (int i = 0; i < expr->list_count; i++)
                collect_strings_expr(table, expr->list_items[i]);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->element_count; i++)
                collect_strings_expr(table, expr->elements[i]);
            break;
        case EXPR_DICT:
            for (int i = 0; i < expr->dict_count; i++) {
                collect_strings_expr(table, expr->dict_keys[i]);
                collect_strings_expr(table, expr->dict_values[i]);
            }
            break;
        case EXPR_LIST_AT:
            collect_strings_expr(table, expr->list_expr);
            collect_strings_expr(table, expr->list_index);
            break;
        case EXPR_ARRAY_INDEX:
            collect_strings_expr(table, expr->array_expr);
            collect_strings_expr(table, expr->index_expr);
            break;
        case EXPR_DICT_GET:
            collect_strings_expr(table, expr->dict_expr);
            collect_strings_expr(table, expr->dict_key);
            break;
        case EXPR_INDEX_OF:
            collect_strings_expr(table, expr->index_value);
            collect_strings_expr(table, expr->index_array);
            break;
        case EXPR_BINARY:
            collect_strings_expr(table, expr->left);
            collect_strings_expr(table, expr->right);
            break;
        case EXPR_UNARY:
            collect_strings_expr(table, expr->expr);
            break;
        case EXPR_CAST:
            collect_strings_expr(table, expr->cast_expr);
            break;
        default:
            break;
    }
}

static void collect_symbols_expr(SymbolTable* table, const Expr* expr);
static void collect_functions_ast(LLVMGen* g, const ASTNode* node, const char* owner_library);
static void collect_locals_expr(SymbolTable* table, const Expr* expr);
static void emit_stmt(LLVMGen* g, const ASTNode* node);
static int emit_require_llvl_expr(LLVMGen* g, const Expr* expr);
static void collect_symbols_ast(SymbolTable* table, const ASTNode* node) {
    if (!node || !table)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++)
                collect_symbols_ast(table, node->body[i]);
            break;
        case AST_SET:
            if (node->name)
                sym_add(table, node->name);
            collect_symbols_expr(table, node->expr);
            break;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
            collect_symbols_expr(table, node->expr);
            break;
        case AST_PRINT_STRING:
            for (int i = 0; i < node->part_count; i++) {
                StringPart* sp = &node->parts[i];
                if (sp->type == STR_EXPR)
                    collect_symbols_expr(table, sp->expr);
            }
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++) {
                collect_symbols_expr(table, node->branches[i].condition);
                collect_symbols_ast(table, node->branches[i].block);
            }
            collect_symbols_ast(table, node->else_block);
            break;
        case AST_WHILE:
            collect_symbols_expr(table, node->while_stmt.condition);
            collect_symbols_ast(table, node->while_stmt.body);
            break;
        case AST_REPEAT:
            collect_symbols_expr(table, node->repeat_stmt.times);
            collect_symbols_ast(table, node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            if (node->for_each_stmt.item_name)
                sym_add(table, node->for_each_stmt.item_name);
            collect_symbols_expr(table, node->for_each_stmt.iterable);
            collect_symbols_ast(table, node->for_each_stmt.body);
            break;
        case AST_MATCH:
            collect_symbols_expr(table, node->match_stmt.target);
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                collect_symbols_expr(table, node->match_stmt.branches[i].value);
                collect_symbols_ast(table, node->match_stmt.branches[i].block);
            }
            collect_symbols_ast(table, node->match_stmt.otherwise_block);
            break;
        case AST_TRY:
            collect_symbols_ast(table, node->try_stmt.try_block);
            collect_symbols_ast(table, node->try_stmt.otherwise_block);
            break;
        case AST_RETURN:
            collect_symbols_expr(table, node->return_stmt.value);
            break;
        case AST_YIELD:
            collect_symbols_expr(table, node->expr);
            break;
        case AST_FILE_WRITE:
        case AST_FILE_APPEND:
            collect_symbols_expr(table, node->file_io_stmt.path);
            collect_symbols_expr(table, node->file_io_stmt.content);
            break;
        case AST_LLVL_SAVE:
            if (node->llvl_save.name)
                sym_add(table, node->llvl_save.name);
            collect_symbols_expr(table, node->llvl_save.size);
            break;
        case AST_LLVL_REMOVE:
            if (node->llvl_remove.name)
                sym_add(table, node->llvl_remove.name);
            break;
        case AST_LLVL_RESIZE:
            if (node->llvl_resize.name)
                sym_add(table, node->llvl_resize.name);
            collect_symbols_expr(table, node->llvl_resize.size);
            break;
        case AST_LLVL_COPY:
            if (node->llvl_copy.src)
                sym_add(table, node->llvl_copy.src);
            if (node->llvl_copy.dest)
                sym_add(table, node->llvl_copy.dest);
            if (node->llvl_copy.has_size)
                collect_symbols_expr(table, node->llvl_copy.size);
            break;
        case AST_LLVL_MOVE:
            if (node->llvl_move.src)
                sym_add(table, node->llvl_move.src);
            if (node->llvl_move.dest)
                sym_add(table, node->llvl_move.dest);
            break;
        case AST_LLVL_SET_VALUE:
            if (node->llvl_set_value.name)
                sym_add(table, node->llvl_set_value.name);
            collect_symbols_expr(table, node->llvl_set_value.value);
            break;
        case AST_LLVL_SET_BYTE:
            if (node->llvl_set_byte.name)
                sym_add(table, node->llvl_set_byte.name);
            collect_symbols_expr(table, node->llvl_set_byte.index);
            collect_symbols_expr(table, node->llvl_set_byte.value);
            break;
        case AST_LLVL_BIT_OP:
            if (node->llvl_bit_op.name)
                sym_add(table, node->llvl_bit_op.name);
            collect_symbols_expr(table, node->llvl_bit_op.index);
            break;
        case AST_LLVL_SET_AT:
            collect_symbols_expr(table, node->llvl_set_at.address);
            collect_symbols_expr(table, node->llvl_set_at.value);
            break;
        case AST_LLVL_ATOMIC_OP:
            collect_symbols_expr(table, node->llvl_atomic_op.address);
            collect_symbols_expr(table, node->llvl_atomic_op.value);
            break;
        case AST_LLVL_MARK_VOLATILE:
            collect_symbols_expr(table, node->llvl_mark_volatile.target);
            break;
        case AST_LLVL_SET_CHECK:
            break;
        case AST_LLVL_PORT_WRITE:
            collect_symbols_expr(table, node->llvl_port_write.port);
            collect_symbols_expr(table, node->llvl_port_write.value);
            break;
        case AST_LLVL_REGISTER_INTERRUPT:
            collect_symbols_expr(table, node->llvl_register_interrupt.interrupt_id);
            break;
        case AST_LLVL_SET_FIELD:
            collect_symbols_expr(table, node->llvl_set_field.target);
            collect_symbols_expr(table, node->llvl_set_field.value);
            break;
        case AST_LLVL_PIN_WRITE:
            collect_symbols_expr(table, node->llvl_pin_write.value);
            collect_symbols_expr(table, node->llvl_pin_write.pin);
            break;
        case AST_LLVL_WAIT:
            collect_symbols_expr(table, node->llvl_wait.duration);
            break;
        case AST_LLVL_ENUM_DECL:
            for (int i = 0; i < node->llvl_enum_decl.count; i++)
                sym_add(table, node->llvl_enum_decl.names[i]);
            break;
        case AST_LLVL_BITFIELD_DECL:
            for (int i = 0; i < node->llvl_bitfield_decl.count; i++)
                sym_add(table, node->llvl_bitfield_decl.names[i]);
            break;
        case AST_SET_ELEMENT:
            if (node->set_element.name)
                sym_add(table, node->set_element.name);
            collect_symbols_expr(table, node->set_element.index);
            collect_symbols_expr(table, node->set_element.value);
            break;
        case AST_LIST_ADD:
            if (node->list_add.list_name)
                sym_add(table, node->list_add.list_name);
            collect_symbols_expr(table, node->list_add.value);
            break;
        case AST_LIST_REMOVE:
            if (node->list_remove.list_name)
                sym_add(table, node->list_remove.list_name);
            collect_symbols_expr(table, node->list_remove.value);
            break;
        case AST_LIST_REMOVE_ELEMENT:
            if (node->list_remove_element.list_name)
                sym_add(table, node->list_remove_element.list_name);
            collect_symbols_expr(table, node->list_remove_element.index);
            break;
        case AST_LIST_CLEAR:
            if (node->list_clear.list_name)
                sym_add(table, node->list_clear.list_name);
            break;
        case AST_DICT_ADD:
            if (node->dict_add.dict_name)
                sym_add(table, node->dict_add.dict_name);
            collect_symbols_expr(table, node->dict_add.key);
            collect_symbols_expr(table, node->dict_add.value);
            break;
        case AST_DICT_REMOVE:
            if (node->dict_remove.dict_name)
                sym_add(table, node->dict_remove.dict_name);
            collect_symbols_expr(table, node->dict_remove.key);
            break;
        case AST_DICT_CLEAR:
            if (node->dict_clear.dict_name)
                sym_add(table, node->dict_clear.dict_name);
            break;
        case AST_DICT_CONTAINS_ITEM:
            if (node->dict_contains.dict_name)
                sym_add(table, node->dict_contains.dict_name);
            collect_symbols_expr(table, node->dict_contains.key);
            break;
        case AST_LIBRARY_TAKE:
            if (node->library_take.name)
                sym_add(table, node->library_take.name);
            if (node->library_take.alias_name)
                sym_add(table, node->library_take.alias_name);
            break;
        default:
            break;
    }
}

static void collect_symbols_expr(SymbolTable* table, const Expr* expr) {
    if (!expr || !table)
        return;
    switch (expr->type) {
        case EXPR_VARIABLE:
            if (expr->name)
                sym_add(table, expr->name);
            break;
        case EXPR_CALL:
            for (int i = 0; i < expr->call_arg_count; i++)
                collect_symbols_expr(table, expr->call_args[i]);
            break;
        case EXPR_INPUT:
            break;
        case EXPR_BUILTIN:
            collect_symbols_expr(table, expr->builtin_arg);
            break;
        case EXPR_CHAR_AT:
            collect_symbols_expr(table, expr->char_string);
            collect_symbols_expr(table, expr->char_index);
            break;
        case EXPR_FILE_READ:
            collect_symbols_expr(table, expr->file_read_path);
            break;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_ATOMIC_READ:
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            collect_symbols_expr(table, expr->llvl_target);
            break;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            collect_symbols_expr(table, expr->llvl_target);
            collect_symbols_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_OFFSET:
            collect_symbols_expr(table, expr->llvl_target);
            collect_symbols_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_FIELD:
            collect_symbols_expr(table, expr->llvl_target);
            break;
        case EXPR_LIST_COMPREHENSION:
            if (expr->comp_var_name)
                sym_add(table, expr->comp_var_name);
            collect_symbols_expr(table, expr->comp_iterable);
            collect_symbols_expr(table, expr->comp_filter);
            collect_symbols_expr(table, expr->comp_result);
            break;
        case EXPR_LIST:
            for (int i = 0; i < expr->list_count; i++)
                collect_symbols_expr(table, expr->list_items[i]);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->element_count; i++)
                collect_symbols_expr(table, expr->elements[i]);
            break;
        case EXPR_DICT:
            for (int i = 0; i < expr->dict_count; i++) {
                collect_symbols_expr(table, expr->dict_keys[i]);
                collect_symbols_expr(table, expr->dict_values[i]);
            }
            break;
        case EXPR_LIST_AT:
            collect_symbols_expr(table, expr->list_expr);
            collect_symbols_expr(table, expr->list_index);
            break;
        case EXPR_ARRAY_INDEX:
            collect_symbols_expr(table, expr->array_expr);
            collect_symbols_expr(table, expr->index_expr);
            break;
        case EXPR_DICT_GET:
            collect_symbols_expr(table, expr->dict_expr);
            collect_symbols_expr(table, expr->dict_key);
            break;
        case EXPR_INDEX_OF:
            collect_symbols_expr(table, expr->index_value);
            collect_symbols_expr(table, expr->index_array);
            break;
        case EXPR_BINARY:
            collect_symbols_expr(table, expr->left);
            collect_symbols_expr(table, expr->right);
            break;
        case EXPR_UNARY:
            collect_symbols_expr(table, expr->expr);
            break;
        case EXPR_CAST:
            collect_symbols_expr(table, expr->cast_expr);
            break;
        default:
            break;
    }
}

static void collect_functions_ast(LLVMGen* g, const ASTNode* node, const char* owner_library) {
    if (!g || !node || !g->ok)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->count; i++) {
                collect_functions_ast(g, node->body[i], owner_library);
                if (!g->ok)
                    return;
            }
            break;
        case AST_FUNCTION:
            g->current_loading_library = owner_library;
            functions_add(g, node);
            g->current_loading_library = NULL;
            break;
        case AST_TYPE_DECL:
            types_add(g, node);
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                collect_functions_ast(g, node->branches[i].block, owner_library);
            collect_functions_ast(g, node->else_block, owner_library);
            break;
        case AST_WHILE:
            collect_functions_ast(g, node->while_stmt.body, owner_library);
            break;
        case AST_REPEAT:
            collect_functions_ast(g, node->repeat_stmt.body, owner_library);
            break;
        default:
            break;
    }
}

static void collect_locals_ast(SymbolTable* table, const ASTNode* node) {
    if (!table || !node)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
        case AST_LLVL_BLOCK:
            for (int i = 0; i < node->count; i++)
                collect_locals_ast(table, node->body[i]);
            break;
        case AST_SET:
            if (node->name)
                sym_add(table, node->name);
            collect_locals_expr(table, node->expr);
            break;
        case AST_PRINT_EXPR:
        case AST_EXPR_STMT:
            collect_locals_expr(table, node->expr);
            break;
        case AST_PRINT_STRING:
            for (int i = 0; i < node->part_count; i++) {
                if (node->parts[i].type == STR_EXPR)
                    collect_locals_expr(table, node->parts[i].expr);
            }
            break;
        case AST_IF:
            for (int i = 0; i < node->branch_count; i++)
                collect_locals_ast(table, node->branches[i].block);
            collect_locals_ast(table, node->else_block);
            for (int i = 0; i < node->branch_count; i++)
                collect_locals_expr(table, node->branches[i].condition);
            break;
        case AST_WHILE:
            collect_locals_expr(table, node->while_stmt.condition);
            collect_locals_ast(table, node->while_stmt.body);
            break;
        case AST_REPEAT:
            collect_locals_expr(table, node->repeat_stmt.times);
            collect_locals_ast(table, node->repeat_stmt.body);
            break;
        case AST_FOR_EACH:
            if (node->for_each_stmt.item_name)
                sym_add(table, node->for_each_stmt.item_name);
            collect_locals_expr(table, node->for_each_stmt.iterable);
            collect_locals_ast(table, node->for_each_stmt.body);
            break;
        case AST_MATCH:
            collect_locals_expr(table, node->match_stmt.target);
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                collect_locals_expr(table, node->match_stmt.branches[i].value);
                collect_locals_ast(table, node->match_stmt.branches[i].block);
            }
            collect_locals_ast(table, node->match_stmt.otherwise_block);
            break;
        case AST_TRY:
            collect_locals_ast(table, node->try_stmt.try_block);
            collect_locals_ast(table, node->try_stmt.otherwise_block);
            break;
        case AST_RETURN:
            collect_locals_expr(table, node->return_stmt.value);
            break;
        case AST_YIELD:
            collect_locals_expr(table, node->expr);
            break;
        case AST_SET_ELEMENT:
            collect_locals_expr(table, node->set_element.index);
            collect_locals_expr(table, node->set_element.value);
            break;
        case AST_LIST_ADD:
            collect_locals_expr(table, node->list_add.value);
            break;
        case AST_LIST_REMOVE:
            collect_locals_expr(table, node->list_remove.value);
            break;
        case AST_LIST_REMOVE_ELEMENT:
            collect_locals_expr(table, node->list_remove_element.index);
            break;
        case AST_LIST_CLEAR:
            break;
        case AST_DICT_ADD:
            collect_locals_expr(table, node->dict_add.key);
            collect_locals_expr(table, node->dict_add.value);
            break;
        case AST_DICT_REMOVE:
            collect_locals_expr(table, node->dict_remove.key);
            break;
        case AST_DICT_CLEAR:
            break;
        case AST_DICT_CONTAINS_ITEM:
            collect_locals_expr(table, node->dict_contains.key);
            break;
        case AST_FILE_WRITE:
        case AST_FILE_APPEND:
            collect_locals_expr(table, node->file_io_stmt.path);
            collect_locals_expr(table, node->file_io_stmt.content);
            break;
        case AST_LLVL_SAVE:
            if (node->llvl_save.name)
                sym_add(table, node->llvl_save.name);
            collect_locals_expr(table, node->llvl_save.size);
            break;
        case AST_LLVL_RESIZE:
            collect_locals_expr(table, node->llvl_resize.size);
            break;
        case AST_LLVL_COPY:
            if (!node->llvl_copy.has_size && node->llvl_copy.dest)
                sym_add(table, node->llvl_copy.dest);
            if (node->llvl_copy.has_size)
                collect_locals_expr(table, node->llvl_copy.size);
            break;
        case AST_LLVL_MOVE:
            if (node->llvl_move.dest)
                sym_add(table, node->llvl_move.dest);
            break;
        case AST_LLVL_SET_VALUE:
            collect_locals_expr(table, node->llvl_set_value.value);
            break;
        case AST_LLVL_SET_BYTE:
            collect_locals_expr(table, node->llvl_set_byte.index);
            collect_locals_expr(table, node->llvl_set_byte.value);
            break;
        case AST_LLVL_BIT_OP:
            collect_locals_expr(table, node->llvl_bit_op.index);
            break;
        case AST_LLVL_SET_AT:
            collect_locals_expr(table, node->llvl_set_at.address);
            collect_locals_expr(table, node->llvl_set_at.value);
            break;
        case AST_LLVL_ATOMIC_OP:
            collect_locals_expr(table, node->llvl_atomic_op.address);
            collect_locals_expr(table, node->llvl_atomic_op.value);
            break;
        case AST_LLVL_MARK_VOLATILE:
            collect_locals_expr(table, node->llvl_mark_volatile.target);
            break;
        case AST_LLVL_SET_CHECK:
            break;
        case AST_LLVL_PORT_WRITE:
            collect_locals_expr(table, node->llvl_port_write.port);
            collect_locals_expr(table, node->llvl_port_write.value);
            break;
        case AST_LLVL_REGISTER_INTERRUPT:
            collect_locals_expr(table, node->llvl_register_interrupt.interrupt_id);
            break;
        case AST_LLVL_SET_FIELD:
            collect_locals_expr(table, node->llvl_set_field.target);
            collect_locals_expr(table, node->llvl_set_field.value);
            break;
        case AST_LLVL_PIN_WRITE:
            collect_locals_expr(table, node->llvl_pin_write.value);
            collect_locals_expr(table, node->llvl_pin_write.pin);
            break;
        case AST_LLVL_WAIT:
            collect_locals_expr(table, node->llvl_wait.duration);
            break;
        case AST_FUNCTION:
            break;
        default:
            break;
    }
}

static void collect_locals_expr(SymbolTable* table, const Expr* expr) {
    if (!table || !expr)
        return;
    switch (expr->type) {
        case EXPR_LIST_COMPREHENSION:
            if (expr->comp_var_name)
                sym_add(table, expr->comp_var_name);
            collect_locals_expr(table, expr->comp_iterable);
            collect_locals_expr(table, expr->comp_filter);
            collect_locals_expr(table, expr->comp_result);
            break;
        case EXPR_BINARY:
            collect_locals_expr(table, expr->left);
            collect_locals_expr(table, expr->right);
            break;
        case EXPR_UNARY:
            collect_locals_expr(table, expr->expr);
            break;
        case EXPR_CAST:
            collect_locals_expr(table, expr->cast_expr);
            break;
        case EXPR_CALL:
            for (int i = 0; i < expr->call_arg_count; i++)
                collect_locals_expr(table, expr->call_args[i]);
            break;
        case EXPR_LIST:
            for (int i = 0; i < expr->list_count; i++)
                collect_locals_expr(table, expr->list_items[i]);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->element_count; i++)
                collect_locals_expr(table, expr->elements[i]);
            break;
        case EXPR_DICT:
            for (int i = 0; i < expr->dict_count; i++) {
                collect_locals_expr(table, expr->dict_keys[i]);
                collect_locals_expr(table, expr->dict_values[i]);
            }
            break;
        case EXPR_LIST_AT:
            collect_locals_expr(table, expr->list_expr);
            collect_locals_expr(table, expr->list_index);
            break;
        case EXPR_ARRAY_INDEX:
            collect_locals_expr(table, expr->array_expr);
            collect_locals_expr(table, expr->index_expr);
            break;
        case EXPR_DICT_GET:
            collect_locals_expr(table, expr->dict_expr);
            collect_locals_expr(table, expr->dict_key);
            break;
        case EXPR_INDEX_OF:
            collect_locals_expr(table, expr->index_value);
            collect_locals_expr(table, expr->index_array);
            break;
        case EXPR_BUILTIN:
            collect_locals_expr(table, expr->builtin_arg);
            break;
        case EXPR_CHAR_AT:
            collect_locals_expr(table, expr->char_string);
            collect_locals_expr(table, expr->char_index);
            break;
        case EXPR_FILE_READ:
            collect_locals_expr(table, expr->file_read_path);
            break;
        case EXPR_LLVL_VALUE_OF:
        case EXPR_LLVL_ATOMIC_READ:
        case EXPR_LLVL_PLACE_OF:
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            collect_locals_expr(table, expr->llvl_target);
            break;
        case EXPR_LLVL_BYTE_OF:
        case EXPR_LLVL_BIT_OF:
            collect_locals_expr(table, expr->llvl_target);
            collect_locals_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_OFFSET:
            collect_locals_expr(table, expr->llvl_target);
            collect_locals_expr(table, expr->llvl_index);
            break;
        case EXPR_LLVL_FIELD:
            collect_locals_expr(table, expr->llvl_target);
            break;
        default:
            break;
    }
}

static void fprint_ir_escaped(FILE* out, const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (p && *p) {
        unsigned char c = *p++;
        if (c == '"' || c == '\\' || c < 32 || c > 126) {
            fprintf(out, "\\%02X", (unsigned int)c);
        } else {
            fputc((int)c, out);
        }
    }
}

static char* cg_temp(LLVMGen* g) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%%t%d", g->temp_id++);
    char* out = cg_strdup(buf);
    if (!out) {
        cg_set_err(g, "Out of memory while generating temporary names.");
        return "";
    }
    return out;
}

static char* cg_label(LLVMGen* g, const char* prefix) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%d", prefix, g->label_id++);
    char* out = cg_strdup(buf);
    if (!out) {
        cg_set_err(g, "Out of memory while generating labels.");
        return "";
    }
    return out;
}

static void cg_flush_temp(FILE* src, FILE* dst) {
    if (!src || !dst)
        return;
    fflush(src);
    fseek(src, 0, SEEK_SET);
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
}

static char* cg_alloca_value(LLVMGen* g) {
    char* ptr = cg_temp(g);
    FILE* out = g && g->entry_alloca_out ? g->entry_alloca_out : (g ? g->out : NULL);
    if (out)
        fprintf(out, "%s = alloca %%Value\n", ptr);
    return ptr;
}

static char* cg_alloca_i32(LLVMGen* g) {
    char* ptr = cg_temp(g);
    FILE* out = g && g->entry_alloca_out ? g->entry_alloca_out : (g ? g->out : NULL);
    if (out)
        fprintf(out, "%s = alloca i32\n", ptr);
    return ptr;
}

static int expr_is_int(LLVMGen* g, const Expr* expr) {
    if (!g || !expr)
        return 0;
    switch (expr->type) {
        case EXPR_LITERAL:
            return 1;
        case EXPR_VARIABLE: {
            int lidx = local_index(g, expr->name);
            if (lidx >= 0) {
                if (!g->fn.int_known)
                    return 0;
                return g->fn.int_known[lidx] != 0;
            }
            int idx = sym_index(&g->symbols, expr->name);
            if (idx < 0 || !g->int_known)
                return 0;
            return g->int_known[idx] != 0;
        }
        case EXPR_UNARY:
            if (expr->op == OP_NEG || expr->op == OP_BIT_NOT)
                return expr_is_int(g, expr->expr);
            return 0;
        case EXPR_BINARY:
            if (expr->op == OP_ADD || expr->op == OP_SUB ||
                expr->op == OP_MUL || expr->op == OP_DIV ||
                expr->op == OP_BIT_AND || expr->op == OP_BIT_OR ||
                expr->op == OP_BIT_XOR || expr->op == OP_SHL ||
                expr->op == OP_SHR) {
                return expr_is_int(g, expr->left) && expr_is_int(g, expr->right);
            }
            return 0;
        case EXPR_INDEX_OF:
            return 1;
        case EXPR_BUILTIN:
            return expr->builtin_type == BUILTIN_LENGTH;
        case EXPR_LLVL_READ_PIN:
        case EXPR_LLVL_PORT_READ:
            return 1;
        case EXPR_CAST:
            return expr->cast_type == CAST_TO_INTEGER;
        default:
            return 0;
    }
}

static int expr_is_float(LLVMGen* g, const Expr* expr) {
    if (!g || !expr)
        return 0;
    switch (expr->type) {
        case EXPR_FLOAT_LITERAL:
            return 1;
        case EXPR_VARIABLE: {
            int lidx = local_index(g, expr->name);
            if (lidx >= 0) {
                if (!g->fn.float_known)
                    return 0;
                return g->fn.float_known[lidx] != 0;
            }
            int idx = sym_index(&g->symbols, expr->name);
            if (idx < 0 || !g->float_known)
                return 0;
            return g->float_known[idx] != 0;
        }
        case EXPR_UNARY:
            if (expr->op == OP_NEG)
                return expr_is_float(g, expr->expr);
            return 0;
        case EXPR_BINARY:
            if (expr->op == OP_ADD || expr->op == OP_SUB ||
                expr->op == OP_MUL || expr->op == OP_DIV) {
                return expr_is_float(g, expr->left) && expr_is_float(g, expr->right);
            }
            return 0;
        case EXPR_CAST:
            if (expr->cast_type == CAST_TO_FLOAT)
                return expr_is_int(g, expr->cast_expr) || expr_is_float(g, expr->cast_expr);
            return 0;
        default:
            return 0;
    }
}

static int local_index(LLVMGen* g, const char* name) {
    if (!g || !name || !g->fn.in_function)
        return -1;
    return sym_index(&g->fn.locals, name);
}

static char* emit_load_global_int(LLVMGen* g, int idx) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference.");
        return NULL;
    }
    char* ptr = cg_temp(g);
    char* val = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x i32], [%d x i32]* @globals_i32, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    fprintf(g->out, "%s = load i32, i32* %s\n", val, ptr);
    return val;
}

static char* emit_load_local_int(LLVMGen* g, int idx) {
    if (!g || idx < 0 || !g->fn.local_i32) {
        cg_set_err(g, "Unknown local variable reference.");
        return NULL;
    }
    char* val = cg_temp(g);
    fprintf(g->out, "%s = load i32, i32* %s\n", val, g->fn.local_i32[idx]);
    return val;
}

static char* emit_load_global_float(LLVMGen* g, int idx) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference.");
        return NULL;
    }
    char* ptr = cg_temp(g);
    char* val = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x double], [%d x double]* @globals_f64, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    fprintf(g->out, "%s = load double, double* %s\n", val, ptr);
    return val;
}

static char* emit_load_local_float(LLVMGen* g, int idx) {
    if (!g || idx < 0 || !g->fn.local_f64) {
        cg_set_err(g, "Unknown local variable reference.");
        return NULL;
    }
    char* val = cg_temp(g);
    fprintf(g->out, "%s = load double, double* %s\n", val, g->fn.local_f64[idx]);
    return val;
}

static void emit_store_global_int(LLVMGen* g, int idx, const char* value_i32) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable in assignment.");
        return;
    }
    char* ptr = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x i32], [%d x i32]* @globals_i32, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    fprintf(g->out, "store i32 %s, i32* %s\n", value_i32, ptr);
}

static void emit_store_local_int(LLVMGen* g, int idx, const char* value_i32) {
    if (!g || idx < 0 || !g->fn.local_i32) {
        cg_set_err(g, "Unknown local variable in assignment.");
        return;
    }
    fprintf(g->out, "store i32 %s, i32* %s\n", value_i32, g->fn.local_i32[idx]);
}

static void emit_store_global_float(LLVMGen* g, int idx, const char* value_f64) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable in assignment.");
        return;
    }
    char* ptr = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x double], [%d x double]* @globals_f64, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    fprintf(g->out, "store double %s, double* %s\n", value_f64, ptr);
}

static void emit_store_local_float(LLVMGen* g, int idx, const char* value_f64) {
    if (!g || idx < 0 || !g->fn.local_f64) {
        cg_set_err(g, "Unknown local variable in assignment.");
        return;
    }
    fprintf(g->out, "store double %s, double* %s\n", value_f64, g->fn.local_f64[idx]);
}

static char* emit_expr(LLVMGen* g, const Expr* expr);

static char* emit_expr_int(LLVMGen* g, const Expr* expr) {
    if (!expr) {
        cg_set_err(g, "Missing expression node.");
        return NULL;
    }
    switch (expr->type) {
        case EXPR_LITERAL: {
            char* out = cg_temp(g);
            fprintf(g->out, "%s = add i32 0, %d\n", out, expr->int_value);
            return out;
        }
        case EXPR_VARIABLE: {
            return emit_load_var_int(g, expr->name);
        }
        case EXPR_UNARY: {
            if (expr->op == OP_NEG) {
                char* v = emit_expr_int(g, expr->expr);
                if (!v)
                    return NULL;
                char* out = cg_temp(g);
                fprintf(g->out, "%s = sub i32 0, %s\n", out, v);
                return out;
            }
            if (expr->op == OP_BIT_NOT) {
                char* v = emit_expr_int(g, expr->expr);
                if (!v)
                    return NULL;
                char* out = cg_temp(g);
                fprintf(g->out, "%s = xor i32 %s, -1\n", out, v);
                return out;
            }
            break;
        }
        case EXPR_BINARY: {
            if (expr->op == OP_DIV) {
                char* left_val = emit_expr(g, expr->left);
                char* right_val = emit_expr(g, expr->right);
                if (!left_val || !right_val)
                    return NULL;
                char* out_val = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_div(%%Value* %s, %%Value* %s, %%Value* %s)\n", out_val, left_val, right_val);
                char* out = cg_temp(g);
                fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", out, out_val);
                return out;
            }
            char* left = emit_expr_int(g, expr->left);
            char* right = emit_expr_int(g, expr->right);
            if (!left || !right)
                return NULL;
            char* out = cg_temp(g);
            switch (expr->op) {
                case OP_ADD:
                    fprintf(g->out, "%s = add i32 %s, %s\n", out, left, right);
                    return out;
                case OP_SUB:
                    fprintf(g->out, "%s = sub i32 %s, %s\n", out, left, right);
                    return out;
                case OP_MUL:
                    fprintf(g->out, "%s = mul i32 %s, %s\n", out, left, right);
                    return out;
                case OP_BIT_AND:
                    fprintf(g->out, "%s = and i32 %s, %s\n", out, left, right);
                    return out;
                case OP_BIT_OR:
                    fprintf(g->out, "%s = or i32 %s, %s\n", out, left, right);
                    return out;
                case OP_BIT_XOR:
                    fprintf(g->out, "%s = xor i32 %s, %s\n", out, left, right);
                    return out;
                case OP_SHL:
                    fprintf(g->out, "%s = shl i32 %s, %s\n", out, left, right);
                    return out;
                case OP_SHR:
                    fprintf(g->out, "%s = ashr i32 %s, %s\n", out, left, right);
                    return out;
                case OP_DIV:
                    break;
                default:
                    break;
            }
            break;
        }
        case EXPR_INDEX_OF: {
            char* boxed = emit_expr(g, expr);
            if (!boxed)
                return NULL;
            char* out = cg_temp(g);
            fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", out, boxed);
            return out;
        }
        case EXPR_BUILTIN:
            if (expr->builtin_type == BUILTIN_LENGTH) {
                char* boxed = emit_expr(g, expr);
                if (!boxed)
                    return NULL;
                char* out = cg_temp(g);
                fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", out, boxed);
                return out;
            }
            break;
        case EXPR_CAST:
            if (expr->cast_type == CAST_TO_INTEGER) {
                if (expr_is_int(g, expr->cast_expr))
                    return emit_expr_int(g, expr->cast_expr);
                char* v = emit_expr(g, expr->cast_expr);
                if (!v)
                    return NULL;
                char* casted = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_cast_int(%%Value* %s, %%Value* %s)\n", casted, v);
                char* out = cg_temp(g);
                fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", out, casted);
                return out;
            }
            break;
        default:
            break;
    }
    cg_set_err(g, "Unsupported integer expression in LLVM fast path.");
    return NULL;
}

static char* emit_expr_float(LLVMGen* g, const Expr* expr) {
    if (!expr) {
        cg_set_err(g, "Missing expression node.");
        return NULL;
    }
    switch (expr->type) {
        case EXPR_FLOAT_LITERAL: {
            char* out = cg_temp(g);
            fprintf(g->out, "%s = fadd double 0.0, %.17e\n", out, expr->float_value);
            return out;
        }
        case EXPR_LITERAL: {
            char* out = cg_temp(g);
            fprintf(g->out, "%s = sitofp i32 %d to double\n", out, expr->int_value);
            return out;
        }
        case EXPR_VARIABLE: {
            return emit_load_var_float(g, expr->name);
        }
        case EXPR_UNARY: {
            if (expr->op != OP_NEG)
                break;
            char* v = emit_expr_float(g, expr->expr);
            if (!v)
                return NULL;
            char* out = cg_temp(g);
            fprintf(g->out, "%s = fsub double 0.0, %s\n", out, v);
            return out;
        }
        case EXPR_BINARY: {
            char* left = emit_expr_float(g, expr->left);
            char* right = emit_expr_float(g, expr->right);
            if (!left || !right)
                return NULL;
            char* out = cg_temp(g);
            switch (expr->op) {
                case OP_ADD:
                    fprintf(g->out, "%s = fadd double %s, %s\n", out, left, right);
                    return out;
                case OP_SUB:
                    fprintf(g->out, "%s = fsub double %s, %s\n", out, left, right);
                    return out;
                case OP_MUL:
                    fprintf(g->out, "%s = fmul double %s, %s\n", out, left, right);
                    return out;
                case OP_DIV:
                    fprintf(g->out, "%s = fdiv double %s, %s\n", out, left, right);
                    return out;
                default:
                    break;
            }
            break;
        }
        case EXPR_CAST:
            if (expr->cast_type == CAST_TO_FLOAT) {
                if (expr_is_float(g, expr->cast_expr))
                    return emit_expr_float(g, expr->cast_expr);
                if (expr_is_int(g, expr->cast_expr)) {
                    char* v = emit_expr_int(g, expr->cast_expr);
                    if (!v)
                        return NULL;
                    char* out = cg_temp(g);
                    fprintf(g->out, "%s = sitofp i32 %s to double\n", out, v);
                    return out;
                }
            }
            break;
        default:
            break;
    }
    cg_set_err(g, "Unsupported float expression in LLVM fast path.");
    return NULL;
}

static void emit_function(LLVMGen* g, const FunctionEntry* fn);

static char* emit_expr_cond_i1(LLVMGen* g, const Expr* expr) {
    if (!expr) {
        cg_set_err(g, "Missing condition expression.");
        return NULL;
    }
    if (expr->type == EXPR_BINARY) {
        OpType op = expr->op;
        if (op == OP_EQ || op == OP_NE || op == OP_GT || op == OP_LT || op == OP_GTE || op == OP_LTE) {
            if (expr_is_int(g, expr->left) && expr_is_int(g, expr->right)) {
                char* left = emit_expr_int(g, expr->left);
                char* right = emit_expr_int(g, expr->right);
                if (!left || !right)
                    return NULL;
                char* out = cg_temp(g);
                const char* pred = "eq";
                if (op == OP_NE) pred = "ne";
                else if (op == OP_GT) pred = "sgt";
                else if (op == OP_LT) pred = "slt";
                else if (op == OP_GTE) pred = "sge";
                else if (op == OP_LTE) pred = "sle";
                fprintf(g->out, "%s = icmp %s i32 %s, %s\n", out, pred, left, right);
                return out;
            }
            if (expr_is_float(g, expr->left) && expr_is_float(g, expr->right)) {
                char* left = emit_expr_float(g, expr->left);
                char* right = emit_expr_float(g, expr->right);
                if (!left || !right)
                    return NULL;
                char* out = cg_temp(g);
                const char* pred = "oeq";
                if (op == OP_NE) pred = "one";
                else if (op == OP_GT) pred = "ogt";
                else if (op == OP_LT) pred = "olt";
                else if (op == OP_GTE) pred = "oge";
                else if (op == OP_LTE) pred = "ole";
                fprintf(g->out, "%s = fcmp %s double %s, %s\n", out, pred, left, right);
                return out;
            }
        }
    }

    char* boxed = emit_expr(g, expr);
    if (!g->ok)
        return NULL;
    char* cond_i32 = cg_temp(g);
    char* cond_i1 = cg_temp(g);
    fprintf(g->out, "%s = call i32 @rt_truthy(%%Value* %s)\n", cond_i32, boxed);
    fprintf(g->out, "%s = icmp ne i32 %s, 0\n", cond_i1, cond_i32);
    return cond_i1;
}

static void emit_string_globals(LLVMGen* g) {
    for (int i = 0; i < g->strings.count; i++) {
        StrConst* s = &g->strings.items[i];
        fprintf(g->out, "@.str%d = private unnamed_addr constant [%d x i8] c\"",
                s->id, s->length);
        fprint_ir_escaped(g->out, s->text);
        fprintf(g->out, "\\00\", align 1\n");
    }
    fprintf(g->out, "\n");
}

static void emit_library_init_call(LLVMGen* g, const LibraryModule* lib) {
    if (!g || !lib)
        return;
    char flag[64];
    char fn[64];
    snprintf(flag, sizeof(flag), "@.lib_loaded%d", lib->init_id);
    snprintf(fn, sizeof(fn), "@__sicht_lib_init_%d", lib->init_id);
    char* loaded = cg_temp(g);
    char* cmp = cg_temp(g);
    char* do_label = cg_label(g, "libload");
    char* end_label = cg_label(g, "libend");
    fprintf(g->out, "%s = load i32, i32* %s\n", loaded, flag);
    fprintf(g->out, "%s = icmp ne i32 %s, 0\n", cmp, loaded);
    fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cmp, end_label, do_label);
    fprintf(g->out, "%s:\n", do_label);
    fprintf(g->out, "call void %s()\n", fn);
    fprintf(g->out, "store i32 1, i32* %s\n", flag);
    fprintf(g->out, "br label %%%s\n", end_label);
    fprintf(g->out, "%s:\n", end_label);
}

static void emit_gc_tick(LLVMGen* g) {
    if (!g || !g->ok)
        return;
    fprintf(g->out, "call void @rt_step_tick()\n");
    fprintf(g->out, "call void @rt_gc_maybe_collect()\n");
}

static void emit_file_init_call(LLVMGen* g, const FileModule* file) {
    if (!g || !file)
        return;
    char fn[64];
    snprintf(fn, sizeof(fn), "@__sicht_file_init_%d", file->init_id);
    fprintf(g->out, "call void %s()\n", fn);
}

static void emit_module_init_function(LLVMGen* g, const char* name, const ASTNode* program, int is_library, int init_id) {
    if (!g || !program)
        return;
    char fn[64];
    if (is_library)
        snprintf(fn, sizeof(fn), "@__sicht_lib_init_%d", init_id);
    else
        snprintf(fn, sizeof(fn), "@__sicht_file_init_%d", init_id);

    FunctionCtx saved_fn = g->fn;
    const char* saved_loading = g->current_loading_library;
    int saved_loop = g->loop_depth;
    int saved_try = g->try_depth;
    int saved_llvl_depth = g->llvl_depth;
    memset(&g->fn, 0, sizeof(g->fn));
    g->fn.in_function = 0;
    g->fn.owner_library = is_library ? name : NULL;
    g->current_loading_library = is_library ? name : saved_loading;
    g->loop_depth = 0;
    g->try_depth = 0;
    g->llvl_depth = 0;
    g->entry_alloca_out = NULL;

    FILE* saved_out = g->out;
    FILE* body_tmp = tmpfile();
    FILE* alloca_tmp = tmpfile();
    if (!body_tmp || !alloca_tmp) {
        if (body_tmp)
            fclose(body_tmp);
        if (alloca_tmp)
            fclose(alloca_tmp);
        body_tmp = NULL;
        alloca_tmp = NULL;
    }

    fprintf(saved_out, "define void %s() {\n", fn);
    fprintf(saved_out, "entry:\n");

    if (body_tmp && alloca_tmp) {
        g->out = body_tmp;
        g->entry_alloca_out = alloca_tmp;
        emit_stmt(g, program);
        if (g->ok)
            fprintf(g->out, "  ret void\n");

        cg_flush_temp(alloca_tmp, saved_out);
        cg_flush_temp(body_tmp, saved_out);
        fprintf(saved_out, "}\n\n");

        g->out = saved_out;
        g->entry_alloca_out = NULL;
        fclose(body_tmp);
        fclose(alloca_tmp);
    } else {
        emit_stmt(g, program);
        if (g->ok)
            fprintf(saved_out, "  ret void\n");
        fprintf(saved_out, "}\n\n");
    }

    g->current_loading_library = saved_loading;
    g->fn = saved_fn;
    g->loop_depth = saved_loop;
    g->try_depth = saved_try;
    g->llvl_depth = saved_llvl_depth;
}

static char* emit_string_ptr(LLVMGen* g, const char* text) {
    int idx = -1;
    for (int i = 0; i < g->strings.count; i++) {
        if (strcmp(g->strings.items[i].text, text) == 0) {
            idx = g->strings.items[i].id;
            break;
        }
    }
    if (idx < 0) {
        if (g && g->err && g->err_size > 0) {
            char preview[128];
            preview[0] = '\0';
            if (text) {
                size_t n = strlen(text);
                if (n > sizeof(preview) - 1)
                    n = sizeof(preview) - 1;
                memcpy(preview, text, n);
                preview[n] = '\0';
            }
            cg_set_err(g, "Missing string literal in table: `%s`", preview);
        } else {
            cg_set_err(g, "Missing string literal in table.");
        }
        return NULL;
    }
    char* ptr = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x i8], [%d x i8]* @.str%d, i32 0, i32 0\n",
        ptr,
        g->strings.items[idx].length,
        g->strings.items[idx].length,
        idx
    );
    return ptr;
}

static void emit_store_global(LLVMGen* g, int idx, const char* value_ptr) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable in assignment.");
        return;
    }
    char* ptr = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* @globals, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    char* loaded = cg_temp(g);
    fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, value_ptr);
    fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, ptr);
}

static void emit_store_local_value(LLVMGen* g, int idx, const char* value_ptr) {
    if (!g || idx < 0 || !g->fn.local_values) {
        cg_set_err(g, "Unknown local variable in assignment.");
        return;
    }
    char* loaded = cg_temp(g);
    fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, value_ptr);
    fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, g->fn.local_values[idx]);
}

static char* emit_load_global(LLVMGen* g, int idx) {
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference.");
        return NULL;
    }
    char* ptr = cg_temp(g);
    fprintf(g->out,
        "%s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* @globals, i32 0, i32 %d\n",
        ptr, g->symbols.count, g->symbols.count, idx
    );
    return ptr;
}

static char* emit_load_local(LLVMGen* g, int idx) {
    if (!g || idx < 0 || !g->fn.local_values) {
        cg_set_err(g, "Unknown local variable reference.");
        return NULL;
    }
    return g->fn.local_values[idx];
}

static char* emit_load_var_value(LLVMGen* g, const char* name) {
    int lidx = local_index(g, name);
    if (lidx >= 0)
        return emit_load_local(g, lidx);
    int idx = sym_index(&g->symbols, name);
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference: %s", name ? name : "<null>");
        return NULL;
    }
    return emit_load_global(g, idx);
}

static char* emit_load_var_int(LLVMGen* g, const char* name) {
    int lidx = local_index(g, name);
    if (lidx >= 0)
        return emit_load_local_int(g, lidx);
    int idx = sym_index(&g->symbols, name);
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference: %s", name ? name : "<null>");
        return NULL;
    }
    return emit_load_global_int(g, idx);
}

static char* emit_load_var_float(LLVMGen* g, const char* name) {
    int lidx = local_index(g, name);
    if (lidx >= 0)
        return emit_load_local_float(g, lidx);
    int idx = sym_index(&g->symbols, name);
    if (idx < 0) {
        cg_set_err(g, "Unknown variable reference: %s", name ? name : "<null>");
        return NULL;
    }
    return emit_load_global_float(g, idx);
}

static char* emit_expr(LLVMGen* g, const Expr* expr) {
    if (!expr) {
        cg_set_err(g, "Missing expression node.");
        return NULL;
    }
    switch (expr->type) {
        case EXPR_LITERAL: {
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %d)\n", out, expr->int_value);
            return out;
        }
        case EXPR_FLOAT_LITERAL: {
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_make_float(%%Value* %s, double %.17e)\n", out, expr->float_value);
            return out;
        }
        case EXPR_BOOL_LITERAL: {
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_make_bool(%%Value* %s, i32 %d)\n", out, expr->int_value ? 1 : 0);
            return out;
        }
        case EXPR_STRING_LITERAL: {
            char* ptr = emit_string_ptr(g, expr->string_value ? expr->string_value : "");
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_make_string(%%Value* %s, i8* %s)\n", out, ptr);
            return out;
        }
        case EXPR_VARIABLE: {
            return emit_load_var_value(g, expr->name);
        }
        case EXPR_CALL: {
            if (!expr->call_name || expr->call_name[0] == '\0') {
                cg_set_err(g, "Missing function name in call expression.");
                return NULL;
            }
            if (strcmp(expr->call_name, "args") == 0) {
                if (expr->call_arg_count != 0) {
                    cg_set_err(g, "Invalid args call");
                    return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_cli_args(%%Value* %s)\n", out);
                return out;
            }
            if (strcmp(expr->call_name, "native.http.get") == 0) {
                if (expr->call_arg_count < 1 || expr->call_arg_count > 2) {
                    cg_set_err(g, "Invalid native HTTP call");
                    return NULL;
                }
                char* url = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                const char* headers = "null";
                if (expr->call_arg_count == 2) {
                    headers = emit_expr(g, expr->call_args[1]);
                    if (!g->ok)
                        return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_http_get(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, url, headers);
                return out;
            }
            if (strcmp(expr->call_name, "native.http.post") == 0) {
                if (expr->call_arg_count < 2 || expr->call_arg_count > 3) {
                    cg_set_err(g, "Invalid native HTTP call");
                    return NULL;
                }
                char* url = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* body = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                const char* headers = "null";
                if (expr->call_arg_count == 3) {
                    headers = emit_expr(g, expr->call_args[2]);
                    if (!g->ok)
                        return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_http_post(%%Value* %s, %%Value* %s, %%Value* %s, %%Value* %s)\n", out, url, body, headers);
                return out;
            }
            if (strcmp(expr->call_name, "native.http.request") == 0) {
                if (expr->call_arg_count < 2 || expr->call_arg_count > 4) {
                    cg_set_err(g, "Invalid native HTTP call");
                    return NULL;
                }
                char* method = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* url = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                const char* body = "null";
                const char* headers = "null";
                if (expr->call_arg_count >= 3) {
                    body = emit_expr(g, expr->call_args[2]);
                    if (!g->ok)
                        return NULL;
                }
                if (expr->call_arg_count == 4) {
                    headers = emit_expr(g, expr->call_args[3]);
                    if (!g->ok)
                        return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_http_request(%%Value* %s, %%Value* %s, %%Value* %s, %%Value* %s, %%Value* %s)\n",
                    out, method, url, body, headers);
                return out;
            }
            if (strcmp(expr->call_name, "native.http.download") == 0) {
                if (expr->call_arg_count < 2 || expr->call_arg_count > 3) {
                    cg_set_err(g, "Invalid native HTTP call");
                    return NULL;
                }
                char* url = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* path = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                const char* headers = "null";
                if (expr->call_arg_count == 3) {
                    headers = emit_expr(g, expr->call_args[2]);
                    if (!g->ok)
                        return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_http_download(%%Value* %s, %%Value* %s, %%Value* %s, %%Value* %s)\n",
                    out, url, path, headers);
                return out;
            }
            if (strcmp(expr->call_name, "native.time.ms") == 0) {
                if (expr->call_arg_count != 0) {
                    cg_set_err(g, "Invalid native time call");
                    return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_time_ms(%%Value* %s)\n", out);
                return out;
            }
            if (strcmp(expr->call_name, "native.process.id") == 0) {
                if (expr->call_arg_count != 0) {
                    cg_set_err(g, "Invalid native process call");
                    return NULL;
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_process_id(%%Value* %s)\n", out);
                return out;
            }
            if (strcmp(expr->call_name, "native.process.run") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native process call");
                    return NULL;
                }
                char* cmd = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_process_run(%%Value* %s, %%Value* %s)\n", out, cmd);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.directory_exists") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_directory_exists(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.create_directory") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_create_directory(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.list_directories") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_list_directories(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.list_files") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_list_files(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.remove_directory") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_remove_directory(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.delete_file") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_delete_file(%%Value* %s, %%Value* %s)\n", out, path);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.copy_file") == 0) {
                if (expr->call_arg_count != 2) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* src = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* dst = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_copy_file(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, src, dst);
                return out;
            }
            if (strcmp(expr->call_name, "native.io.copy_directory") == 0) {
                if (expr->call_arg_count != 2) {
                    cg_set_err(g, "Invalid native IO call");
                    return NULL;
                }
                char* src = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* dst = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_io_copy_directory(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, src, dst);
                return out;
            }
            if (strcmp(expr->call_name, "native.env.get") == 0) {
                if (expr->call_arg_count != 1) {
                    cg_set_err(g, "Invalid native env call");
                    return NULL;
                }
                char* key = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_env_get(%%Value* %s, %%Value* %s)\n", out, key);
                return out;
            }
            if (strcmp(expr->call_name, "native.zip.extract") == 0) {
                if (expr->call_arg_count != 2) {
                    cg_set_err(g, "Invalid native zip call");
                    return NULL;
                }
                char* zip_path = emit_expr(g, expr->call_args[0]);
                if (!g->ok)
                    return NULL;
                char* dest = emit_expr(g, expr->call_args[1]);
                if (!g->ok)
                    return NULL;
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_native_zip_extract(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, zip_path, dest);
                return out;
            }
            FunctionEntry* fn = find_function(g, expr->call_name);
            if (!fn) {
                FunctionEntry* hidden = find_function_any(g, expr->call_name);
                if (hidden && hidden->owner_library && !function_is_accessible(g, hidden)) {
                    cg_set_err(g, "Function `%s` belongs to library `%s` and is not imported yet.",
                        expr->call_name, hidden->owner_library);
                    return NULL;
                }
                TypeEntry* type = find_type(g, expr->call_name);
                if (!type) {
                    cg_set_err(g, "Undefined callable in LLVM backend: %s", expr->call_name);
                    return NULL;
                }
                if (expr->call_arg_count != type->field_count) {
                    cg_set_err(g, "Type %s expects %d value(s), but got %d.",
                        expr->call_name, type->field_count, expr->call_arg_count);
                    return NULL;
                }
                int field_count = type->field_count;
                int assigned[64] = {0};
                int field_arg[64];
                if (field_count > 64 || expr->call_arg_count > 64) {
                    cg_set_err(g, "Type %s has too many fields for LLVM backend.", expr->call_name);
                    return NULL;
                }
                for (int i = 0; i < field_count; i++)
                    field_arg[i] = -1;
                int next_pos = 0;
                for (int i = 0; i < expr->call_arg_count; i++) {
                    if (expr->call_arg_names && expr->call_arg_names[i]) {
                        int idx = type_field_index(type, expr->call_arg_names[i]);
                        if (idx < 0) {
                            cg_set_err(g, "Type %s has no field named %s.",
                                expr->call_name, expr->call_arg_names[i]);
                            return NULL;
                        }
                        if (assigned[idx]) {
                            cg_set_err(g, "Field %s was provided more than once.", expr->call_arg_names[i]);
                            return NULL;
                        }
                        assigned[idx] = 1;
                        field_arg[idx] = i;
                        continue;
                    }
                    while (next_pos < field_count && assigned[next_pos])
                        next_pos++;
                    if (next_pos >= field_count) {
                        cg_set_err(g, "Too many positional values for type %s.", expr->call_name);
                        return NULL;
                    }
                    assigned[next_pos] = 1;
                    field_arg[next_pos] = i;
                    next_pos++;
                }
                for (int i = 0; i < field_count; i++) {
                    if (!assigned[i]) {
                        cg_set_err(g, "Missing value for field %s in %s.", type->node->type_decl.fields[i], expr->call_name);
                        return NULL;
                    }
                }
                char* out = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_dict_new(%%Value* %s, i32 %d)\n", out, field_count);
                for (int i = 0; i < field_count; i++) {
                    Expr* value_expr = expr->call_args[field_arg[i]];
                    char* value = emit_expr(g, value_expr);
                    if (!g->ok)
                        return NULL;
                    const char* field_name = type->node->type_decl.fields[i];
                    char* key_ptr = emit_string_ptr(g, field_name ? field_name : "");
                    char* key_val = cg_alloca_value(g);
                    fprintf(g->out, "call void @rt_make_string(%%Value* %s, i8* %s)\n", key_val, key_ptr);
                    fprintf(g->out, "call void @rt_dict_set(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, key_val, value);
                }
                return out;
            }
            int param_count = fn->param_count;
            int min_args = fn->required_param_count;
            if (param_count > 64 || expr->call_arg_count > 64) {
                cg_set_err(g, "Function %s has too many parameters for LLVM backend.", expr->call_name);
                return NULL;
            }
            if (expr->call_arg_count < min_args || expr->call_arg_count > param_count) {
                cg_set_err(g, "Invalid argument count for function %s.", expr->call_name);
                return NULL;
            }
            char* arg_values[64];
            for (int i = 0; i < expr->call_arg_count; i++) {
                arg_values[i] = emit_expr(g, expr->call_args[i]);
                if (!g->ok)
                    return NULL;
            }
            int assigned[64] = {0};
            int param_arg[64];
            for (int i = 0; i < param_count; i++)
                param_arg[i] = -1;
            int next_pos = 0;
            for (int i = 0; i < expr->call_arg_count; i++) {
                if (expr->call_arg_names && expr->call_arg_names[i]) {
                    int idx = function_param_index(fn->node, expr->call_arg_names[i]);
                    if (idx < 0) {
                        cg_set_err(g, "Function %s has no parameter named %s.", expr->call_name, expr->call_arg_names[i]);
                        return NULL;
                    }
                    if (assigned[idx]) {
                        cg_set_err(g, "Parameter %s was provided more than once.", expr->call_arg_names[i]);
                        return NULL;
                    }
                    assigned[idx] = 1;
                    param_arg[idx] = i;
                    continue;
                }
                while (next_pos < param_count && assigned[next_pos])
                    next_pos++;
                if (next_pos >= param_count) {
                    cg_set_err(g, "Too many positional arguments for function %s.", expr->call_name);
                    return NULL;
                }
                assigned[next_pos] = 1;
                param_arg[next_pos] = i;
                next_pos++;
            }
            char* param_values[64];
            for (int i = 0; i < param_count; i++) {
                if (assigned[i]) {
                    param_values[i] = arg_values[param_arg[i]];
                    continue;
                }
                Expr* default_expr = fn->node->function_decl.param_defaults[i];
                if (!default_expr) {
                    cg_set_err(g, "Missing required parameter %s.", fn->node->function_decl.params[i]);
                    return NULL;
                }
                param_values[i] = emit_expr(g, default_expr);
                if (!g->ok)
                    return NULL;
            }
            if (fn->is_generator) {
                char* out = cg_alloca_value(g);
                char* args_arr = NULL;
                if (param_count > 0) {
                    char* arr = cg_temp(g);
                    fprintf(g->out, "%s = alloca [%d x %%Value]\n", arr, param_count);
                    for (int i = 0; i < param_count; i++) {
                        char* loaded = cg_temp(g);
                        fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, param_values[i]);
                        char* elem_ptr = cg_temp(g);
                        fprintf(g->out,
                            "%s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* %s, i32 0, i32 %d\n",
                            elem_ptr, param_count, param_count, arr, i
                        );
                        fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, elem_ptr);
                    }
                    char* base_ptr = cg_temp(g);
                    fprintf(g->out,
                        "%s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* %s, i32 0, i32 0\n",
                        base_ptr, param_count, param_count, arr
                    );
                    args_arr = base_ptr;
                }
                char* fn_ptr = cg_temp(g);
                fprintf(g->out, "%s = bitcast void (%%Value*, %%Value*, i32)* %s to i8*\n",
                    fn_ptr, fn->gen_ir_name);
                fprintf(g->out, "call void @rt_make_generator(%%Value* %s, i8* %s, %%Value* %s, i32 %d)\n",
                    out, fn_ptr, args_arr ? args_arr : "null", param_count);
                return out;
            }
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void %s(%%Value* %s", fn->ir_name, out);
            for (int i = 0; i < param_count; i++) {
                fprintf(g->out, ", %%Value* %s", param_values[i]);
            }
            fprintf(g->out, ")\n");
            return out;
        }
        case EXPR_INPUT: {
            char* out = cg_alloca_value(g);
            if (expr->input_prompt && expr->input_prompt[0]) {
                char* prompt_ptr = emit_string_ptr(g, expr->input_prompt);
                fprintf(g->out, "call void @rt_input(%%Value* %s, i8* %s)\n", out, prompt_ptr);
            } else {
                fprintf(g->out, "call void @rt_input(%%Value* %s, i8* null)\n", out);
            }
            return out;
        }
        case EXPR_BUILTIN: {
            char* arg = emit_expr(g, expr->builtin_arg);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_builtin(%%Value* %s, i32 %d, %%Value* %s)\n",
                out, (int)expr->builtin_type, arg);
            return out;
        }
        case EXPR_CHAR_AT: {
            char* str_val = emit_expr(g, expr->char_string);
            if (!g->ok)
                return NULL;
            char* idx_val = emit_expr(g, expr->char_index);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_char_at(%%Value* %s, %%Value* %s, %%Value* %s)\n",
                out, str_val, idx_val);
            return out;
        }
        case EXPR_FILE_READ: {
            char* path_val = emit_expr(g, expr->file_read_path);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_file_read(%%Value* %s, %%Value* %s)\n", out, path_val);
            return out;
        }
        case EXPR_LLVL_VALUE_OF: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_get_value(%%Value* %s, %%Value* %s)\n", out, target);
            return out;
        }
        case EXPR_LLVL_BYTE_OF: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            char* index = emit_expr(g, expr->llvl_index);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_get_byte(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, target, index);
            return out;
        }
        case EXPR_LLVL_BIT_OF: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            char* index = emit_expr(g, expr->llvl_index);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_get_bit(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, target, index);
            return out;
        }
        case EXPR_LLVL_PLACE_OF: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_place_of(%%Value* %s, %%Value* %s)\n", out, target);
            return out;
        }
        case EXPR_LLVL_OFFSET: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* base = emit_expr(g, expr->llvl_target);
            char* offset = emit_expr(g, expr->llvl_index);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_offset(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, base, offset);
            return out;
        }
        case EXPR_LLVL_ATOMIC_READ: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_get_at(%%Value* %s, %%Value* %s)\n", out, target);
            return out;
        }
        case EXPR_LLVL_FIELD: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* target = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            const char* field_name = expr->name ? expr->name : "";
            char* field_ptr = emit_string_ptr(g, field_name);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_field_get(%%Value* %s, %%Value* %s, i8* %s)\n", out, target, field_ptr);
            return out;
        }
        case EXPR_LLVL_READ_PIN: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* pin = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_pin_read(%%Value* %s, %%Value* %s)\n", out, pin);
            return out;
        }
        case EXPR_LLVL_PORT_READ: {
            if (!emit_require_llvl_expr(g, expr))
                return NULL;
            char* port = emit_expr(g, expr->llvl_target);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_port_read(%%Value* %s, %%Value* %s)\n", out, port);
            return out;
        }
        case EXPR_LIST_ADD: {
            char* title_ptr = emit_string_ptr(g, "Invalid expression");
            char* msg_ptr = emit_string_ptr(g, "List add is a statement, not an expression.");
            fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 0)\n", out);
            return out;
        }
        case EXPR_LIST:
        case EXPR_ARRAY_LITERAL: {
            int count = expr->type == EXPR_LIST ? expr->list_count : expr->element_count;
            Expr** items = expr->type == EXPR_LIST ? expr->list_items : expr->elements;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_list_new(%%Value* %s, i32 %d)\n", out, count);
            for (int i = 0; i < count; i++) {
                char* item = emit_expr(g, items[i]);
                if (!g->ok)
                    return NULL;
                fprintf(g->out, "call void @rt_list_set_i32(%%Value* %s, i32 %d, %%Value* %s)\n", out, i, item);
            }
            return out;
        }
        case EXPR_DICT: {
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_dict_new(%%Value* %s, i32 %d)\n", out, expr->dict_count);
            for (int i = 0; i < expr->dict_count; i++) {
                char* key = emit_expr(g, expr->dict_keys[i]);
                if (!g->ok)
                    return NULL;
                char* val = emit_expr(g, expr->dict_values[i]);
                if (!g->ok)
                    return NULL;
                fprintf(g->out, "call void @rt_dict_set(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, key, val);
            }
            return out;
        }
        case EXPR_LIST_AT:
        case EXPR_ARRAY_INDEX: {
            Expr* list_expr = expr->type == EXPR_LIST_AT ? expr->list_expr : expr->array_expr;
            Expr* index_expr = expr->type == EXPR_LIST_AT ? expr->list_index : expr->index_expr;
            char* list_val = emit_expr(g, list_expr);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            if (expr_is_int(g, index_expr)) {
                char* idx_i32 = emit_expr_int(g, index_expr);
                if (!g->ok)
                    return NULL;
                fprintf(g->out, "call void @rt_list_get_i32(%%Value* %s, %%Value* %s, i32 %s)\n", out, list_val, idx_i32);
                return out;
            }
            char* idx_val = emit_expr(g, index_expr);
            if (!g->ok)
                return NULL;
            fprintf(g->out, "call void @rt_list_get(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, list_val, idx_val);
            return out;
        }
        case EXPR_DICT_GET: {
            char* dict_val = emit_expr(g, expr->dict_expr);
            if (!g->ok)
                return NULL;
            char* key_val = emit_expr(g, expr->dict_key);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_dict_get(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, dict_val, key_val);
            return out;
        }
        case EXPR_INDEX_OF: {
            char* value = emit_expr(g, expr->index_value);
            if (!g->ok)
                return NULL;
            char* array = emit_expr(g, expr->index_array);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_list_index_of(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, array, value);
            return out;
        }
        case EXPR_LIST_COMPREHENSION: {
            char* source_val = emit_expr(g, expr->comp_iterable);
            if (!g->ok)
                return NULL;
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_list_new(%%Value* %s, i32 0)\n", out);

            char* saved = cg_alloca_value(g);
            char* target_ptr = emit_load_var_value(g, expr->comp_var_name);
            if (!g->ok)
                return NULL;
            char* loaded = cg_temp(g);
            fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, target_ptr);
            fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, saved);

            char* count = cg_temp(g);
            fprintf(g->out, "%s = call i32 @rt_collection_count(%%Value* %s)\n", count, source_val);
            char* i_ptr = cg_alloca_i32(g);
            fprintf(g->out, "store i32 0, i32* %s\n", i_ptr);

            char* cond_label = cg_label(g, "compcond");
            char* body_label = cg_label(g, "compbody");
            char* end_label = cg_label(g, "compend");
            fprintf(g->out, "br label %%%s\n", cond_label);
            fprintf(g->out, "%s:\n", cond_label);
            char* cur = cg_temp(g);
            fprintf(g->out, "%s = load i32, i32* %s\n", cur, i_ptr);
            char* cmp = cg_temp(g);
            fprintf(g->out, "%s = icmp slt i32 %s, %s\n", cmp, cur, count);
            fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cmp, body_label, end_label);

            fprintf(g->out, "%s:\n", body_label);
            char* item_val = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_collection_item(%%Value* %s, %%Value* %s, i32 %s)\n", item_val, source_val, cur);
            int lidx = local_index(g, expr->comp_var_name);
            if (lidx >= 0) {
                emit_store_local_value(g, lidx, item_val);
            } else {
                int gidx = sym_index(&g->symbols, expr->comp_var_name);
                emit_store_global(g, gidx, item_val);
            }
            if (expr->comp_filter) {
                char* cond = emit_expr_cond_i1(g, expr->comp_filter);
                if (!g->ok)
                    return NULL;
                char* add_label = cg_label(g, "compadd");
                char* next_label = cg_label(g, "compnext");
                fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cond, add_label, next_label);
                fprintf(g->out, "%s:\n", add_label);
                char* res_val = emit_expr(g, expr->comp_result);
                if (!g->ok)
                    return NULL;
                fprintf(g->out, "call void @rt_list_add(%%Value* %s, %%Value* %s)\n", out, res_val);
                fprintf(g->out, "br label %%%s\n", next_label);
                fprintf(g->out, "%s:\n", next_label);
            } else {
                char* res_val = emit_expr(g, expr->comp_result);
                if (!g->ok)
                    return NULL;
                fprintf(g->out, "call void @rt_list_add(%%Value* %s, %%Value* %s)\n", out, res_val);
            }
            char* next = cg_temp(g);
            fprintf(g->out, "%s = add i32 %s, 1\n", next, cur);
            fprintf(g->out, "store i32 %s, i32* %s\n", next, i_ptr);
            fprintf(g->out, "br label %%%s\n", cond_label);
            fprintf(g->out, "%s:\n", end_label);

            char* saved_loaded = cg_temp(g);
            fprintf(g->out, "%s = load %%Value, %%Value* %s\n", saved_loaded, saved);
            fprintf(g->out, "store %%Value %s, %%Value* %s\n", saved_loaded, target_ptr);
            return out;
        }
        case EXPR_BINARY: {
            if (expr->op == OP_CONTAINS) {
                char* left = emit_expr(g, expr->left);
                char* right = emit_expr(g, expr->right);
                char* out = cg_alloca_value(g);
                if (!left || !right)
                    return NULL;
                fprintf(g->out, "call void @rt_contains(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                return out;
            }
            if (expr->op == OP_EQ || expr->op == OP_NE || expr->op == OP_GT ||
                expr->op == OP_LT || expr->op == OP_GTE || expr->op == OP_LTE) {
                if (expr_is_int(g, expr->left) && expr_is_int(g, expr->right)) {
                    char* li = emit_expr_int(g, expr->left);
                    char* ri = emit_expr_int(g, expr->right);
                    if (!li || !ri)
                        return NULL;
                    char* out = cg_alloca_value(g);
                    char* cmp = cg_temp(g);
                    const char* pred = "eq";
                    if (expr->op == OP_NE) pred = "ne";
                    else if (expr->op == OP_GT) pred = "sgt";
                    else if (expr->op == OP_LT) pred = "slt";
                    else if (expr->op == OP_GTE) pred = "sge";
                    else if (expr->op == OP_LTE) pred = "sle";
                    fprintf(g->out, "%s = icmp %s i32 %s, %s\n", cmp, pred, li, ri);
                    char* cmp_i32 = cg_temp(g);
                    fprintf(g->out, "%s = zext i1 %s to i32\n", cmp_i32, cmp);
                    fprintf(g->out, "call void @rt_make_bool(%%Value* %s, i32 %s)\n", out, cmp_i32);
                    return out;
                }
                if (expr_is_float(g, expr->left) && expr_is_float(g, expr->right)) {
                    char* lf = emit_expr_float(g, expr->left);
                    char* rf = emit_expr_float(g, expr->right);
                    if (!lf || !rf)
                        return NULL;
                    char* out = cg_alloca_value(g);
                    char* cmp = cg_temp(g);
                    const char* pred = "oeq";
                    if (expr->op == OP_NE) pred = "one";
                    else if (expr->op == OP_GT) pred = "ogt";
                    else if (expr->op == OP_LT) pred = "olt";
                    else if (expr->op == OP_GTE) pred = "oge";
                    else if (expr->op == OP_LTE) pred = "ole";
                    fprintf(g->out, "%s = fcmp %s double %s, %s\n", cmp, pred, lf, rf);
                    char* cmp_i32 = cg_temp(g);
                    fprintf(g->out, "%s = zext i1 %s to i32\n", cmp_i32, cmp);
                    fprintf(g->out, "call void @rt_make_bool(%%Value* %s, i32 %s)\n", out, cmp_i32);
                    return out;
                }
            }
            char* left = emit_expr(g, expr->left);
            char* right = emit_expr(g, expr->right);
            char* out = cg_alloca_value(g);
            if (!left || !right)
                return NULL;
            switch (expr->op) {
                case OP_ADD:
                    fprintf(g->out, "call void @rt_add(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_SUB:
                    fprintf(g->out, "call void @rt_sub(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_MUL:
                    fprintf(g->out, "call void @rt_mul(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_DIV:
                    fprintf(g->out, "call void @rt_div(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_EQ:
                    fprintf(g->out, "call void @rt_eq(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_NE:
                    fprintf(g->out, "call void @rt_ne(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_GT:
                    fprintf(g->out, "call void @rt_gt(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_LT:
                    fprintf(g->out, "call void @rt_lt(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_GTE:
                    fprintf(g->out, "call void @rt_gte(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_LTE:
                    fprintf(g->out, "call void @rt_lte(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_AND:
                    fprintf(g->out, "call void @rt_and(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                case OP_OR:
                    fprintf(g->out, "call void @rt_or(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, left, right);
                    return out;
                default:
                    cg_set_err(g, "Unsupported binary operator in LLVM backend.");
                    return NULL;
            }
        }
        case EXPR_UNARY: {
            char* v = emit_expr(g, expr->expr);
            char* out = cg_alloca_value(g);
            if (!v)
                return NULL;
            if (expr->op == OP_NOT) {
                fprintf(g->out, "call void @rt_not(%%Value* %s, %%Value* %s)\n", out, v);
                return out;
            }
            if (expr->op == OP_NEG) {
                fprintf(g->out, "call void @rt_neg(%%Value* %s, %%Value* %s)\n", out, v);
                return out;
            }
            cg_set_err(g, "Unsupported unary operator in LLVM backend.");
            return NULL;
        }
        case EXPR_CAST: {
            char* v = emit_expr(g, expr->cast_expr);
            char* out = cg_alloca_value(g);
            if (!v)
                return NULL;
            switch (expr->cast_type) {
                case CAST_TO_INTEGER:
                    fprintf(g->out, "call void @rt_cast_int(%%Value* %s, %%Value* %s)\n", out, v);
                    return out;
                case CAST_TO_FLOAT:
                    fprintf(g->out, "call void @rt_cast_float(%%Value* %s, %%Value* %s)\n", out, v);
                    return out;
                case CAST_TO_BOOLEAN:
                    fprintf(g->out, "call void @rt_cast_bool(%%Value* %s, %%Value* %s)\n", out, v);
                    return out;
                case CAST_TO_STRING:
                    fprintf(g->out, "call void @rt_cast_string(%%Value* %s, %%Value* %s)\n", out, v);
                    return out;
            }
            cg_set_err(g, "Unsupported cast target in LLVM backend.");
            return NULL;
        }
        default:
            cg_set_err(g, "Unsupported expression in LLVM backend: %s", expr_type_name(expr->type));
            return NULL;
    }
}

static void emit_stmt(LLVMGen* g, const ASTNode* node);

static void emit_block(LLVMGen* g, const ASTNode* node) {
    if (!node || !g || !g->ok)
        return;
    for (int i = 0; i < node->count; i++) {
        emit_gc_tick(g);
        emit_stmt(g, node->body[i]);
        if (!g->ok)
            return;
        if (g->fn.in_function && g->fn.terminated)
            return;
    }
}

static void emit_if(LLVMGen* g, const ASTNode* node) {
    char* end_label = cg_label(g, "ifend");
    int all_terminated = 1;
    for (int i = 0; i < node->branch_count; i++) {
        char* then_label = cg_label(g, "ifthen");
        char* next_label = cg_label(g, "ifnext");
        char* cond_i1 = emit_expr_cond_i1(g, node->branches[i].condition);
        if (!g->ok)
            return;
        fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cond_i1, then_label, next_label);
        fprintf(g->out, "%s:\n", then_label);
        g->fn.terminated = 0;
        emit_stmt(g, node->branches[i].block);
        if (!g->ok)
            return;
        if (!g->fn.terminated) {
            fprintf(g->out, "br label %%%s\n", end_label);
            all_terminated = 0;
        }
        fprintf(g->out, "%s:\n", next_label);
        g->fn.terminated = 0;
    }

    if (node->else_block) {
        g->fn.terminated = 0;
        emit_stmt(g, node->else_block);
        if (!g->ok)
            return;
        if (!g->fn.terminated) {
            fprintf(g->out, "br label %%%s\n", end_label);
            all_terminated = 0;
        }
        g->fn.terminated = 0;
    } else {
        fprintf(g->out, "br label %%%s\n", end_label);
        all_terminated = 0;
    }
    fprintf(g->out, "%s:\n", end_label);
    if (!g->fn.has_return)
        g->fn.terminated = all_terminated ? 1 : 0;
}

static void emit_while(LLVMGen* g, const ASTNode* node) {
    char* cond_label = cg_label(g, "whilecond");
    char* body_label = cg_label(g, "whilebody");
    char* end_label = cg_label(g, "whileend");
    char* limit_val = NULL;
    char* limit_ptr = NULL;
    char* limit_check_label = NULL;

    if (node->while_stmt.repeat_limit) {
        if (expr_is_int(g, node->while_stmt.repeat_limit)) {
            limit_val = emit_expr_int(g, node->while_stmt.repeat_limit);
            if (!g->ok)
                return;
        } else {
            char* limit_expr = emit_expr(g, node->while_stmt.repeat_limit);
            if (!g->ok)
                return;
            limit_val = cg_temp(g);
            fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", limit_val, limit_expr);
        }

        {
            char* neg = cg_temp(g);
            fprintf(g->out, "%s = icmp slt i32 %s, 0\n", neg, limit_val);
            char* neg_label = cg_label(g, "whileneg");
            char* ok_label = cg_label(g, "whilelimitok");
            fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", neg, neg_label, ok_label);
            fprintf(g->out, "%s:\n", neg_label);
            {
                char* title_ptr = emit_string_ptr(g, "Invalid while repeat limit");
                char* msg_ptr = emit_string_ptr(g, "Repeat limit cannot be negative.");
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
                fprintf(g->out, "br label %%%s\n", end_label);
            }
            fprintf(g->out, "%s:\n", ok_label);
        }

        limit_ptr = cg_alloca_i32(g);
        fprintf(g->out, "store i32 %s, i32* %s\n", limit_val, limit_ptr);
        limit_check_label = cg_label(g, "whilelimitcheck");
    }

    char* iter_ptr = NULL;
    if (node->while_stmt.repeat_limit) {
        iter_ptr = cg_alloca_i32(g);
        fprintf(g->out, "store i32 0, i32* %s\n", iter_ptr);
    }
    if (g->loop_depth < 64) {
        g->loop_break_labels[g->loop_depth] = end_label;
        g->loop_continue_labels[g->loop_depth] = cond_label;
        g->loop_depth++;
    } else {
        cg_set_err(g, "Too many nested loops in LLVM backend.");
        return;
    }

    fprintf(g->out, "br label %%%s\n", cond_label);
    fprintf(g->out, "%s:\n", cond_label);
    emit_gc_tick(g);
    if (node->while_stmt.repeat_limit) {
        fprintf(g->out, "br label %%%s\n", limit_check_label);
        fprintf(g->out, "%s:\n", limit_check_label);
        char* iter_val = cg_temp(g);
        fprintf(g->out, "%s = load i32, i32* %s\n", iter_val, iter_ptr);
        char* limit_cur = cg_temp(g);
        fprintf(g->out, "%s = load i32, i32* %s\n", limit_cur, limit_ptr);
        char* limit_done = cg_temp(g);
        fprintf(g->out, "%s = icmp sge i32 %s, %s\n", limit_done, iter_val, limit_cur);
        char* limit_ok_label = cg_label(g, "whilecondok");
        fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", limit_done, end_label, limit_ok_label);
        fprintf(g->out, "%s:\n", limit_ok_label);
    }
    char* cond_i1 = emit_expr_cond_i1(g, node->while_stmt.condition);
    if (!g->ok)
        return;
    fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cond_i1, body_label, end_label);

    fprintf(g->out, "%s:\n", body_label);
    g->fn.terminated = 0;
    if (node->while_stmt.repeat_limit) {
        char* iter_val = cg_temp(g);
        fprintf(g->out, "%s = load i32, i32* %s\n", iter_val, iter_ptr);
        char* next_iter = cg_temp(g);
        fprintf(g->out, "%s = add i32 %s, 1\n", next_iter, iter_val);
        fprintf(g->out, "store i32 %s, i32* %s\n", next_iter, iter_ptr);
    }
    emit_stmt(g, node->while_stmt.body);
    if (!g->ok)
        return;
    if (!g->fn.terminated)
        fprintf(g->out, "br label %%%s\n", cond_label);
    fprintf(g->out, "%s:\n", end_label);
    g->loop_depth--;
    g->fn.terminated = 0;
}

static void emit_repeat(LLVMGen* g, const ASTNode* node) {
    char* cond_label = cg_label(g, "repeatcond");
    char* body_label = cg_label(g, "repeatbody");
    char* end_label = cg_label(g, "repeatend");
    if (g->loop_depth < 64) {
        g->loop_break_labels[g->loop_depth] = end_label;
        g->loop_continue_labels[g->loop_depth] = cond_label;
        g->loop_depth++;
    } else {
        cg_set_err(g, "Too many nested loops in LLVM backend.");
        return;
    }

    char* times_int = NULL;
    if (expr_is_int(g, node->repeat_stmt.times)) {
        times_int = emit_expr_int(g, node->repeat_stmt.times);
        if (!g->ok)
            return;
    } else {
        char* times_val = emit_expr(g, node->repeat_stmt.times);
        if (!g->ok)
            return;
        times_int = cg_temp(g);
        fprintf(g->out, "%s = call i32 @rt_get_int(%%Value* %s)\n", times_int, times_val);
    }

    {
        char* neg = cg_temp(g);
        fprintf(g->out, "%s = icmp slt i32 %s, 0\n", neg, times_int);
        char* neg_label = cg_label(g, "repeatneg");
        char* start_label = cg_label(g, "repeatstart");
        fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", neg, neg_label, start_label);
        fprintf(g->out, "%s:\n", neg_label);
        {
            char* title_ptr = emit_string_ptr(g, "Invalid repeat count");
            char* msg_ptr = emit_string_ptr(g, "Repeat count cannot be negative.");
            if (!g->ok)
                return;
            fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
            fprintf(g->out, "br label %%%s\n", end_label);
        }
        fprintf(g->out, "%s:\n", start_label);
    }

    const ASTNode* body_node = unwrap_single_stmt_block(node->repeat_stmt.body);
    if (body_node && body_node->type == AST_PRINT_STRING) {
        char* joined = build_print_string_line(body_node);
        if (joined) {
            char* ptr = emit_string_ptr(g, joined);
            free(joined);
            if (!ptr)
                return;
            fprintf(g->out, "call void @rt_print_text_repeat_checked(i8* %s, i32 %s)\n", ptr, times_int);
            fprintf(g->out, "br label %%%s\n", end_label);
            fprintf(g->out, "%s:\n", end_label);
            g->loop_depth--;
            g->fn.terminated = 0;
            return;
        }
    }

    char* counter_ptr = cg_alloca_i32(g);
    fprintf(g->out, "store i32 0, i32* %s\n", counter_ptr);

    fprintf(g->out, "br label %%%s\n", cond_label);
    fprintf(g->out, "%s:\n", cond_label);
    char* cur = cg_temp(g);
    fprintf(g->out, "%s = load i32, i32* %s\n", cur, counter_ptr);
    char* cmp = cg_temp(g);
    fprintf(g->out, "%s = icmp slt i32 %s, %s\n", cmp, cur, times_int);
    fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cmp, body_label, end_label);

    fprintf(g->out, "%s:\n", body_label);
    emit_gc_tick(g);
    {
        char* next = cg_temp(g);
        fprintf(g->out, "%s = add i32 %s, 1\n", next, cur);
        fprintf(g->out, "store i32 %s, i32* %s\n", next, counter_ptr);
    }
    g->fn.terminated = 0;
    emit_stmt(g, node->repeat_stmt.body);
    if (!g->ok)
        return;
    if (!g->fn.terminated) {
        fprintf(g->out, "br label %%%s\n", cond_label);
    }
    fprintf(g->out, "%s:\n", end_label);
    g->loop_depth--;
    g->fn.terminated = 0;
}

static int llvl_is_active(const LLVMGen* g) {
    return g && g->llvl_depth > 0;
}

static void emit_llvl_enter(LLVMGen* g) {
    if (!g || !g->ok)
        return;
    g->llvl_depth++;
    if (g->llvl_depth == 1)
        fprintf(g->out, "call void @rt_llvl_set_gc_paused(i32 1)\n");
}

static void emit_llvl_exit(LLVMGen* g) {
    if (!g || !g->ok)
        return;
    if (g->llvl_depth <= 0)
        return;
    g->llvl_depth--;
    if (g->llvl_depth == 0)
        fprintf(g->out, "call void @rt_llvl_set_gc_paused(i32 0)\n");
}

static int emit_require_llvl(LLVMGen* g, const ASTNode* node) {
    if (!g || !g->ok)
        return 0;
    if (llvl_is_active(g))
        return 1;
    (void)node;
    fprintf(g->out, "call void @rt_llvl_require()\n");
    return 1;
}

static int emit_require_llvl_expr(LLVMGen* g, const Expr* expr) {
    if (!g || !g->ok)
        return 0;
    if (llvl_is_active(g))
        return 1;
    (void)expr;
    fprintf(g->out, "call void @rt_llvl_require()\n");
    return 1;
}

static void emit_stmt(LLVMGen* g, const ASTNode* node) {
    if (!node || !g || !g->ok)
        return;
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            if (node->llvl_mode) {
                emit_llvl_enter(g);
                emit_block(g, node);
                emit_llvl_exit(g);
            } else {
                emit_block(g, node);
            }
            return;
        case AST_LLVL_BLOCK:
            emit_llvl_enter(g);
            emit_block(g, node);
            emit_llvl_exit(g);
            return;
        case AST_SET: {
            int lidx = local_index(g, node->name);
            int idx = lidx >= 0 ? -1 : sym_index(&g->symbols, node->name);
            if (expr_is_int(g, node->expr)) {
                char* value_i32 = emit_expr_int(g, node->expr);
                if (!g->ok)
                    return;
                if (lidx >= 0) {
                    emit_store_local_int(g, lidx, value_i32);
                    char* boxed = cg_alloca_value(g);
                    fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %s)\n", boxed, value_i32);
                    emit_store_local_value(g, lidx, boxed);
                    if (g->fn.int_known)
                        g->fn.int_known[lidx] = 1;
                    if (g->fn.float_known)
                        g->fn.float_known[lidx] = 0;
                } else {
                    emit_store_global_int(g, idx, value_i32);
                    char* boxed = cg_alloca_value(g);
                    fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %s)\n", boxed, value_i32);
                    emit_store_global(g, idx, boxed);
                    if (g->int_known && idx >= 0)
                        g->int_known[idx] = 1;
                    if (g->float_known && idx >= 0)
                        g->float_known[idx] = 0;
                }
                return;
            }
            if (expr_is_float(g, node->expr)) {
                char* value_f64 = emit_expr_float(g, node->expr);
                if (!g->ok)
                    return;
                if (lidx >= 0) {
                    emit_store_local_float(g, lidx, value_f64);
                    char* boxed = cg_alloca_value(g);
                    fprintf(g->out, "call void @rt_make_float(%%Value* %s, double %s)\n", boxed, value_f64);
                    emit_store_local_value(g, lidx, boxed);
                    if (g->fn.float_known)
                        g->fn.float_known[lidx] = 1;
                    if (g->fn.int_known)
                        g->fn.int_known[lidx] = 0;
                } else {
                    emit_store_global_float(g, idx, value_f64);
                    char* boxed = cg_alloca_value(g);
                    fprintf(g->out, "call void @rt_make_float(%%Value* %s, double %s)\n", boxed, value_f64);
                    emit_store_global(g, idx, boxed);
                    if (g->float_known && idx >= 0)
                        g->float_known[idx] = 1;
                    if (g->int_known && idx >= 0)
                        g->int_known[idx] = 0;
                }
                return;
            }
            char* value = emit_expr(g, node->expr);
            if (!g->ok)
                return;
            if (lidx >= 0) {
                emit_store_local_value(g, lidx, value);
                if (g->fn.int_known)
                    g->fn.int_known[lidx] = 0;
                if (g->fn.float_known)
                    g->fn.float_known[lidx] = 0;
            } else {
                emit_store_global(g, idx, value);
                if (g->int_known && idx >= 0)
                    g->int_known[idx] = 0;
                if (g->float_known && idx >= 0)
                    g->float_known[idx] = 0;
            }
            return;
        }
        case AST_PRINT_EXPR: {
            if (expr_is_int(g, node->expr)) {
                char* v = emit_expr_int(g, node->expr);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_print_int(i32 %s)\n", v);
                fprintf(g->out, "call void @rt_print_newline()\n");
                return;
            }
            if (expr_is_float(g, node->expr)) {
                char* v = emit_expr_float(g, node->expr);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_print_float(double %s)\n", v);
                fprintf(g->out, "call void @rt_print_newline()\n");
                return;
            }
            char* v = emit_expr(g, node->expr);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_print_inline(%%Value* %s)\n", v);
            fprintf(g->out, "call void @rt_print_newline()\n");
            return;
        }
        case AST_PRINT_STRING: {
            int all_text = 1;
            for (int i = 0; i < node->part_count; i++) {
                if (node->parts[i].type != STR_TEXT) {
                    all_text = 0;
                    break;
                }
            }
            if (all_text) {
                char* joined = build_print_string_line(node);
                if (!joined) {
                    cg_set_err(g, "Failed to build print string literal.");
                    return;
                }
                char* ptr = emit_string_ptr(g, joined);
                free(joined);
                if (!ptr)
                    return;
                fprintf(g->out, "call void @rt_print_text(i8* %s)\n", ptr);
                return;
            }
            for (int i = 0; i < node->part_count; i++) {
                StringPart* sp = &node->parts[i];
                if (sp->type == STR_TEXT) {
                    char* ptr = emit_string_ptr(g, sp->text ? sp->text : "");
                    fprintf(g->out, "call void @rt_print_text(i8* %s)\n", ptr);
                } else {
                    char* v = emit_expr(g, sp->expr);
                    if (!g->ok)
                        return;
                    fprintf(g->out, "call void @rt_print_inline(%%Value* %s)\n", v);
                }
            }
            fprintf(g->out, "call void @rt_print_newline()\n");
            return;
        }
        case AST_LLVL_SAVE: {
            if (!emit_require_llvl(g, node))
                return;
            char* size = emit_expr(g, node->llvl_save.size);
            if (!g->ok)
                return;
            size_t elem_size = 0;
            int elem_kind = (int)BUFFER_ELEM_RAW;
            const char* elem_type_name = NULL;
            if (node->llvl_save.has_type) {
                elem_size = llvl_type_size_from_kind(g, node->llvl_save.elem_kind, node->llvl_save.elem_type_name);
                if (!g->ok)
                    return;
                elem_kind = llvl_kind_to_buffer_kind(node->llvl_save.elem_kind);
                elem_type_name = node->llvl_save.elem_type_name;
                char* elem_val = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %u)\n", elem_val, (unsigned int)elem_size);
                char* total = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_mul(%%Value* %s, %%Value* %s, %%Value* %s)\n", total, size, elem_val);
                size = total;
            }
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_alloc(%%Value* %s, %%Value* %s)\n", out, size);
            if (node->llvl_save.has_type) {
                char* type_ptr = elem_type_name ? emit_string_ptr(g, elem_type_name) : "null";
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_llvl_set_buffer_meta(%%Value* %s, i64 %llu, i32 %d, i8* %s)\n",
                    out, (unsigned long long)elem_size, elem_kind, type_ptr);
            }
            int lidx = local_index(g, node->llvl_save.name);
            int idx = lidx >= 0 ? -1 : sym_index(&g->symbols, node->llvl_save.name);
            if (lidx >= 0) {
                emit_store_local_value(g, lidx, out);
                if (g->fn.int_known)
                    g->fn.int_known[lidx] = 0;
                if (g->fn.float_known)
                    g->fn.float_known[lidx] = 0;
            } else if (idx >= 0) {
                emit_store_global(g, idx, out);
                if (g->int_known)
                    g->int_known[idx] = 0;
                if (g->float_known)
                    g->float_known[idx] = 0;
            } else {
                cg_set_err(g, "Unknown variable reference: %s", node->llvl_save.name ? node->llvl_save.name : "<null>");
            }
            return;
        }
        case AST_LLVL_REMOVE: {
            if (!emit_require_llvl(g, node))
                return;
            char* buf = emit_load_var_value(g, node->llvl_remove.name);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_free(%%Value* %s)\n", buf);
            return;
        }
        case AST_LLVL_RESIZE: {
            if (!emit_require_llvl(g, node))
                return;
            char* buf = emit_load_var_value(g, node->llvl_resize.name);
            char* size = emit_expr(g, node->llvl_resize.size);
            if (!g->ok)
                return;
            size_t elem_size = 0;
            int elem_kind = (int)BUFFER_ELEM_RAW;
            const char* elem_type_name = NULL;
            if (node->llvl_resize.has_type) {
                elem_size = llvl_type_size_from_kind(g, node->llvl_resize.elem_kind, node->llvl_resize.elem_type_name);
                if (!g->ok)
                    return;
                elem_kind = llvl_kind_to_buffer_kind(node->llvl_resize.elem_kind);
                elem_type_name = node->llvl_resize.elem_type_name;
                char* elem_val = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %u)\n", elem_val, (unsigned int)elem_size);
                char* total = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_mul(%%Value* %s, %%Value* %s, %%Value* %s)\n", total, size, elem_val);
                size = total;
                char* type_ptr = elem_type_name ? emit_string_ptr(g, elem_type_name) : "null";
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_llvl_set_buffer_meta(%%Value* %s, i64 %llu, i32 %d, i8* %s)\n",
                    buf, (unsigned long long)elem_size, elem_kind, type_ptr);
            }
            if (node->llvl_resize.op == LLVL_RESIZE_ANY) {
                fprintf(g->out, "call void @rt_llvl_resize_any(%%Value* %s, %%Value* %s)\n", buf, size);
            } else {
                int is_grow = node->llvl_resize.op == LLVL_RESIZE_GROW ? 1 : 0;
                fprintf(g->out, "call void @rt_llvl_resize(%%Value* %s, %%Value* %s, i32 %d)\n", buf, size, is_grow);
            }
            return;
        }
        case AST_LLVL_COPY: {
            if (!emit_require_llvl(g, node))
                return;
            char* src = emit_load_var_value(g, node->llvl_copy.src);
            if (!g->ok)
                return;
            if (node->llvl_copy.has_size) {
                char* dst = emit_load_var_value(g, node->llvl_copy.dest);
                char* size = emit_expr(g, node->llvl_copy.size);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_llvl_copy_bytes(%%Value* %s, %%Value* %s, %%Value* %s)\n", src, dst, size);
                return;
            }
            char* out = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_copy(%%Value* %s, %%Value* %s)\n", out, src);
            int lidx = local_index(g, node->llvl_copy.dest);
            int idx = lidx >= 0 ? -1 : sym_index(&g->symbols, node->llvl_copy.dest);
            if (lidx >= 0) {
                emit_store_local_value(g, lidx, out);
                if (g->fn.int_known)
                    g->fn.int_known[lidx] = 0;
                if (g->fn.float_known)
                    g->fn.float_known[lidx] = 0;
            } else if (idx >= 0) {
                emit_store_global(g, idx, out);
                if (g->int_known)
                    g->int_known[idx] = 0;
                if (g->float_known)
                    g->float_known[idx] = 0;
            } else {
                cg_set_err(g, "Unknown variable reference: %s", node->llvl_copy.dest ? node->llvl_copy.dest : "<null>");
            }
            return;
        }
        case AST_LLVL_MOVE: {
            if (!emit_require_llvl(g, node))
                return;
            char* src = emit_load_var_value(g, node->llvl_move.src);
            char* dst = emit_load_var_value(g, node->llvl_move.dest);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_move(%%Value* %s, %%Value* %s)\n", dst, src);
            return;
        }
        case AST_LLVL_SET_VALUE: {
            if (!emit_require_llvl(g, node))
                return;
            char* buf = emit_load_var_value(g, node->llvl_set_value.name);
            char* value = emit_expr(g, node->llvl_set_value.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_set_value(%%Value* %s, %%Value* %s)\n", buf, value);
            return;
        }
        case AST_LLVL_SET_BYTE: {
            if (!emit_require_llvl(g, node))
                return;
            char* buf = emit_load_var_value(g, node->llvl_set_byte.name);
            char* index = emit_expr(g, node->llvl_set_byte.index);
            char* value = emit_expr(g, node->llvl_set_byte.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_set_byte(%%Value* %s, %%Value* %s, %%Value* %s)\n", buf, index, value);
            return;
        }
        case AST_LLVL_BIT_OP: {
            if (!emit_require_llvl(g, node))
                return;
            char* buf = emit_load_var_value(g, node->llvl_bit_op.name);
            char* index = emit_expr(g, node->llvl_bit_op.index);
            if (!g->ok)
                return;
            int op = 0;
            if (node->llvl_bit_op.op == LLVL_BIT_CLEAR)
                op = 1;
            else if (node->llvl_bit_op.op == LLVL_BIT_FLIP)
                op = 2;
            fprintf(g->out, "call void @rt_llvl_bit_op(%%Value* %s, %%Value* %s, i32 %d)\n", buf, index, op);
            return;
        }
        case AST_LLVL_SET_AT: {
            if (!emit_require_llvl(g, node))
                return;
            char* addr = emit_expr(g, node->llvl_set_at.address);
            char* value = emit_expr(g, node->llvl_set_at.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_set_at(%%Value* %s, %%Value* %s)\n", addr, value);
            return;
        }
        case AST_LLVL_ATOMIC_OP: {
            if (!emit_require_llvl(g, node))
                return;
            char* addr = emit_expr(g, node->llvl_atomic_op.address);
            if (!g->ok)
                return;
            if (node->llvl_atomic_op.op == LLVL_ATOMIC_WRITE) {
                char* value = emit_expr(g, node->llvl_atomic_op.value);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_llvl_set_at(%%Value* %s, %%Value* %s)\n", addr, value);
                return;
            }
            char* current = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_llvl_get_at(%%Value* %s, %%Value* %s)\n", current, addr);
            char* delta = emit_expr(g, node->llvl_atomic_op.value);
            if (!g->ok)
                return;
            char* out = cg_alloca_value(g);
            if (node->llvl_atomic_op.op == LLVL_ATOMIC_ADD) {
                fprintf(g->out, "call void @rt_add(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, current, delta);
            } else {
                fprintf(g->out, "call void @rt_sub(%%Value* %s, %%Value* %s, %%Value* %s)\n", out, current, delta);
            }
            fprintf(g->out, "call void @rt_llvl_set_at(%%Value* %s, %%Value* %s)\n", addr, out);
            return;
        }
        case AST_LLVL_MARK_VOLATILE: {
            if (!emit_require_llvl(g, node))
                return;
            char* target = emit_expr(g, node->llvl_mark_volatile.target);
            if (!g->ok)
                return;
            (void)target;
            return;
        }
        case AST_LLVL_SET_CHECK: {
            if (!emit_require_llvl(g, node))
                return;
            int enabled = node->llvl_set_check.enabled ? 1 : 0;
            if (node->llvl_set_check.kind == LLVL_CHECK_BOUNDS) {
                fprintf(g->out, "call void @rt_llvl_set_bounds_check(i32 %d)\n", enabled);
            } else {
                fprintf(g->out, "call void @rt_llvl_set_pointer_checks(i32 %d)\n", enabled);
            }
            return;
        }
        case AST_LLVL_PORT_WRITE: {
            if (!emit_require_llvl(g, node))
                return;
            char* port = emit_expr(g, node->llvl_port_write.port);
            char* value = emit_expr(g, node->llvl_port_write.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_port_write(%%Value* %s, %%Value* %s)\n", port, value);
            return;
        }
        case AST_LLVL_REGISTER_INTERRUPT: {
            if (!emit_require_llvl(g, node))
                return;
            char* id_val = emit_expr(g, node->llvl_register_interrupt.interrupt_id);
            if (!g->ok)
                return;
            const char* handler_name = node->llvl_register_interrupt.handler_name
                ? node->llvl_register_interrupt.handler_name
                : "";
            char* handler_ptr = emit_string_ptr(g, handler_name);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_register_interrupt(%%Value* %s, i8* %s)\n", id_val, handler_ptr);
            return;
        }
        case AST_LLVL_SET_FIELD: {
            if (!emit_require_llvl(g, node))
                return;
            char* target = emit_expr(g, node->llvl_set_field.target);
            char* value = emit_expr(g, node->llvl_set_field.value);
            if (!g->ok)
                return;
            const char* field_name = node->llvl_set_field.field_name ? node->llvl_set_field.field_name : "";
            char* field_ptr = emit_string_ptr(g, field_name);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_field_set(%%Value* %s, i8* %s, %%Value* %s)\n", target, field_ptr, value);
            return;
        }
        case AST_LLVL_PIN_WRITE: {
            if (!emit_require_llvl(g, node))
                return;
            char* value = emit_expr(g, node->llvl_pin_write.value);
            char* pin = emit_expr(g, node->llvl_pin_write.pin);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_pin_write(%%Value* %s, %%Value* %s)\n", value, pin);
            return;
        }
        case AST_LLVL_WAIT: {
            if (!emit_require_llvl(g, node))
                return;
            char* duration = emit_expr(g, node->llvl_wait.duration);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_wait_ms(%%Value* %s)\n", duration);
            return;
        }
        case AST_LLVL_STRUCT_DECL:
        case AST_LLVL_UNION_DECL: {
            int is_union = node->type == AST_LLVL_UNION_DECL ? 1 : 0;
            add_llvl_type(g, node, is_union);
            if (!g->ok)
                return;
            const char* type_name = is_union ? node->llvl_union_decl.name : node->llvl_struct_decl.name;
            LlvlTypeEntry* def = find_llvl_type(g, type_name);
            if (!def)
                return;
            char* type_ptr = emit_string_ptr(g, def->name ? def->name : "");
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_llvl_register_type(i8* %s, i32 %d, i32 %d, i64 %llu)\n",
                type_ptr, is_union, def->field_count, (unsigned long long)def->size);
            for (int i = 0; i < def->field_count; i++) {
                const char* fname = def->fields[i].name ? def->fields[i].name : "";
                char* field_ptr = emit_string_ptr(g, fname);
                if (!g->ok)
                    return;
                fprintf(g->out,
                    "call void @rt_llvl_register_field(i8* %s, i32 %d, i8* %s, i32 %d, i32 %d, i64 %llu, i64 %llu)\n",
                    type_ptr,
                    i,
                    field_ptr,
                    (int)def->fields[i].kind,
                    def->fields[i].array_len,
                    (unsigned long long)def->fields[i].offset,
                    (unsigned long long)def->fields[i].size
                );
            }
            return;
        }
        case AST_LLVL_ENUM_DECL: {
            for (int i = 0; i < node->llvl_enum_decl.count; i++) {
                const char* name = node->llvl_enum_decl.names[i];
                if (!name)
                    continue;
                int idx = sym_index(&g->symbols, name);
                if (idx < 0) {
                    cg_set_err(g, "Unknown enum symbol: %s", name);
                    return;
                }
                char* v = cg_temp(g);
                fprintf(g->out, "%s = add i32 0, %d\n", v, node->llvl_enum_decl.values[i]);
                emit_store_global_int(g, idx, v);
                char* boxed = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %d)\n", boxed, node->llvl_enum_decl.values[i]);
                emit_store_global(g, idx, boxed);
                if (g->int_known)
                    g->int_known[idx] = 1;
                if (g->float_known)
                    g->float_known[idx] = 0;
            }
            return;
        }
        case AST_LLVL_BITFIELD_DECL: {
            for (int i = 0; i < node->llvl_bitfield_decl.count; i++) {
                const char* name = node->llvl_bitfield_decl.names[i];
                if (!name)
                    continue;
                int idx = sym_index(&g->symbols, name);
                if (idx < 0) {
                    cg_set_err(g, "Unknown bitfield symbol: %s", name);
                    return;
                }
                char* v = cg_temp(g);
                fprintf(g->out, "%s = add i32 0, %d\n", v, node->llvl_bitfield_decl.bits[i]);
                emit_store_global_int(g, idx, v);
                char* boxed = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %d)\n", boxed, node->llvl_bitfield_decl.bits[i]);
                emit_store_global(g, idx, boxed);
                if (g->int_known)
                    g->int_known[idx] = 1;
                if (g->float_known)
                    g->float_known[idx] = 0;
            }
            return;
        }
        case AST_FUNCTION:
            return;
        case AST_RETURN: {
            if (!g->fn.in_function) {
                cg_set_err(g, "return can only be used inside a function.");
                return;
            }
            if (g->fn.is_generator) {
                char* title_ptr = emit_string_ptr(g, "Invalid generator flow");
                char* msg_ptr = emit_string_ptr(g, "Generator function cannot use `return`.");
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
                fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
                g->fn.terminated = 1;
                g->fn.has_return = 1;
                return;
            }
            if (expr_is_int(g, node->return_stmt.value)) {
                char* v = emit_expr_int(g, node->return_stmt.value);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_make_int(%%Value* %s, i32 %s)\n", g->fn.ret_ptr, v);
            } else if (expr_is_float(g, node->return_stmt.value)) {
                char* v = emit_expr_float(g, node->return_stmt.value);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_make_float(%%Value* %s, double %s)\n", g->fn.ret_ptr, v);
            } else {
                char* value = emit_expr(g, node->return_stmt.value);
                if (!g->ok)
                    return;
                char* loaded = cg_temp(g);
                fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, value);
                fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, g->fn.ret_ptr);
            }
            for (int i = g->try_depth - 1; i >= 0; i--) {
                fprintf(g->out, "call void @runtime_try_end(ptr %s)\n", g->try_envs[i]);
            }
            fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
            g->fn.terminated = 1;
            g->fn.has_return = 1;
            return;
        }
        case AST_YIELD: {
            if (!g->fn.in_function) {
                cg_set_err(g, "yield can only be used inside a function.");
                return;
            }
            if (!g->fn.is_generator) {
                char* title_ptr = emit_string_ptr(g, "Invalid yield");
                char* msg_ptr = emit_string_ptr(g, "`yield` can only be used inside a generator function.");
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
                fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
                g->fn.terminated = 1;
                return;
            }
            char* value = emit_expr(g, node->expr);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_gen_yield(%%Value* %s, %%Value* %s)\n", g->fn.gen_out_list, value);
            return;
        }
        case AST_EXPR_STMT: {
            (void)emit_expr(g, node->expr);
            return;
        }
        case AST_IF:
            emit_if(g, node);
            return;
        case AST_WHILE:
            emit_while(g, node);
            return;
        case AST_REPEAT:
            emit_repeat(g, node);
            return;
        case AST_TRY: {
            char* env = cg_temp(g);
            fprintf(g->out, "%s = call ptr @runtime_try_alloc()\n", env);
            {
                char* env_ok = cg_temp(g);
                fprintf(g->out, "%s = icmp ne ptr %s, null\n", env_ok, env);
                char* alloc_ok = cg_label(g, "tryallocok");
                char* alloc_fail = cg_label(g, "tryallocfail");
                fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", env_ok, alloc_ok, alloc_fail);

                fprintf(g->out, "%s:\n", alloc_fail);
                {
                    char* title_ptr = emit_string_ptr(g, "Too many nested try blocks");
                    char* msg_ptr = emit_string_ptr(g, "try nesting limit reached.");
                    if (!g->ok)
                        return;
                    fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
                    fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
                }

                fprintf(g->out, "%s:\n", alloc_ok);
            }

            char* push_res = cg_temp(g);
            fprintf(g->out, "%s = call i32 @runtime_try_push(ptr %s)\n", push_res, env);
            char* push_ok = cg_temp(g);
            fprintf(g->out, "%s = icmp eq i32 %s, 0\n", push_ok, push_res);
            char* push_ok_label = cg_label(g, "trypushok");
            char* push_fail_label = cg_label(g, "trypushfail");
            fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", push_ok, push_ok_label, push_fail_label);

            fprintf(g->out, "%s:\n", push_fail_label);
            {
                char* title_ptr = emit_string_ptr(g, "Too many nested try blocks");
                char* msg_ptr = emit_string_ptr(g, "try nesting limit reached.");
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
                fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
            }

            fprintf(g->out, "%s:\n", push_ok_label);
            char* frame = cg_temp(g);
            fprintf(g->out, "%s = call ptr @llvm.frameaddress(i32 0)\n", frame);
            char* res = cg_temp(g);
            fprintf(g->out, "%s = call i32 @_setjmp(ptr %s, ptr %s)\n", res, env, frame);
            char* is_ok = cg_temp(g);
            fprintf(g->out, "%s = icmp eq i32 %s, 0\n", is_ok, res);
            char* try_label = cg_label(g, "try");
            char* otherwise_label = cg_label(g, "tryelse");
            char* end_label = cg_label(g, "tryend");
            fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", is_ok, try_label, otherwise_label);

            fprintf(g->out, "%s:\n", try_label);
            if (g->try_depth >= 32) {
                cg_set_err(g, "Too many nested try blocks in LLVM backend.");
                return;
            }
            g->try_envs[g->try_depth++] = env;
            emit_stmt(g, node->try_stmt.try_block);
            if (!g->ok)
                return;
            g->try_depth--;
            fprintf(g->out, "call void @runtime_try_end(ptr %s)\n", env);
            fprintf(g->out, "br label %%%s\n", end_label);

            fprintf(g->out, "%s:\n", otherwise_label);
            fprintf(g->out, "call void @runtime_try_end(ptr %s)\n", env);
            if (node->try_stmt.otherwise_block) {
                emit_stmt(g, node->try_stmt.otherwise_block);
                if (!g->ok)
                    return;
            }
            fprintf(g->out, "br label %%%s\n", end_label);
            fprintf(g->out, "%s:\n", end_label);
            g->fn.terminated = 0;
            return;
        }
        case AST_SET_ELEMENT: {
            char* list_val = emit_load_var_value(g, node->set_element.name);
            if (!g->ok)
                return;
            if (expr_is_int(g, node->set_element.index)) {
                char* idx = emit_expr_int(g, node->set_element.index);
                if (!g->ok)
                    return;
                char* value = emit_expr(g, node->set_element.value);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_list_set_i32(%%Value* %s, i32 %s, %%Value* %s)\n", list_val, idx, value);
                return;
            }
            char* idx_val = emit_expr(g, node->set_element.index);
            if (!g->ok)
                return;
            char* value = emit_expr(g, node->set_element.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_list_set(%%Value* %s, %%Value* %s, %%Value* %s)\n", list_val, idx_val, value);
            return;
        }
        case AST_LIST_ADD: {
            char* list_val = emit_load_var_value(g, node->list_add.list_name);
            if (!g->ok)
                return;
            char* value = emit_expr(g, node->list_add.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_list_add(%%Value* %s, %%Value* %s)\n", list_val, value);
            return;
        }
        case AST_LIST_REMOVE: {
            char* list_val = emit_load_var_value(g, node->list_remove.list_name);
            if (!g->ok)
                return;
            char* value = emit_expr(g, node->list_remove.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_list_remove(%%Value* %s, %%Value* %s)\n", list_val, value);
            return;
        }
        case AST_LIST_REMOVE_ELEMENT: {
            char* list_val = emit_load_var_value(g, node->list_remove_element.list_name);
            if (!g->ok)
                return;
            if (expr_is_int(g, node->list_remove_element.index)) {
                char* idx = emit_expr_int(g, node->list_remove_element.index);
                if (!g->ok)
                    return;
                fprintf(g->out, "call void @rt_list_remove_element_i32(%%Value* %s, i32 %s)\n", list_val, idx);
                return;
            }
            char* idx_val = emit_expr(g, node->list_remove_element.index);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_list_remove_element(%%Value* %s, %%Value* %s)\n", list_val, idx_val);
            return;
        }
        case AST_LIST_CLEAR: {
            char* list_val = emit_load_var_value(g, node->list_clear.list_name);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_list_clear(%%Value* %s)\n", list_val);
            return;
        }
        case AST_DICT_ADD: {
            char* dict_val = emit_load_var_value(g, node->dict_add.dict_name);
            if (!g->ok)
                return;
            char* key = emit_expr(g, node->dict_add.key);
            if (!g->ok)
                return;
            char* value = emit_expr(g, node->dict_add.value);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_dict_set(%%Value* %s, %%Value* %s, %%Value* %s)\n", dict_val, key, value);
            return;
        }
        case AST_DICT_REMOVE: {
            char* dict_val = emit_load_var_value(g, node->dict_remove.dict_name);
            if (!g->ok)
                return;
            char* key = emit_expr(g, node->dict_remove.key);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_dict_remove(%%Value* %s, %%Value* %s)\n", dict_val, key);
            return;
        }
        case AST_DICT_CLEAR: {
            char* dict_val = emit_load_var_value(g, node->dict_clear.dict_name);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_dict_clear(%%Value* %s)\n", dict_val);
            return;
        }
        case AST_DICT_CONTAINS_ITEM: {
            char* dict_val = emit_load_var_value(g, node->dict_contains.dict_name);
            if (!g->ok)
                return;
            char* key = emit_expr(g, node->dict_contains.key);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_dict_contains_item(%%Value* %s, %%Value* %s)\n", dict_val, key);
            return;
        }
        case AST_TYPE_DECL:
            return;
        case AST_CREATE_LIBRARY:
            return;
        case AST_LIBRARY_OFFER:
            if (!g->current_loading_library) {
                cg_set_err(g, "`offer` can only be used in a library file.");
            }
            return;
        case AST_LOAD_LIBRARY: {
            LibraryModule* lib = ensure_library_loaded(g, node->library_load.name);
            if (!lib || !g->ok)
                return;
            emit_library_init_call(g, lib);
            return;
        }
        case AST_LOAD_FILE: {
            if (!node->file_load.path || node->file_load.path->type != EXPR_STRING_LITERAL) {
                cg_set_err_loc(g, node->line, node->column,
                    "LLVM backend requires a string literal in `load file`.");
                return;
            }
            FileModule* file = ensure_file_loaded(g, node->file_load.path->string_value);
            if (!file || !g->ok)
                return;
            emit_file_init_call(g, file);
            return;
        }
        case AST_LIBRARY_TAKE: {
            char resolved_name[512];
            if (!resolve_library_name(g, node->library_take.library_name, resolved_name, sizeof(resolved_name)))
                return;
            if (!is_library_loaded(g, resolved_name)) {
                cg_set_err(g, "Library `%s` is not loaded.", resolved_name);
                return;
            }
            int lib_idx = library_module_index(g, resolved_name);
            if (lib_idx < 0) {
                cg_set_err(g, "Library `%s` is not available in LLVM backend.", resolved_name);
                return;
            }
            LibraryModule* lib = &g->libraries[lib_idx];
            FunctionEntry* fn = find_function_in_library(g, lib->name, node->library_take.name);
            if (fn) {
                return;
            }
            if (node->library_take.alias_name && node->library_take.alias_name[0]) {
                int src_idx = sym_index(&g->symbols, node->library_take.name);
                int dst_idx = sym_index(&g->symbols, node->library_take.alias_name);
                if (src_idx < 0 || dst_idx < 0) {
                    cg_set_err(g, "Cannot import value `%s` from `%s`.", node->library_take.name, lib->name);
                    return;
                }
                char* src_ptr = emit_load_global(g, src_idx);
                if (!g->ok)
                    return;
                emit_store_global(g, dst_idx, src_ptr);
                if (g->int_known && dst_idx >= 0)
                    g->int_known[dst_idx] = 0;
                if (g->float_known && dst_idx >= 0)
                    g->float_known[dst_idx] = 0;
            }
            return;
        }
        case AST_LIBRARY_TAKE_ALL:
            return;
        case AST_FILE_WRITE: {
            char* path = emit_expr(g, node->file_io_stmt.path);
            if (!g->ok)
                return;
            char* content = emit_expr(g, node->file_io_stmt.content);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_file_write(%%Value* %s, %%Value* %s, i32 0)\n", path, content);
            return;
        }
        case AST_FILE_APPEND: {
            char* path = emit_expr(g, node->file_io_stmt.path);
            if (!g->ok)
                return;
            char* content = emit_expr(g, node->file_io_stmt.content);
            if (!g->ok)
                return;
            fprintf(g->out, "call void @rt_file_write(%%Value* %s, %%Value* %s, i32 1)\n", path, content);
            return;
        }
        case AST_EXIT: {
            if (g->loop_depth > 0) {
                const char* break_label = g->loop_break_labels[g->loop_depth - 1];
                fprintf(g->out, "br label %%%s\n", break_label);
                g->fn.terminated = 1;
            }
            return;
        }
        case AST_NEXT: {
            if (g->loop_depth > 0) {
                const char* cont_label = g->loop_continue_labels[g->loop_depth - 1];
                fprintf(g->out, "br label %%%s\n", cont_label);
                g->fn.terminated = 1;
            }
            return;
        }
        case AST_MATCH: {
            char* target = emit_expr(g, node->match_stmt.target);
            if (!g->ok)
                return;
            char* end_label = cg_label(g, "matchend");
            for (int i = 0; i < node->match_stmt.branch_count; i++) {
                char* case_label = cg_label(g, "matchcase");
                char* next_label = cg_label(g, "matchnext");
                char* candidate = emit_expr(g, node->match_stmt.branches[i].value);
                if (!g->ok)
                    return;
                char* eq_val = cg_alloca_value(g);
                fprintf(g->out, "call void @rt_eq(%%Value* %s, %%Value* %s, %%Value* %s)\n", eq_val, target, candidate);
                char* cond = cg_temp(g);
                fprintf(g->out, "%s = call i32 @rt_truthy(%%Value* %s)\n", cond, eq_val);
                fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cond, case_label, next_label);
                fprintf(g->out, "%s:\n", case_label);
                emit_stmt(g, node->match_stmt.branches[i].block);
                if (!g->ok)
                    return;
                if (!g->fn.terminated)
                    fprintf(g->out, "br label %%%s\n", end_label);
                fprintf(g->out, "%s:\n", next_label);
                g->fn.terminated = 0;
            }
            if (node->match_stmt.otherwise_block) {
                emit_stmt(g, node->match_stmt.otherwise_block);
                if (!g->ok)
                    return;
            }
            if (!g->fn.terminated)
                fprintf(g->out, "br label %%%s\n", end_label);
            fprintf(g->out, "%s:\n", end_label);
            g->fn.terminated = 0;
            return;
        }
        case AST_FOR_EACH: {
            char* iter_val = emit_expr(g, node->for_each_stmt.iterable);
            if (!g->ok)
                return;
            char* count = cg_temp(g);
            fprintf(g->out, "%s = call i32 @rt_collection_count(%%Value* %s)\n", count, iter_val);

            char* cond_label = cg_label(g, "foreachcond");
            char* body_label = cg_label(g, "foreachbody");
            char* end_label = cg_label(g, "foreachend");
            char* next_label = cond_label;

            char* i_ptr = cg_alloca_i32(g);
            fprintf(g->out, "store i32 0, i32* %s\n", i_ptr);

            g->loop_break_labels[g->loop_depth] = end_label;
            g->loop_continue_labels[g->loop_depth] = cond_label;
            g->loop_depth++;

            fprintf(g->out, "br label %%%s\n", cond_label);
            fprintf(g->out, "%s:\n", cond_label);
            char* cur = cg_temp(g);
            fprintf(g->out, "%s = load i32, i32* %s\n", cur, i_ptr);
            char* cmp = cg_temp(g);
            fprintf(g->out, "%s = icmp slt i32 %s, %s\n", cmp, cur, count);
            fprintf(g->out, "br i1 %s, label %%%s, label %%%s\n", cmp, body_label, end_label);

            fprintf(g->out, "%s:\n", body_label);
            emit_gc_tick(g);
            char* item = cg_alloca_value(g);
            fprintf(g->out, "call void @rt_collection_item(%%Value* %s, %%Value* %s, i32 %s)\n", item, iter_val, cur);
            int lidx = local_index(g, node->for_each_stmt.item_name);
            if (lidx >= 0) {
                emit_store_local_value(g, lidx, item);
                if (g->fn.int_known) g->fn.int_known[lidx] = 0;
                if (g->fn.float_known) g->fn.float_known[lidx] = 0;
            } else {
                int gidx = sym_index(&g->symbols, node->for_each_stmt.item_name);
                emit_store_global(g, gidx, item);
                if (g->int_known && gidx >= 0) g->int_known[gidx] = 0;
                if (g->float_known && gidx >= 0) g->float_known[gidx] = 0;
            }
            g->fn.terminated = 0;
            emit_stmt(g, node->for_each_stmt.body);
            if (!g->ok)
                return;
            if (!g->fn.terminated) {
                char* next = cg_temp(g);
                fprintf(g->out, "%s = add i32 %s, 1\n", next, cur);
                fprintf(g->out, "store i32 %s, i32* %s\n", next, i_ptr);
                fprintf(g->out, "br label %%%s\n", next_label);
            }
            fprintf(g->out, "%s:\n", end_label);
            g->loop_depth--;
            g->fn.terminated = 0;
            return;
        }
        default:
            cg_set_err(g, "Unsupported AST node in LLVM backend: %s", ast_type_name(node->type));
            return;
    }
}

static void emit_function(LLVMGen* g, const FunctionEntry* fn) {
    if (!g || !fn || !fn->node || !g->ok)
        return;

    FunctionCtx saved = g->fn;
    int saved_llvl_depth = g->llvl_depth;
    FILE* saved_out = g->out;
    FILE* body_tmp = NULL;
    FILE* alloca_tmp = NULL;
    g->entry_alloca_out = NULL;
    memset(&g->fn, 0, sizeof(g->fn));
    g->fn.in_function = 1;
    g->fn.is_generator = fn->is_generator;
    g->fn.owner_library = fn->owner_library;
    g->llvl_depth = g->program_llvl_mode ? 1 : 0;

    SymbolTable locals = {0};
    const ASTNode* decl = fn->node;
    for (int i = 0; i < decl->function_decl.param_count; i++) {
        if (decl->function_decl.params[i])
            sym_add(&locals, decl->function_decl.params[i]);
    }
    collect_locals_ast(&locals, decl->function_decl.body);

    g->fn.locals = locals;
    g->fn.local_count = locals.count;
    if (locals.count > 0) {
        if ((size_t)locals.count > SIZE_MAX / sizeof(char*)) {
            cg_set_err(g, "Out of memory while preparing function locals.");
            goto cleanup;
        }
        g->fn.int_known = (unsigned char*)calloc((size_t)locals.count, 1);
        g->fn.float_known = (unsigned char*)calloc((size_t)locals.count, 1);
        g->fn.local_values = (char**)calloc((size_t)locals.count, sizeof(char*));
        g->fn.local_i32 = (char**)calloc((size_t)locals.count, sizeof(char*));
        g->fn.local_f64 = (char**)calloc((size_t)locals.count, sizeof(char*));
        if (!g->fn.int_known || !g->fn.float_known || !g->fn.local_values ||
            !g->fn.local_i32 || !g->fn.local_f64) {
            cg_set_err(g, "Out of memory while preparing function locals.");
            goto cleanup;
        }
    }

    g->fn.ret_ptr = fn->is_generator ? "%out_list" : "%out";
    g->fn.ret_label = cg_label(g, "fnret");
    g->fn.gen_out_list = fn->is_generator ? "%out_list" : NULL;
    g->fn.terminated = 0;
    g->fn.has_return = 0;

    if (fn->is_generator) {
        fprintf(saved_out, "define void %s(%%Value* %s, %%Value* %%args, i32 %%arg_count) {\n",
            fn->gen_ir_name, g->fn.ret_ptr);
    } else {
        fprintf(saved_out, "define void %s(%%Value* %s", fn->ir_name, g->fn.ret_ptr);
        for (int i = 0; i < decl->function_decl.param_count; i++) {
            fprintf(saved_out, ", %%Value* %%arg%d", i);
        }
        fprintf(saved_out, ") {\n");
    }
    fprintf(saved_out, "entry:\n");

    for (int i = 0; i < locals.count; i++) {
        char* vptr = cg_temp(g);
        fprintf(saved_out, "%s = alloca %%Value\n", vptr);
        fprintf(saved_out, "store %%Value zeroinitializer, %%Value* %s\n", vptr);
        g->fn.local_values[i] = vptr;
        char* iptr = cg_temp(g);
        fprintf(saved_out, "%s = alloca i32\n", iptr);
        fprintf(saved_out, "store i32 0, i32* %s\n", iptr);
        g->fn.local_i32[i] = iptr;
        char* fptr = cg_temp(g);
        fprintf(saved_out, "%s = alloca double\n", fptr);
        fprintf(saved_out, "store double 0.0, double* %s\n", fptr);
        g->fn.local_f64[i] = fptr;
    }

    body_tmp = tmpfile();
    alloca_tmp = tmpfile();
    if (!body_tmp || !alloca_tmp) {
        if (body_tmp)
            fclose(body_tmp);
        if (alloca_tmp)
            fclose(alloca_tmp);
        body_tmp = NULL;
        alloca_tmp = NULL;
    } else {
        g->out = body_tmp;
        g->entry_alloca_out = alloca_tmp;
    }

    g->fn.root_count = 0;
    fprintf(g->out, "call void @rt_gc_push_root(%%Value* %s)\n", g->fn.ret_ptr);
    g->fn.root_count++;
    for (int i = 0; i < locals.count; i++) {
        fprintf(g->out, "call void @rt_gc_push_root(%%Value* %s)\n", g->fn.local_values[i]);
        g->fn.root_count++;
    }

    for (int i = 0; i < decl->function_decl.param_count; i++) {
        const char* param = decl->function_decl.params[i];
        int idx = sym_index(&g->fn.locals, param);
        if (idx >= 0) {
            char* loaded = cg_temp(g);
            if (fn->is_generator) {
                char* arg_ptr = cg_temp(g);
                fprintf(g->out, "%s = getelementptr inbounds %%Value, %%Value* %%args, i32 %d\n", arg_ptr, i);
                fprintf(g->out, "%s = load %%Value, %%Value* %s\n", loaded, arg_ptr);
            } else {
                fprintf(g->out, "%s = load %%Value, %%Value* %%arg%d\n", loaded, i);
            }
            fprintf(g->out, "store %%Value %s, %%Value* %s\n", loaded, g->fn.local_values[idx]);
        }
    }

    emit_stmt(g, decl->function_decl.body);
    if (!g->ok)
        goto cleanup;

    if (!fn->is_generator) {
        if (!g->fn.terminated) {
            char* title_ptr = emit_string_ptr(g, "Missing function result");
            char* msg_ptr = emit_string_ptr(g, "Use `return value` or end the function with an expression.");
            if (!g->ok)
                goto cleanup;
            fprintf(g->out, "call void @runtime_error(i8* %s, i8* %s, i8* %s)\n", title_ptr, msg_ptr, msg_ptr);
            fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
        }
        fprintf(g->out, "%s:\n", g->fn.ret_label);
        if (g->fn.root_count > 0)
            fprintf(g->out, "call void @rt_gc_pop_roots(i32 %d)\n", g->fn.root_count);
        fprintf(g->out, "ret void\n");
    } else {
        if (!g->fn.terminated) {
            fprintf(g->out, "br label %%%s\n", g->fn.ret_label);
        }
        fprintf(g->out, "%s:\n", g->fn.ret_label);
        if (g->fn.root_count > 0)
            fprintf(g->out, "call void @rt_gc_pop_roots(i32 %d)\n", g->fn.root_count);
        fprintf(g->out, "ret void\n");
    }
    if (body_tmp && alloca_tmp) {
        cg_flush_temp(alloca_tmp, saved_out);
        cg_flush_temp(body_tmp, saved_out);
        fprintf(saved_out, "}\n");
    } else {
        fprintf(saved_out, "}\n");
    }

cleanup:
    if (body_tmp && alloca_tmp) {
        g->out = saved_out;
        g->entry_alloca_out = NULL;
        fclose(body_tmp);
        fclose(alloca_tmp);
    } else {
        g->out = saved_out;
    }
    free(locals.names);
    free(g->fn.int_known);
    free(g->fn.float_known);
    free(g->fn.local_values);
    free(g->fn.local_i32);
    free(g->fn.local_f64);
    g->fn = saved;
    g->llvl_depth = saved_llvl_depth;
}

int llvm_codegen_write_ir(
    const ASTNode* program,
    const char* source_label,
    const char* output_path,
    char* err,
    size_t err_size
) {
    if (!program || !output_path || !output_path[0]) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Missing program or output path for LLVM codegen.");
        return 0;
    }

    LLVMGen g;
    memset(&g, 0, sizeof(g));
    g.ok = 1;
    g.err = err;
    g.err_size = err_size;
    g.source_label = source_label;
    g.program_llvl_mode = program->llvl_mode ? 1 : 0;
    FILE* out = NULL;
    int result = 0;

    if (source_label && source_label[0])
        source_set_label(source_label);

    check_library_stmt_positions(&g, program, 0);
    if (!g.ok)
        goto cleanup;

    scan_loads_ast(&g, program);
    if (!g.ok)
        goto cleanup;

    collect_symbols_ast(&g.symbols, program);
    for (int i = 0; i < g.library_count; i++)
        collect_symbols_ast(&g.symbols, g.libraries[i].program);
    for (int i = 0; i < g.file_count; i++)
        collect_symbols_ast(&g.symbols, g.files[i].program);

    collect_strings_ast(&g.strings, program);
    for (int i = 0; i < g.library_count; i++)
        collect_strings_ast(&g.strings, g.libraries[i].program);
    for (int i = 0; i < g.file_count; i++)
        collect_strings_ast(&g.strings, g.files[i].program);
    str_add(&g.strings, "Missing function result");
    str_add(&g.strings, "Use `return value` or end the function with an expression.");
    str_add(&g.strings, "Invalid generator flow");
    str_add(&g.strings, "Generator function cannot use `return`.");
    str_add(&g.strings, "Invalid yield");
    str_add(&g.strings, "`yield` can only be used inside a generator function.");
    str_add(&g.strings, "Invalid while repeat limit");
    str_add(&g.strings, "Repeat limit cannot be negative.");
    str_add(&g.strings, "Invalid repeat count");
    str_add(&g.strings, "Repeat count cannot be negative.");
    str_add(&g.strings, "Too many nested try blocks");
    str_add(&g.strings, "try nesting limit reached.");
    str_add(&g.strings, "Unsupported feature");
    str_add(&g.strings, "Library operations are not supported in LLVM backend yet.");
    str_add(&g.strings, "Invalid expression");
    str_add(&g.strings, "List add is a statement, not an expression.");
    if (source_label && source_label[0])
        str_add(&g.strings, source_label);
    collect_functions_ast(&g, program, NULL);
    for (int i = 0; i < g.library_count; i++)
        collect_functions_ast(&g, g.libraries[i].program, g.libraries[i].name);
    for (int i = 0; i < g.file_count; i++)
        collect_functions_ast(&g, g.files[i].program, NULL);
    if (!g.ok)
        goto cleanup;

    resolve_imports_ast(&g, program);
    for (int i = 0; i < g.library_count && g.ok; i++) {
        const char* saved_loading = g.current_loading_library;
        g.current_loading_library = g.libraries[i].name;
        resolve_imports_ast(&g, g.libraries[i].program);
        g.current_loading_library = saved_loading;
    }
    for (int i = 0; i < g.file_count && g.ok; i++)
        resolve_imports_ast(&g, g.files[i].program);
    if (!g.ok)
        goto cleanup;
    for (int i = 0; i < g.function_count; i++) {
        g.functions[i].is_generator = ast_contains_yield(g.functions[i].node->function_decl.body);
        g.functions[i].ir_name = mangle_function_name(
            g.functions[i].name,
            g.functions[i].owner_library,
            i
        );
        if (!g.functions[i].ir_name) {
            if (err && err_size > 0)
                snprintf(err, err_size, "Out of memory while preparing function names.");
            g.ok = 0;
            goto cleanup;
        }
        if (g.functions[i].is_generator) {
            size_t len = strlen(g.functions[i].ir_name);
            if (len > SIZE_MAX - 5) {
                if (err && err_size > 0)
                    snprintf(err, err_size, "Out of memory while preparing generator names.");
                g.ok = 0;
                goto cleanup;
            }
            len += 5;
            g.functions[i].gen_ir_name = (char*)malloc(len);
            if (!g.functions[i].gen_ir_name) {
                if (err && err_size > 0)
                    snprintf(err, err_size, "Out of memory while preparing generator names.");
                g.ok = 0;
                goto cleanup;
            }
            snprintf(g.functions[i].gen_ir_name, len, "%s_gen", g.functions[i].ir_name);
        }
    }
    if (g.symbols.count > 0) {
        if (g.symbols.count < 0) {
            if (err && err_size > 0)
                snprintf(err, err_size, "Out of memory while preparing globals.");
            g.ok = 0;
            goto cleanup;
        }
        g.int_known = (unsigned char*)calloc((size_t)g.symbols.count, 1);
        g.float_known = (unsigned char*)calloc((size_t)g.symbols.count, 1);
    }

    out = fopen(output_path, "wb");
    if (!out) {
        if (err && err_size > 0)
            snprintf(err, err_size, "Could not open IR output path.");
        g.ok = 0;
        goto cleanup;
    }
    g.out = out;

    fprintf(out, "; Sicht native LLVM IR (subset backend)\n");
#ifdef _WIN32
    fprintf(out, "target triple = \"x86_64-w64-windows-gnu\"\n");
#endif
    fprintf(out, "%%Value = type { i32, i32, double, i8*, i8*, i8*, i8*, i8*, i8* }\n\n");

    emit_string_globals(&g);

    for (int i = 0; i < g.library_count; i++) {
        fprintf(out, "@.lib_loaded%d = global i32 0\n", g.libraries[i].init_id);
    }
    if (g.library_count > 0)
        fprintf(out, "\n");

    if (g.symbols.count > 0) {
        fprintf(out, "@globals = global [%d x %%Value] zeroinitializer\n\n", g.symbols.count);
        fprintf(out, "@globals_i32 = global [%d x i32] zeroinitializer\n\n", g.symbols.count);
        fprintf(out, "@globals_f64 = global [%d x double] zeroinitializer\n\n", g.symbols.count);
    }

    fprintf(out, "declare void @rt_make_int(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_make_float(%%Value*, double)\n");
    fprintf(out, "declare void @rt_make_bool(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_make_string(%%Value*, i8*)\n");
    fprintf(out, "declare void @rt_add(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_sub(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_mul(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_div(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_eq(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_ne(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_gt(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_lt(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_gte(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_lte(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_and(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_or(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_not(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_neg(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_cast_int(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_cast_float(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_cast_bool(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_cast_string(%%Value*, %%Value*)\n");
    fprintf(out, "declare i32 @rt_get_int(%%Value*)\n");
    fprintf(out, "declare i32 @rt_truthy(%%Value*)\n");
    fprintf(out, "declare void @rt_print_inline(%%Value*)\n");
    fprintf(out, "declare void @rt_print_int(i32)\n");
    fprintf(out, "declare void @rt_print_float(double)\n");
    fprintf(out, "declare void @rt_print_text(i8*)\n");
    fprintf(out, "declare void @rt_print_text_repeat(i8*, i32)\n");
    fprintf(out, "declare void @rt_print_text_repeat_checked(i8*, i32)\n");
    fprintf(out, "declare void @rt_print_newline()\n");
    fprintf(out, "declare void @rt_list_new(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_list_set(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_list_set_i32(%%Value*, i32, %%Value*)\n");
    fprintf(out, "declare void @rt_list_get(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_list_get_i32(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_list_add(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_list_remove(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_list_remove_element(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_list_remove_element_i32(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_list_clear(%%Value*)\n");
    fprintf(out, "declare void @rt_list_index_of(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_gen_yield(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_dict_new(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_dict_set(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_dict_get(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_dict_remove(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_dict_clear(%%Value*)\n");
    fprintf(out, "declare void @rt_dict_contains_item(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_contains(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_input(%%Value*, i8*)\n");
    fprintf(out, "declare void @rt_builtin(%%Value*, i32, %%Value*)\n");
    fprintf(out, "declare void @rt_char_at(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_file_read(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_file_write(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_llvl_require()\n");
    fprintf(out, "declare void @rt_llvl_set_gc_paused(i32)\n");
    fprintf(out, "declare void @rt_llvl_set_bounds_check(i32)\n");
    fprintf(out, "declare void @rt_llvl_set_pointer_checks(i32)\n");
    fprintf(out, "declare void @rt_llvl_alloc(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_free(%%Value*)\n");
    fprintf(out, "declare void @rt_llvl_resize(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_llvl_resize_any(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_copy(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_copy_bytes(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_move(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_get_value(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_set_value(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_get_byte(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_set_byte(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_get_bit(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_bit_op(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_llvl_place_of(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_offset(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_get_at(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_set_at(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_get_at_typed(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_llvl_set_at_typed(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_llvl_set_buffer_meta(%%Value*, i64, i32, i8*)\n");
    fprintf(out, "declare void @rt_llvl_register_type(i8*, i32, i32, i64)\n");
    fprintf(out, "declare void @rt_llvl_register_field(i8*, i32, i8*, i32, i32, i64, i64)\n");
    fprintf(out, "declare void @rt_llvl_field_get(%%Value*, %%Value*, i8*)\n");
    fprintf(out, "declare void @rt_llvl_field_set(%%Value*, i8*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_wait_ms(%%Value*)\n");
    fprintf(out, "declare void @rt_llvl_pin_write(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_pin_read(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_port_write(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_port_read(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_llvl_register_interrupt(%%Value*, i8*)\n");
    fprintf(out, "declare void @rt_cli_set_args_from_argv(i32, i8**)\n");
    fprintf(out, "declare void @rt_native_cli_args(%%Value*)\n");
    fprintf(out, "declare void @rt_native_io_directory_exists(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_create_directory(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_list_directories(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_list_files(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_remove_directory(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_delete_file(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_copy_file(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_io_copy_directory(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_env_get(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_zip_extract(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_http_get(%%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_http_post(%%Value*, %%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_http_request(%%Value*, %%Value*, %%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_http_download(%%Value*, %%Value*, %%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_time_ms(%%Value*)\n");
    fprintf(out, "declare void @rt_native_process_run(%%Value*, %%Value*)\n");
    fprintf(out, "declare void @rt_native_process_id(%%Value*)\n");
    fprintf(out, "declare void @rt_make_generator(%%Value*, i8*, %%Value*, i32)\n");
    fprintf(out, "declare i32 @rt_generator_next(%%Value*, %%Value*)\n");
    fprintf(out, "declare i32 @rt_collection_count(%%Value*)\n");
    fprintf(out, "declare void @rt_collection_item(%%Value*, %%Value*, i32)\n");
    fprintf(out, "declare void @rt_gc_register_globals(%%Value*, i32)\n");
    fprintf(out, "declare void @rt_gc_push_root(%%Value*)\n");
    fprintf(out, "declare void @rt_gc_pop_roots(i32)\n");
    fprintf(out, "declare void @rt_gc_maybe_collect()\n");
    fprintf(out, "declare void @rt_step_tick()\n");
    fprintf(out, "declare void @gc_init()\n");
    fprintf(out, "declare void @gc_shutdown()\n");
    fprintf(out, "declare void @source_set_label(i8*)\n\n");
    fprintf(out, "declare void @runtime_error(i8*, i8*, i8*)\n\n");
    fprintf(out, "declare ptr @runtime_try_alloc()\n");
    fprintf(out, "declare i32 @runtime_try_push(ptr)\n");
    fprintf(out, "declare ptr @llvm.frameaddress(i32)\n");
    fprintf(out, "declare i32 @_setjmp(ptr, ptr) returns_twice\n");
    fprintf(out, "declare void @runtime_try_end(ptr)\n\n");

    for (int i = 0; i < g.function_count; i++) {
        emit_function(&g, &g.functions[i]);
        if (!g.ok) {
            goto cleanup;
        }
    }

    for (int i = 0; i < g.library_count; i++) {
        emit_module_init_function(&g, g.libraries[i].name, g.libraries[i].program, 1, g.libraries[i].init_id);
        if (!g.ok) {
            goto cleanup;
        }
    }
    for (int i = 0; i < g.file_count; i++) {
        emit_module_init_function(&g, NULL, g.files[i].program, 0, g.files[i].init_id);
        if (!g.ok) {
            goto cleanup;
        }
    }

    fprintf(out, "define i32 @main(i32 %%argc, i8** %%argv) {\n");
    fprintf(out, "entry:\n");

    FILE* body_tmp = tmpfile();
    FILE* alloca_tmp = tmpfile();
    if (!body_tmp || !alloca_tmp) {
        if (body_tmp)
            fclose(body_tmp);
        if (alloca_tmp)
            fclose(alloca_tmp);
        body_tmp = NULL;
        alloca_tmp = NULL;
    }

    if (body_tmp && alloca_tmp) {
        FILE* saved_out = g.out;
        g.out = body_tmp;
        g.entry_alloca_out = alloca_tmp;

        fprintf(g.out, "  call void @rt_cli_set_args_from_argv(i32 %%argc, i8** %%argv)\n");
        fprintf(g.out, "  call void @gc_init()\n");
        if (g.symbols.count > 0) {
            char* gptr = cg_temp(&g);
            fprintf(g.out, "  %s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* @globals, i32 0, i32 0\n",
                gptr, g.symbols.count, g.symbols.count);
            fprintf(g.out, "  call void @rt_gc_register_globals(%%Value* %s, i32 %d)\n", gptr, g.symbols.count);
            free(gptr);
        }
        if (source_label && source_label[0]) {
            char* label_ptr = emit_string_ptr(&g, source_label);
            fprintf(g.out, "  call void @source_set_label(i8* %s)\n", label_ptr);
        }

        memset(&g.fn, 0, sizeof(g.fn));
        g.fn.ret_label = cg_label(&g, "mainret");

        emit_stmt(&g, program);
        if (!g.ok) {
            g.out = saved_out;
            g.entry_alloca_out = NULL;
            fclose(body_tmp);
            fclose(alloca_tmp);
            goto cleanup;
        }

        if (!g.fn.terminated)
            fprintf(g.out, "  br label %%%s\n", g.fn.ret_label);
        fprintf(g.out, "%s:\n", g.fn.ret_label);
        fprintf(g.out, "  call void @gc_shutdown()\n");
        fprintf(g.out, "  ret i32 0\n");

        cg_flush_temp(alloca_tmp, out);
        cg_flush_temp(body_tmp, out);
        fprintf(out, "}\n");

        g.out = saved_out;
        g.entry_alloca_out = NULL;
        fclose(body_tmp);
        fclose(alloca_tmp);
    } else {
        g.entry_alloca_out = NULL;
        fprintf(out, "  call void @rt_cli_set_args_from_argv(i32 %%argc, i8** %%argv)\n");
        fprintf(out, "  call void @gc_init()\n");
        if (g.symbols.count > 0) {
            char* gptr = cg_temp(&g);
            fprintf(out, "  %s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* @globals, i32 0, i32 0\n",
                gptr, g.symbols.count, g.symbols.count);
            fprintf(out, "  call void @rt_gc_register_globals(%%Value* %s, i32 %d)\n", gptr, g.symbols.count);
            free(gptr);
        }
        if (source_label && source_label[0]) {
            char* label_ptr = emit_string_ptr(&g, source_label);
            fprintf(out, "  call void @source_set_label(i8* %s)\n", label_ptr);
        }

        memset(&g.fn, 0, sizeof(g.fn));
        g.fn.ret_label = cg_label(&g, "mainret");

        emit_stmt(&g, program);
        if (!g.ok) {
            goto cleanup;
        }

        if (!g.fn.terminated)
            fprintf(out, "  br label %%%s\n", g.fn.ret_label);
        fprintf(out, "%s:\n", g.fn.ret_label);
        fprintf(out, "  call void @gc_shutdown()\n");
        fprintf(out, "  ret i32 0\n");
        fprintf(out, "}\n");
    }

    result = g.ok ? 1 : 0;

cleanup:
    if (out) {
        fclose(out);
        out = NULL;
    }
    str_table_free(&g.strings);
    free(g.int_known);
    free(g.float_known);
    if (g.functions) {
        for (int i = 0; i < g.function_count; i++)
            free(g.functions[i].ir_name);
        for (int i = 0; i < g.function_count; i++)
            free(g.functions[i].gen_ir_name);
        free(g.functions);
    }
    if (g.libraries) {
        for (int i = 0; i < g.library_count; i++) {
            free(g.libraries[i].name);
            free(g.libraries[i].path);
        }
        free(g.libraries);
    }
    if (g.files) {
        for (int i = 0; i < g.file_count; i++)
            free(g.files[i].path);
        free(g.files);
    }
    for (int i = 0; i < g.loaded_library_count; i++)
        free(g.loaded_libraries[i]);
    for (int i = 0; i < g.loading_library_count; i++)
        free(g.loading_libraries[i]);
    for (int i = 0; i < g.loading_file_count; i++)
        free(g.loading_files[i]);
    for (int i = 0; i < g.library_offer_count; i++) {
        free(g.library_offers[i].library_name);
        free(g.library_offers[i].symbol_name);
    }
    if (g.llvl_types) {
        for (int i = 0; i < g.llvl_type_count; i++)
            free(g.llvl_types[i].fields);
        free(g.llvl_types);
    }
    free(g.types);
    return result;
}


