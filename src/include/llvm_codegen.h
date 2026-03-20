#ifndef LLVM_CODEGEN_H
#define LLVM_CODEGEN_H

#include <stddef.h>
#include "ast.h"

int llvm_codegen_write_ir(
    const ASTNode* program,
    const char* source_label,
    const char* output_path,
    char* err,
    size_t err_size
);

#endif

