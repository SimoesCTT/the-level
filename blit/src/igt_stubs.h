#ifndef IGT_STUBS_H
#define IGT_STUBS_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Minimal replacements for IGT core symbols used by intel_allocator.c */
#define igt_assert(expr) assert(expr)
#define igt_assert_f(expr, ...) assert(expr)
#define igt_assert_eq(a, b) assert((a) == (b))
#define igt_assert_neq(a, b) assert((a) != (b))

#define igt_info(...) do {} while (0)
#define igt_debug(...) do {} while (0)
#define igt_warn(...) do {} while (0)

/* The allocator uses these types from igt_core.h */
typedef uint64_t igt_handle_t;

#endif
