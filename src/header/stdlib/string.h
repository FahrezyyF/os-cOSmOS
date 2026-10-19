#ifndef _KERNEL_STDLIB_STRING_H
#define _KERNEL_STDLIB_STRING_H

// Note: include guard is not _STRING_H, which collides with glibc <string.h>
// when this header is reused by host-side programs (inserter, tests).

#include <stdint.h>
#include <stddef.h>

/**
 * C standard memset, check man memset or
 * https://man7.org/linux/man-pages/man3/memset.3.html for more details
 *
 * @param s Pointer to memory area to set
 * @param c Constant byte value for filling memory area
 * @param n Memory area size in byte
 *
 * @return Pointer s
*/
void* memset(void *s, int c, size_t n);

/**
 * C standard memcpy, check man memcpy or
 * https://man7.org/linux/man-pages/man3/memcpy.3.html for more details
 *
 * @param dest Starting location for memory area to set
 * @param src Pointer to source memory
 * @param n Memory area size in byte
 *
 * @return Pointer dest
*/
void* memcpy(void* restrict dest, const void* restrict src, size_t n);

/**
 * C standard memcmp, check man memcmp or
 * https://man7.org/linux/man-pages/man3/memcmp.3.html for more details
 *
 * @param s1 Pointer to first memory area
 * @param s2 Pointer to second memory area
 * @param n Memory area size in byte
 *
 * @return Integer as error code, zero for equality, non-zero for inequality
*/
int memcmp(const void *s1, const void *s2, size_t n);

/**
 * C standard memmove, check man memmove or
 * https://man7.org/linux/man-pages/man3/memmove.3.html for more details
 *
 * @param dest Pointer to destination memory
 * @param src Pointer to source memory
 * @param n Memory area size in byte
 *
 * @return Pointer dest
*/
void *memmove(void *dest, const void *src, size_t n);

/**
 * C standard strlen, length of a null-terminated string
 *
 * @param s Null-terminated string
 * @return Number of bytes before the terminating null byte
 */
size_t strlen(const char *s);

/**
 * POSIX strnlen, strlen that never reads more than maxlen bytes
 *
 * @param s      String, not necessarily null-terminated
 * @param maxlen Maximum number of bytes to examine
 * @return min(strlen(s), maxlen)
 */
size_t strnlen(const char *s, size_t maxlen);

/**
 * C standard strcmp, lexicographic comparison as unsigned char
 *
 * @return Negative, zero, or positive like memcmp
 */
int strcmp(const char *s1, const char *s2);

/**
 * C standard strncmp, compare at most n bytes
 *
 * @return Negative, zero, or positive like memcmp
 */
int strncmp(const char *s1, const char *s2, size_t n);

/**
 * C standard strchr, first occurrence of c (converted to char) in s
 *
 * @return Pointer to the matching byte, or NULL. Searching '\0' returns the terminator
 */
char *strchr(const char *s, int c);

/**
 * C standard strrchr, last occurrence of c (converted to char) in s
 *
 * @return Pointer to the matching byte, or NULL
 */
char *strrchr(const char *s, int c);

/**
 * C standard strncpy, copy at most n bytes and pad the rest of dest with '\0'
 * Note: dest is not null-terminated when strlen(src) >= n
 *
 * @return Pointer dest
 */
char *strncpy(char *restrict dest, const char *restrict src, size_t n);

#endif
