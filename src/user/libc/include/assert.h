#ifndef _COSMOS_ASSERT_H
#define _COSMOS_ASSERT_H
#include "cosmos_libc.h"
void cosmos_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#define assert(e) ((e) ? (void) 0 : cosmos_assert_fail(#e, __FILE__, __LINE__))
#endif
