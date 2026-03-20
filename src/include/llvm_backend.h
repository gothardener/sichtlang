#ifndef LLVM_BACKEND_H
#define LLVM_BACKEND_H

#include <stddef.h>

#include "ast.h"

int llvm_backend_is_ready(void);
int llvm_backend_build_exe_from_ir(
    const char* ir_path,
    const char* exe_output_path,
    char* err,
    size_t err_size
);
int llvm_backend_build_exe_from_source(
    const char* runtime_root,
    const char* source_text,
    const char* source_label,
    const ASTNode* program,
    const char* exe_output_path,
    char* err,
    size_t err_size
);

#endif

