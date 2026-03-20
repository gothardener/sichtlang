#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "compat.h"

static const char* compat_current = "1.0";
static char compat_requested_buf[16] = {0};
static int compat_initialized = 0;

static const char* compat_env_value(void) {
    const char* env = getenv("SICHT_COMPAT");
    if (env && env[0])
        return env;
    return NULL;
}

static const char* skip_prefix(const char* s, const char* prefix) {
    if (!s || !prefix)
        return s;
    size_t len = strlen(prefix);
    if (strncmp(s, prefix, len) == 0)
        return s + len;
    return s;
}

static int normalize_version(const char* input, char* out, size_t out_size) {
    if (!out || out_size == 0) 
        return 0;
    if (!input || !input[0]) {
        out[0] = '\0';
        return 1;
    }

    const char* p = skip_prefix(input, "v");
    if (!p || !p[0]) {
        out[0] = '\0';
        return 0;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;
    int count = 0;

    if (sscanf(p, "%d.%d.%d", &major, &minor, &patch) == 3)
        count = 3;
    else if (sscanf(p, "%d.%d", &major, &minor) == 2)
        count = 2;
    else if (sscanf(p, "%d", &major) == 1) {
        minor = 0;
        count = 1;
    }

    if (count == 0)
        return 0;
    if (major < 0 || minor < 0)
        return 0;

    snprintf(out, out_size, "%d.%d", major, minor);
    return 1;
}

static int compat_is_supported_internal(const char* normalized) {
    if (!normalized || !normalized[0])
        return 1;
    return strcmp(normalized, compat_current) == 0;
}

const char* compat_current_version(void) {
    return compat_current;
}

const char* compat_requested_version(void) {
    if (!compat_initialized) {
        const char* env = compat_env_value();
        if (env) {
            char normalized[16] = {0};
            if (normalize_version(env, normalized, sizeof(normalized)) &&
                compat_is_supported_internal(normalized)) {
                snprintf(compat_requested_buf, sizeof(compat_requested_buf), "%s", normalized);
            }
        }
        compat_initialized = 1;
    }
    return compat_requested_buf[0] ? compat_requested_buf : NULL;
}

int compat_is_enabled(void) {
    return compat_requested_version() != NULL;
}

int compat_set_requested(const char* requested, char* err, size_t err_size) {
    if (requested && requested[0]) {
        char normalized[16] = {0};
        if (!normalize_version(requested, normalized, sizeof(normalized))) {
            if (err && err_size > 0)
                snprintf(err, err_size, "Invalid compat version: %s", requested);
            return 0;
        }
        if (!compat_is_supported_internal(normalized)) {
            if (err && err_size > 0)
                snprintf(err, err_size, "Unsupported compat version: %s (supported: %s)",
                    normalized, compat_current);
            return 0;
        }
        snprintf(compat_requested_buf, sizeof(compat_requested_buf), "%s", normalized);
        compat_initialized = 1;
        return 1;
    }

    const char* env = compat_env_value();
    if (env && env[0]) {
        return compat_set_requested(env, err, err_size);
    }

    compat_requested_buf[0] = '\0';
    compat_initialized = 1;
    return 1;
}

