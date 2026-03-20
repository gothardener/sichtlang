#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "utils.h"

char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        printf("Error: Could not read file '%s'\n", path);
        exit(1);
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        fprintf(stderr, "Error: Could not seek in file '%s'\n", path);
        exit(1);
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > (unsigned long)(SIZE_MAX - 1)) {
        fclose(file);
        fprintf(stderr, "Error: Could not determine valid file size for '%s'\n", path);
        exit(1);
    }
    rewind(file);

    char* buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Error: Out of memory while reading '%s'\n", path);
        exit(1);
    }
    size_t read_count = fread(buffer, 1, (size_t)size, file);
    if (read_count != (size_t)size) {
        free(buffer);
        fclose(file);
        fprintf(stderr, "Error: Could not fully read '%s'\n", path);
        exit(1);
    }
    buffer[size] = '\0';
    if (size >= 3) {
        unsigned char b0 = (unsigned char)buffer[0];
        unsigned char b1 = (unsigned char)buffer[1];
        unsigned char b2 = (unsigned char)buffer[2];
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            memmove(buffer, buffer + 3, size - 3);
            size -= 3;
            buffer[size] = '\0';
        }
    }

    fclose(file);
    return buffer;
}

char* str_slice(const char* src, int start, int len) {
    if (start < 0 || len < 0) {
        fprintf(stderr, "Error: Invalid string slice bounds.\n");
        exit(1);
    }
    if ((size_t)len > SIZE_MAX - 1 || (size_t)start > SIZE_MAX - (size_t)len) {
        fprintf(stderr, "Error: Invalid string slice bounds.\n");
        exit(1);
    }
    size_t src_len = strlen(src);
    if ((size_t)start > src_len || (size_t)len > src_len - (size_t)start) {
        fprintf(stderr, "Error: String slice out of range.\n");
        exit(1);
    }
    char* out = malloc((size_t)len + 1);
    if (!out) {
        fprintf(stderr, "Error: Out of memory while slicing string.\n");
        exit(1);
    }
    memcpy(out, src + start, len);
    out[len] = '\0';
    return out;
}


