#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "llvm_backend.h"
#include "llvm_codegen.h"

int llvm_backend_is_ready(void) {
    return 1;
}

static void llvm_set_err(char* err, size_t err_size, const char* msg) {
    if (err && err_size > 0) {
        snprintf(err, err_size, "%s", msg ? msg : "unknown error");
    }
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int ensure_dir(const char* path) {
    if (!path || path[0] == '\0')
        return 0;
#ifdef _WIN32
    if (_mkdir(path) == 0)
        return 1;
#else
    if (mkdir(path, 0755) == 0)
        return 1;
#endif
    return errno == EEXIST;
}

static int file_is_newer(const char* src_path, const char* ref_path) {
    if (!src_path || !ref_path)
        return 1;
    struct stat src_stat;
    struct stat ref_stat;
    if (stat(src_path, &src_stat) != 0)
        return 1;
    if (stat(ref_path, &ref_stat) != 0)
        return 1;
    return src_stat.st_mtime > ref_stat.st_mtime;
}

static int appendf(char* out, size_t out_size, size_t* off, const char* fmt, ...);

static int copy_text(char* out, size_t out_size, const char* text) {
    if (!out || out_size == 0 || !text)
        return 0;
    size_t len = strlen(text);
    if (len + 1 > out_size)
        return 0;
    memcpy(out, text, len + 1);
    return 1;
}

static int detect_candidate_under_root(
    const char* runtime_root,
    const char* const* relative_candidates,
    size_t relative_count,
    char* out,
    size_t out_size
) {
    if (!runtime_root || runtime_root[0] == '\0' || !relative_candidates || relative_count == 0)
        return 0;

    char probe[2048];
    for (size_t i = 0; i < relative_count; i++) {
        int n = snprintf(probe, sizeof(probe), "%s/%s", runtime_root, relative_candidates[i]);
        if (n < 0 || (size_t)n >= sizeof(probe))
            continue;
        if (file_exists(probe))
            return copy_text(out, out_size, probe);
    }
    return 0;
}

static int is_sysroot_dir(const char* root) {
    if (!root || root[0] == '\0')
        return 0;

    const char* header_candidates[] = {
        "usr/include/stdio.h",
        "include/stdio.h",
        "x86_64-w64-mingw32/include/stdio.h"
    };

    char probe[2048];
    for (size_t i = 0; i < sizeof(header_candidates) / sizeof(header_candidates[0]); i++) {
        int n = snprintf(probe, sizeof(probe), "%s/%s", root, header_candidates[i]);
        if (n < 0 || (size_t)n >= sizeof(probe))
            continue;
        if (file_exists(probe))
            return 1;
    }
    return 0;
}

static int resolve_clang_command(const char* runtime_root, char* out, size_t out_size) {
    const char* env_clang = getenv("SICHT_LLVM_CLANG");
    if (env_clang && env_clang[0] != '\0')
        return copy_text(out, out_size, env_clang);

    const char* candidates[] = {
#ifdef _WIN32
        "llvm/bin/clang.exe",
        "llvm/LLVM/bin/clang.exe",
        "installer/llvm/bin/clang.exe",
        "installer/llvm/LLVM/bin/clang.exe"
#else
        "llvm/bin/clang",
        "llvm/LLVM/bin/clang",
        "installer/llvm/bin/clang",
        "installer/llvm/LLVM/bin/clang"
#endif
    };
    if (detect_candidate_under_root(
            runtime_root,
            candidates,
            sizeof(candidates) / sizeof(candidates[0]),
            out,
            out_size)) {
        return 1;
    }

    return copy_text(out, out_size, "clang");
}

static int llvm_keep_ir(void) {
    const char* env = getenv("SICHT_KEEP_IR");
    return (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
}

static void maybe_remove_ir(const char* path) {
    if (!path || !path[0])
        return;
    if (llvm_keep_ir())
        return;
    remove(path);
}

static int resolve_sysroot_path(const char* runtime_root, char* out, size_t out_size) {
    const char* env_sysroot = getenv("SICHT_LLVM_SYSROOT");
    if (env_sysroot && env_sysroot[0] != '\0')
        return copy_text(out, out_size, env_sysroot);

    if (!runtime_root || runtime_root[0] == '\0')
        return 0;

    const char* candidates[] = {
        "llvm/mingw",
        "llvm/llvm-mingw",
        "llvm/sysroot",
        "llvm/x86_64-w64-mingw32",
        "llvm/LLVM/mingw",
        "llvm/LLVM/llvm-mingw",
        "llvm/LLVM/sysroot",
        "llvm/LLVM/x86_64-w64-mingw32",
        "installer/llvm/mingw",
        "installer/llvm/llvm-mingw",
        "installer/llvm/sysroot",
        "installer/llvm/x86_64-w64-mingw32",
        "installer/llvm/LLVM/mingw",
        "installer/llvm/LLVM/llvm-mingw",
        "installer/llvm/LLVM/sysroot",
        "installer/llvm/LLVM/x86_64-w64-mingw32"
    };

    char probe[2048];
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int n = snprintf(probe, sizeof(probe), "%s/%s", runtime_root, candidates[i]);
        if (n < 0 || (size_t)n >= sizeof(probe))
            continue;
        if (is_sysroot_dir(probe))
            return copy_text(out, out_size, probe);
    }

    return 0;
}

static int append_sysroot_flags(
    char* out,
    size_t out_size,
    size_t* off,
    const char* runtime_root
) {
    char sysroot[2048];
    if (!resolve_sysroot_path(runtime_root, sysroot, sizeof(sysroot)))
        return 1;

#ifdef _WIN32
    if (!appendf(out, out_size, off, " --target=x86_64-w64-windows-gnu"))
        return 0;
#endif
    if (!appendf(out, out_size, off, " --sysroot=\"%s\" -fuse-ld=lld", sysroot))
        return 0;

    return 1;
}

static int appendf(char* out, size_t out_size, size_t* off, const char* fmt, ...) {
    if (!out || !off || !fmt || *off >= out_size)
        return 0;

    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(out + *off, out_size - *off, fmt, args);
    va_end(args);

    if (wrote < 0)
        return 0;
    if ((size_t)wrote >= out_size - *off)
        return 0;

    *off += (size_t)wrote;
    return 1;
}

static int llvm_debug_enabled(void) {
    const char* raw = getenv("SICHT_LLVM_DEBUG");
    return raw && raw[0] && strcmp(raw, "0") != 0;
}

static int append_clang_invocation(char* out, size_t out_size, size_t* off, const char* clang_cmd) {
    if (!clang_cmd || clang_cmd[0] == '\0')
        return 0;
#ifdef _WIN32
    if (strchr(clang_cmd, ' ') || strchr(clang_cmd, '\t'))
        return appendf(out, out_size, off, "call \"%s\"", clang_cmd);
    return appendf(out, out_size, off, "%s", clang_cmd);
#else
    return appendf(out, out_size, off, "\"%s\"", clang_cmd);
#endif
}

static int append_default_compile_flags(char* out, size_t out_size, size_t* off) {
#ifdef _WIN32
    if (!appendf(out, out_size, off, " -D_CRT_SECURE_NO_WARNINGS"))
        return 0;
#endif
    return 1;
}

static int append_user_extra_cflags(char* out, size_t out_size, size_t* off) {
    const char* extra = getenv("SICHT_LLVM_EXTRA_CFLAGS");
    if (!extra || extra[0] == '\0')
        return 1;
    return appendf(out, out_size, off, " %s", extra);
}

static int prepare_runtime_objects(
    const char* runtime_root,
    const char* clang_cmd,
    const char* const* runtime_paths,
    size_t source_count,
    char obj_paths[][1024],
    size_t obj_capacity,
    char* err,
    size_t err_size
) {
    if (!runtime_root || runtime_root[0] == '\0') {
        llvm_set_err(err, err_size, "Missing runtime root for LLVM native build.");
        return 0;
    }
    if (!clang_cmd || clang_cmd[0] == '\0') {
        llvm_set_err(err, err_size, "Missing clang command for LLVM native build.");
        return 0;
    }
    if (!runtime_paths || source_count == 0 || source_count > obj_capacity) {
        llvm_set_err(err, err_size, "Invalid runtime source list for LLVM native build.");
        return 0;
    }

    char build_dir[1024];
    char cache_dir[1024];
    int n = snprintf(build_dir, sizeof(build_dir), "%s/build", runtime_root);
    if (n < 0 || (size_t)n >= sizeof(build_dir)) {
        llvm_set_err(err, err_size, "Runtime build directory path is too long.");
        return 0;
    }
    n = snprintf(cache_dir, sizeof(cache_dir), "%s/build/llvm_cache", runtime_root);
    if (n < 0 || (size_t)n >= sizeof(cache_dir)) {
        llvm_set_err(err, err_size, "Runtime cache directory path is too long.");
        return 0;
    }

    if (!ensure_dir(build_dir) || !ensure_dir(cache_dir)) {
        llvm_set_err(err, err_size, "Failed to create runtime build cache directory.");
        return 0;
    }

    int needs_rebuild = 0;
    for (size_t i = 0; i < source_count; i++) {
        n = snprintf(obj_paths[i], 1024, "%s/runtime_%02zu.obj", cache_dir, i);
        if (n < 0 || n >= 1024) {
            llvm_set_err(err, err_size, "Runtime object path is too long.");
            return 0;
        }
        if (!file_exists(obj_paths[i]) ||
            file_is_newer(runtime_paths[i], obj_paths[i])) {
            needs_rebuild = 1;
        }
    }

    if (!needs_rebuild)
        return 1;

    for (size_t i = 0; i < source_count; i++) {
        char command[8192];
        size_t off = 0;
        int ok = append_clang_invocation(command, sizeof(command), &off, clang_cmd);
        if (!ok) {
            llvm_set_err(err, err_size, "Invalid clang command for runtime build.");
            return 0;
        }
        ok = appendf(
            command,
            sizeof(command),
            &off,
            " -std=c11 -O0 -I\"%s/src/include\" -I\"%s/src/debug\" -I\"%s/headers\" -I\"%s/debug\" -c \"%s\" -o \"%s\"",
            runtime_root,
            runtime_root,
            runtime_root,
            runtime_root,
            runtime_paths[i],
            obj_paths[i]
        );
        if (!ok) {
            llvm_set_err(err, err_size, "Runtime compile command is too long.");
            return 0;
        }
        ok = append_default_compile_flags(command, sizeof(command), &off);
        if (!ok) {
            llvm_set_err(err, err_size, "Runtime compile command is too long.");
            return 0;
        }
        ok = append_user_extra_cflags(command, sizeof(command), &off);
        if (!ok) {
            llvm_set_err(err, err_size, "Runtime compile command is too long.");
            return 0;
        }
        ok = append_sysroot_flags(command, sizeof(command), &off, runtime_root);
        if (!ok) {
            llvm_set_err(err, err_size, "Runtime compile command is too long.");
            return 0;
        }
        if (llvm_debug_enabled()) {
            fprintf(stderr, "[llvm-debug] runtime object build:\n%s\n", command);
        }
        int rc = system(command);
        if (rc != 0) {
            llvm_set_err(err, err_size, "Failed to build cached runtime objects for LLVM native build.");
            return 0;
        }
    }

    return 1;
}

int llvm_backend_build_exe_from_ir(
    const char* ir_path,
    const char* exe_output_path,
    char* err,
    size_t err_size
) {
    if (!ir_path || ir_path[0] == '\0') {
        llvm_set_err(err, err_size, "Missing LLVM IR path.");
        return 0;
    }
    if (!exe_output_path || exe_output_path[0] == '\0') {
        llvm_set_err(err, err_size, "Missing executable output path.");
        return 0;
    }

    char clang_cmd[2048];
    if (!resolve_clang_command(NULL, clang_cmd, sizeof(clang_cmd))) {
        llvm_set_err(err, err_size, "Invalid clang command.");
        return 0;
    }

    char command[4096];
    size_t off = 0;
    int ok = append_clang_invocation(command, sizeof(command), &off, clang_cmd);
    if (!ok)
        return 0;
    ok = appendf(command, sizeof(command), &off, " \"%s\" -O2", ir_path);
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }
    ok = append_default_compile_flags(command, sizeof(command), &off);
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }
    ok = append_user_extra_cflags(command, sizeof(command), &off);
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }
    ok = appendf(command, sizeof(command), &off, " -Wno-override-module");
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }
    ok = append_sysroot_flags(command, sizeof(command), &off, NULL);
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }
    ok = appendf(command, sizeof(command), &off, " -o \"%s\"", exe_output_path);
    if (!ok) {
        llvm_set_err(err, err_size, "LLVM compile command is too long.");
        return 0;
    }

    int rc = system(command);
    if (rc != 0) {
        (void)rc;
        llvm_set_err(err, err_size, "LLVM build failed. Install clang or set SICHT_LLVM_CLANG.");
        return 0;
    }

    return 1;
}

int llvm_backend_build_exe_from_source(
    const char* runtime_root,
    const char* source_text,
    const char* source_label,
    const ASTNode* program,
    const char* exe_output_path,
    char* err,
    size_t err_size
) {
    (void)source_text;
    if (!runtime_root || runtime_root[0] == '\0') {
        llvm_set_err(err, err_size, "Missing runtime root for LLVM native build.");
        return 0;
    }
    if (!program) {
        llvm_set_err(err, err_size, "Missing AST program for LLVM native build.");
        return 0;
    }
    if (!exe_output_path || exe_output_path[0] == '\0') {
        llvm_set_err(err, err_size, "Missing executable output path.");
        return 0;
    }

    char clang_cmd[2048];
    if (!resolve_clang_command(runtime_root, clang_cmd, sizeof(clang_cmd))) {
        llvm_set_err(err, err_size, "Invalid clang command for LLVM native build.");
        return 0;
    }

    const char* runtime_sources_new[] = {
        "src/runtime/gc.c",
        "src/runtime/value.c",
        "src/core/utils.c",
        "src/runtime/native_rt.c",
        "src/debug/debug.c",
        "src/debug/error.c",
        "src/debug/explain.c",
        "src/debug/runtime_error.c",
        "src/debug/source.c"
    };
    const char* runtime_sources_legacy[] = {
        "src/gc.c",
        "src/value.c",
        "src/utils.c",
        "src/native_rt.c",
        "debug/debug.c",
        "debug/error.c",
        "debug/explain.c",
        "debug/runtime_error.c",
        "debug/source.c"
    };
    const char** runtime_sources = runtime_sources_new;
    size_t source_count = sizeof(runtime_sources_new) / sizeof(runtime_sources_new[0]);

    {
        char probe[1024];
        int n = snprintf(probe, sizeof(probe), "%s/src/runtime/gc.c", runtime_root);
        if (n < 0 || (size_t)n >= sizeof(probe) || !file_exists(probe)) {
            runtime_sources = runtime_sources_legacy;
            source_count = sizeof(runtime_sources_legacy) / sizeof(runtime_sources_legacy[0]);
        }
    }

    char full_paths[32][1024];
    const char* runtime_paths[32];
    if (source_count > 32u) {
        llvm_set_err(err, err_size, "Internal source list overflow.");
        return 0;
    }

    for (size_t i = 0; i < source_count; i++) {
        int n = snprintf(full_paths[i], sizeof(full_paths[i]), "%s/%s", runtime_root, runtime_sources[i]);
        if (n < 0 || (size_t)n >= sizeof(full_paths[i])) {
            llvm_set_err(err, err_size, "Runtime source path is too long.");
            return 0;
        }
        if (!file_exists(full_paths[i])) {
            llvm_set_err(err, err_size, "Missing runtime source files for native LLVM build.");
            return 0;
        }
        runtime_paths[i] = full_paths[i];
    }

    char ir_path[1024];
    int pid = 0;
#ifdef _WIN32
    pid = _getpid();
#else
    pid = (int)getpid();
#endif
    {
        int n = snprintf(ir_path, sizeof(ir_path), "%s.sicht_native_%d.ll", exe_output_path, pid);
        if (n < 0 || (size_t)n >= sizeof(ir_path)) {
            llvm_set_err(err, err_size, "Temporary native IR path is too long.");
            return 0;
        }
    }

    if (!llvm_codegen_write_ir(program, source_label, ir_path, err, err_size))
        return 0;

    char obj_paths[32][1024];
    if (!prepare_runtime_objects(
            runtime_root,
            clang_cmd,
            runtime_paths,
            source_count,
            obj_paths,
            32u,
            err,
            err_size)) {
        maybe_remove_ir(ir_path);
        return 0;
    }

    char command[16384];
    size_t off = 0;
    int ok = append_clang_invocation(command, sizeof(command), &off, clang_cmd);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "Invalid clang command for LLVM native build.");
        return 0;
    }
    ok = appendf(command, sizeof(command), &off, " -std=c11 -O0");
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }
    ok = append_default_compile_flags(command, sizeof(command), &off);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }
    ok = append_user_extra_cflags(command, sizeof(command), &off);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }
    ok = appendf(command, sizeof(command), &off, " -Wno-override-module");
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }
    ok = append_sysroot_flags(command, sizeof(command), &off, runtime_root);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }

    for (size_t i = 0; i < source_count; i++) {
        ok = appendf(command, sizeof(command), &off, " \"%s\"", obj_paths[i]);
        if (!ok) {
            maybe_remove_ir(ir_path);
            llvm_set_err(err, err_size, "LLVM native compile command is too long.");
            return 0;
        }
    }

    ok = appendf(command, sizeof(command), &off, " \"%s\"", ir_path);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }

#ifdef _WIN32
    ok = appendf(command, sizeof(command), &off, " -lwinhttp");
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }
#endif

#ifndef _WIN32
    {
        const char* use_libcurl = getenv("SICHT_USE_LIBCURL");
        if (use_libcurl && use_libcurl[0] != '\0') {
            ok = appendf(command, sizeof(command), &off, " -lcurl");
            if (!ok) {
                maybe_remove_ir(ir_path);
                llvm_set_err(err, err_size, "LLVM native compile command is too long.");
                return 0;
            }
        }
    }
#endif

    ok = appendf(command, sizeof(command), &off, " -o \"%s\"", exe_output_path);
    if (!ok) {
        maybe_remove_ir(ir_path);
        llvm_set_err(err, err_size, "LLVM native compile command is too long.");
        return 0;
    }

    if (llvm_debug_enabled()) {
        fprintf(stderr, "[llvm-debug] native build command:\n%s\n", command);
    }

    int rc = system(command);
    maybe_remove_ir(ir_path);
    if (rc != 0) {
        llvm_set_err(
            err,
            err_size,
            "LLVM native executable build failed. Ensure bundled clang/sysroot is complete (or set SICHT_LLVM_CLANG/SICHT_LLVM_SYSROOT)."
        );
        return 0;
    }

    return 1;
}



