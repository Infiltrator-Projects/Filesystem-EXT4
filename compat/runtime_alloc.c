/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic infallible allocator policy retained for EXT2/3/4 worker.
 * No filesystem structures, layout or writer logic reside in this file.
 */
#include "ld_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *program_name = "EXT.so";
void ld_runtime_set_program_name(const char *name) {
    if (name && *name) program_name = name;
}
void *ld_xmalloc(size_t size) {
    void *ptr = malloc(size == 0u ? 1u : size);
    if (ptr == NULL) {
        fprintf(stderr, "%s: malloc: %s\n", program_name, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return ptr;
}
