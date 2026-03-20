#ifndef _WIN32
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "native_exe_builder.h"

static void set_err(char* err, size_t err_size, const char* msg) {
    if (err && err_size > 0) {
        snprintf(err, err_size, "%s", msg ? msg : "unknown error");
    }
}

static void set_errf(char* err, size_t err_size, const char* fmt, const char* v) {
    if (err && err_size > 0) {
        snprintf(err, err_size, fmt, v ? v : "");
    }
}

static int streq_ci(const char* a, const char* b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int is_known_toolchain(const char* toolchain) {
    if (!toolchain || toolchain[0] == '\0')
        return 1;
    if (streq_ci(toolchain, "auto"))
        return 1;
    if (streq_ci(toolchain, "llvm"))
        return 1;
    if (streq_ci(toolchain, "gnu"))
        return 1;
    return 0;
}

static int resolve_current_executable_path(
    char* out,
    size_t out_size,
    char* err,
    size_t err_size
) {
    if (!out || out_size < 4u) {
        set_err(err, err_size, "Internal error: executable path buffer is too small.");
        return 0;
    }
    out[0] = '\0';

#ifdef _WIN32
    DWORD got = GetModuleFileNameA(NULL, out, (DWORD)out_size);
    if (got == 0u || got >= (DWORD)out_size) {
        set_err(err, err_size, "Could not resolve current executable path.");
        return 0;
    }
    out[got] = '\0';
    return 1;
#else
    ssize_t got = readlink("/proc/self/exe", out, out_size - 1u);
    if (got <= 0 || (size_t)got >= out_size - 1u) {
        set_err(err, err_size, "Could not resolve current executable path.");
        return 0;
    }
    out[got] = '\0';
    return 1;
#endif
}

static int path_equal_host(const char* a, const char* b) {
    if (!a || !b)
        return 0;
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static int path_full_or_copy(const char* input, char* out, size_t out_size) {
    if (!input || !out || out_size == 0)
        return 0;
#ifdef _WIN32
    DWORD got = GetFullPathNameA(input, (DWORD)out_size, out, NULL);
    if (got == 0 || got >= (DWORD)out_size)
        return 0;
    return 1;
#else
    if (input[0] == '/') {
        size_t len = strlen(input);
        if (len + 1 > out_size)
            return 0;
        memcpy(out, input, len + 1);
        return 1;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return 0;
    int wrote = snprintf(out, out_size, "%s/%s", cwd, input);
    if (wrote <= 0 || (size_t)wrote >= out_size)
        return 0;
    return 1;
#endif
}

static int copy_stream(FILE* in, FILE* out, char* err, size_t err_size) {
    unsigned char buf[64u * 1024u];
    while (1) {
        size_t got = fread(buf, 1, sizeof(buf), in);
        if (got > 0) {
            size_t wrote = fwrite(buf, 1, got, out);
            if (wrote != got) {
                set_err(err, err_size, "Could not write executable output.");
                return 0;
            }
        }
        if (got < sizeof(buf)) {
            if (ferror(in)) {
                set_err(err, err_size, "Could not read current executable bytes.");
                return 0;
            }
            break;
        }
    }
    return 1;
}

static int write_u64_le(FILE* out, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++)
        bytes[i] = (uint8_t)((value >> (i * 8)) & 0xffu);
    return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

int native_exe_builder_build(
    const uint8_t* artifact_data,
    size_t artifact_size,
    const char* exe_output_path,
    const char* toolchain,
    char* err,
    size_t err_size
) {
    FILE* in = NULL;
    FILE* out = NULL;
    int ok = 0;
    char runtime_exe_path[4096];

    if (!artifact_data || artifact_size == 0) {
        set_err(err, err_size, "Missing embedded artifact data for executable build.");
        return 0;
    }
    if (!exe_output_path || exe_output_path[0] == '\0') {
        set_err(err, err_size, "Missing executable output path.");
        return 0;
    }
    if (!is_known_toolchain(toolchain)) {
        set_errf(err, err_size, "Unknown toolchain `%s`.", toolchain);
        return 0;
    }
    if (!resolve_current_executable_path(runtime_exe_path, sizeof(runtime_exe_path), err, err_size))
        return 0;

    {
        char norm_runtime[4096];
        char norm_out[4096];
        if (path_full_or_copy(runtime_exe_path, norm_runtime, sizeof(norm_runtime)) &&
            path_full_or_copy(exe_output_path, norm_out, sizeof(norm_out))) {
            if (path_equal_host(norm_runtime, norm_out)) {
                set_err(err, err_size, "Executable output path must not overwrite the currently running Sicht executable.");
                return 0;
            }
        } else if (path_equal_host(runtime_exe_path, exe_output_path)) {
            set_err(err, err_size, "Executable output path must not overwrite the currently running Sicht executable.");
            return 0;
        }
    }

    if ((uint64_t)artifact_size != artifact_size) {
        set_err(err, err_size, "Embedded artifact is too large for executable trailer format.");
        return 0;
    }

    in = fopen(runtime_exe_path, "rb");
    if (!in) {
        set_errf(err, err_size, "Could not open current executable `%s`.", runtime_exe_path);
        goto cleanup;
    }
    out = fopen(exe_output_path, "wb");
    if (!out) {
        set_errf(err, err_size, "Could not open `%s` for writing.", exe_output_path);
        goto cleanup;
    }

    if (!copy_stream(in, out, err, err_size))
        goto cleanup;

    if (artifact_size > 0 && fwrite(artifact_data, 1, artifact_size, out) != artifact_size) {
        set_err(err, err_size, "Could not append embedded artifact payload.");
        goto cleanup;
    }

    if (!write_u64_le(out, (uint64_t)artifact_size)) {
        set_err(err, err_size, "Could not append embedded artifact size.");
        goto cleanup;
    }

    if (fwrite(
        SICHT_EMBEDDED_ARTIFACT_MAGIC,
        1,
        SICHT_EMBEDDED_ARTIFACT_MAGIC_SIZE,
        out
    ) != SICHT_EMBEDDED_ARTIFACT_MAGIC_SIZE) {
        set_err(err, err_size, "Could not append executable trailer magic.");
        goto cleanup;
    }

    if (fflush(out) != 0) {
        set_err(err, err_size, "Could not flush executable output file.");
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (in)
        fclose(in);
    if (out && fclose(out) != 0 && ok) {
        set_err(err, err_size, "Could not finalize executable output file.");
        ok = 0;
    }
    if (!ok)
        remove(exe_output_path);
    return ok;
}



