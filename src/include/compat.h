#ifndef SICHT_COMPAT_H
#define SICHT_COMPAT_H

#include <stddef.h>

const char* compat_current_version(void);
const char* compat_requested_version(void);
int compat_is_enabled(void);

int compat_set_requested(const char* requested, char* err, size_t err_size);

#endif
