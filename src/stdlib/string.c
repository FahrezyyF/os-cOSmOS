#include <stdint.h>
#include <stddef.h>
#include "header/stdlib/string.h"

void* memset(void *s, int c, size_t n) {
    uint8_t *buf = (uint8_t*) s;
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t) c;
    return s;
}

void* memcpy(void* restrict dest, const void* restrict src, size_t n) {
    uint8_t *dstbuf       = (uint8_t*) dest;
    const uint8_t *srcbuf = (const uint8_t*) src;
    for (size_t i = 0; i < n; i++)
        dstbuf[i] = srcbuf[i];
    return dstbuf;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *buf1 = (const uint8_t*) s1;
    const uint8_t *buf2 = (const uint8_t*) s2;
    for (size_t i = 0; i < n; i++) {
        if (buf1[i] < buf2[i])
            return -1;
        else if (buf1[i] > buf2[i])
            return 1;
    }

    return 0;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *dstbuf       = (uint8_t*) dest;
    const uint8_t *srcbuf = (const uint8_t*) src;
    if (dstbuf < srcbuf) {
        for (size_t i = 0; i < n; i++)
            dstbuf[i]   = srcbuf[i];
    } else {
        for (size_t i = n; i != 0; i--)
            dstbuf[i-1] = srcbuf[i-1];
    }

    return dest;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n] != '\0')
        n++;
    return n;
}

int strcmp(const char *s1, const char *s2) {
    const uint8_t *a = (const uint8_t*) s1;
    const uint8_t *b = (const uint8_t*) s2;
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int) *a - (int) *b;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    const uint8_t *a = (const uint8_t*) s1;
    const uint8_t *b = (const uint8_t*) s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (int) a[i] - (int) b[i];
    }
    return 0;
}

char *strchr(const char *s, int c) {
    char target = (char) c;
    for (;; s++) {
        if (*s == target)
            return (char*) s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char target = (char) c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == target)
            last = s;
        if (*s == '\0')
            return (char*) last;
    }
}

char *strncpy(char *restrict dest, const char *restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}
