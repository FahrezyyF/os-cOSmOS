#ifndef _KERNEL_STDLIB_FORMAT_H
#define _KERNEL_STDLIB_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/**
 * Freestanding vsnprintf subset used by kprintf() and the serial logger.
 *
 * Supported conversions: %d %i %u %x %X %o %c %s %p %%
 * Supported flags/width/precision: '-', '0', '+', ' ', width (number or *),
 * precision (number or *, for strings and integers), length modifiers h, hh, l, z.
 * 64-bit (ll) conversions are intentionally unsupported because the kernel is
 * linked without libgcc (no __udivdi3).
 *
 * @param buf  Destination buffer, always null-terminated when size > 0
 * @param size Size of buf in bytes
 * @param fmt  printf-style format string
 * @param ap   Variadic argument list
 * @return Number of characters that would have been written (excluding '\0'),
 *         like C99 vsnprintf, so truncation can be detected with ret >= size
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/**
 * snprintf wrapper around vsnprintf
 */
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
