// Host tests for src/stdlib/string.c (compiled with k_ prefixed symbols, compared with glibc)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void  *k_memset(void *s, int c, size_t n);
void  *k_memcpy(void *restrict dest, const void *restrict src, size_t n);
int    k_memcmp(const void *s1, const void *s2, size_t n);
void  *k_memmove(void *dest, const void *src, size_t n);
size_t k_strlen(const char *s);
size_t k_strnlen(const char *s, size_t maxlen);
int    k_strcmp(const char *s1, const char *s2);
int    k_strncmp(const char *s1, const char *s2, size_t n);
char  *k_strchr(const char *s, int c);
char  *k_strrchr(const char *s, int c);
char  *k_strncpy(char *restrict dest, const char *restrict src, size_t n);

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int sign(int x) { return (x > 0) - (x < 0); }

int main(void) {
    uint8_t a[300], b[300], c[300];
    for (int i = 0; i < 300; i++) a[i] = (uint8_t) (i * 7 + 3);

    // memset / memcpy / memcmp
    for (size_t n = 0; n < 260; n += 37) {
        memset(b, 0x11, sizeof(b)); memset(c, 0x11, sizeof(c));
        CHECK(k_memset(b + 3, 0xAB, n) == b + 3, "memset return");
        memset(c + 3, 0xAB, n);
        CHECK(memcmp(b, c, sizeof(b)) == 0, "memset n=%zu", n);
        CHECK(k_memcpy(b, a, n) == b, "memcpy return");
        CHECK(memcmp(b, a, n) == 0 && b[n] == c[n], "memcpy n=%zu", n);
    }
    uint8_t x[] = {1, 2, 3, 200}, y[] = {1, 2, 4, 1};
    CHECK(sign(k_memcmp(x, y, 4)) == sign(memcmp(x, y, 4)) && k_memcmp(x, y, 2) == 0, "memcmp");
    CHECK(k_memcmp(x + 3, y + 3, 1) > 0, "memcmp is unsigned");

    // memmove both directions with overlap
    for (int shift = -5; shift <= 5; shift++) {
        uint8_t m1[64], m2[64];
        for (int i = 0; i < 64; i++) m1[i] = m2[i] = (uint8_t) i;
        k_memmove(m1 + 20 + shift, m1 + 20, 30);
        memmove(m2 + 20 + shift, m2 + 20, 30);
        CHECK(memcmp(m1, m2, 64) == 0, "memmove shift %d", shift);
    }

    // string functions
    const char *s[] = {"", "a", "abc", "abd", "ab", "hello world", "\xff\x01", "zz"};
    for (int i = 0; i < 8; i++) {
        CHECK(k_strlen(s[i]) == strlen(s[i]), "strlen %d", i);
        CHECK(k_strnlen(s[i], 2) == strnlen(s[i], 2), "strnlen %d", i);
        CHECK(k_strchr(s[i], 'b') == strchr(s[i], 'b') && k_strchr(s[i], '\0') == strchr(s[i], '\0'), "strchr %d", i);
        CHECK(k_strrchr(s[i], 'l') == strrchr(s[i], 'l'), "strrchr %d", i);
        for (int j = 0; j < 8; j++) {
            CHECK(sign(k_strcmp(s[i], s[j])) == sign(strcmp(s[i], s[j])), "strcmp %d %d", i, j);
            for (size_t n = 0; n < 5; n++)
                CHECK(sign(k_strncmp(s[i], s[j], n)) == sign(strncmp(s[i], s[j], n)), "strncmp %d %d %zu", i, j, n);
        }
    }
    char d1[8], d2[8];
    memset(d1, 'x', 8); memset(d2, 'x', 8);
    k_strncpy(d1, "abc", 6); strncpy(d2, "abc", 6);
    CHECK(memcmp(d1, d2, 8) == 0, "strncpy pads with NUL");
    memset(d1, 'x', 8); memset(d2, 'x', 8);
    k_strncpy(d1, "abcdefgh", 4);
    memcpy(d2, "abcd", 4); // what strncpy must produce: 4 bytes, no terminator
    CHECK(memcmp(d1, d2, 8) == 0, "strncpy truncates");

    printf("string: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
