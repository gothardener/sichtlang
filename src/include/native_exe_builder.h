#ifndef NATIVE_EXE_BUILDER_H
#define NATIVE_EXE_BUILDER_H

#include <stddef.h>
#include <stdint.h>

#define SICHT_EMBEDDED_ARTIFACT_MAGIC "SICHTPK1"
#define SICHT_EMBEDDED_ARTIFACT_MAGIC_SIZE 8u
#define SICHT_EMBEDDED_ARTIFACT_TRAILER_SIZE (SICHT_EMBEDDED_ARTIFACT_MAGIC_SIZE + 8u)

int native_exe_builder_build(
    const uint8_t* artifact_data,
    size_t artifact_size,
    const char* exe_output_path,
    const char* toolchain,
    char* err,
    size_t err_size
);

#endif

