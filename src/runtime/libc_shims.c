/*
 * C library entry points the title's clean-room libc does not provide but the
 * SDK's libc++/libunwind archives reference (as in SymphonyStation5's
 * src/runtime/runtime_shims.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

__attribute__((noreturn)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    fprintf(stderr, "assertion failed: %s (%s:%d, %s)\n", expression, file, line, function);
    abort();
}
