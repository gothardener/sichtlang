#include "windows_autoinstall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

static int path_is_safe_for_shell(const char* path) {
    if (!path || path[0] == '\0')
        return 0;
    for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
        if (*p == '"' || *p == '\r' || *p == '\n')
            return 0;
    }
    return 1;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int join_path(char* out, size_t out_size, const char* a, const char* b) {
    if (!out || out_size == 0 || !a || !b)
        return 0;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_sep = (alen > 0 && a[alen - 1] != '\\' && a[alen - 1] != '/');
    size_t need = alen + (size_t)(need_sep ? 1 : 0) + blen + 1;
    if (need > out_size)
        return 0;
    memcpy(out, a, alen);
    size_t off = alen;
    if (need_sep)
        out[off++] = '\\';
    memcpy(out + off, b, blen + 1);
    return 1;
}

static int dirname_of(char* out, size_t out_size, const char* path) {
    if (!out || out_size == 0 || !path || path[0] == '\0')
        return 0;
    size_t len = strlen(path);
    if (len + 1 > out_size)
        return 0;
    memcpy(out, path, len + 1);

    char* slash = strrchr(out, '\\');
    char* fslash = strrchr(out, '/');
    char* cut = slash;
    if (!cut || (fslash && fslash > cut))
        cut = fslash;
    if (!cut)
        return 0;
    *cut = '\0';
    return out[0] != '\0';
}

static int marker_matches(const char* marker_path, const char* root_path) {
    FILE* f = fopen(marker_path, "rb");
    if (!f)
        return 0;

    char line[4096];
    int match = 0;
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[n - 1] = '\0';
            n--;
        }
        if (strcmp(line, root_path) == 0)
            match = 1;
    }
    fclose(f);
    return match;
}

static void write_marker(const char* marker_path, const char* root_path) {
    FILE* f = fopen(marker_path, "wb");
    if (!f)
        return;
    fputs(root_path, f);
    fputc('\n', f);
    fclose(f);
}

void windows_autoinstall_if_needed(const char* exe_path) {
    static int attempted = 0;
    if (attempted)
        return;
    attempted = 1;

    const char* auto_install_env = getenv("SICHT_AUTO_INSTALL");
    if (auto_install_env && auto_install_env[0] == '0' && auto_install_env[1] == '\0')
        return;

    if (!exe_path || exe_path[0] == '\0' || !path_is_safe_for_shell(exe_path))
        return;

    char root_path[4096];
    if (!dirname_of(root_path, sizeof(root_path), exe_path))
        return;
    if (!path_is_safe_for_shell(root_path))
        return;

    char script_path[4096];
    char script_path_fallback[4096];
    char icon_path[4096];
    if (!join_path(script_path, sizeof(script_path), root_path, "register_si_filetype.ps1"))
        return;
    if (!join_path(script_path_fallback, sizeof(script_path_fallback), root_path, "installer\\register_si_filetype.ps1"))
        return;
    if (!join_path(icon_path, sizeof(icon_path), root_path, "image.ico"))
        return;
    if (!file_exists(icon_path))
        return;
    if (!file_exists(script_path)) {
        if (!file_exists(script_path_fallback))
            return;
        memcpy(script_path, script_path_fallback, strlen(script_path_fallback) + 1);
    }
    if (!path_is_safe_for_shell(script_path))
        return;

    const char* local_app_data = getenv("LOCALAPPDATA");
    if (!local_app_data || local_app_data[0] == '\0' || !path_is_safe_for_shell(local_app_data))
        return;

    char marker_dir[4096];
    char marker_path[4096];
    if (!join_path(marker_dir, sizeof(marker_dir), local_app_data, "Sicht"))
        return;
    if (!join_path(marker_path, sizeof(marker_path), marker_dir, "autoinstall.marker"))
        return;

    if (marker_matches(marker_path, root_path))
        return;

    {
        char mkdir_cmd[8192];
        int mk_len = snprintf(mkdir_cmd, sizeof(mkdir_cmd),
            "if not exist \"%s\" mkdir \"%s\" >nul 2>nul",
            marker_dir, marker_dir);
        if (mk_len <= 0 || (size_t)mk_len >= sizeof(mkdir_cmd))
            return;
        (void)system(mkdir_cmd);
    }

    {
        char cmd[16384];
        int len = snprintf(cmd, sizeof(cmd),
            "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\" -ProjectRoot \"%s\" >nul 2>nul",
            script_path, root_path);
        if (len <= 0 || (size_t)len >= sizeof(cmd))
            return;
        if (system(cmd) != 0)
            return;
    }

    write_marker(marker_path, root_path);
}

#else

void windows_autoinstall_if_needed(const char* exe_path) {
    (void)exe_path;
}

#endif


