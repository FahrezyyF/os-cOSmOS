// Host tests for src/stdlib/format.c (kernel vsnprintf), compared against glibc snprintf
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

int k_snprintf(char *buf, size_t size, const char *fmt, ...);

static int failures = 0, checks = 0;

#define SAME(fmt, ...) do {                                                        \
        char got[128], want[128];                                                  \
        int rg = k_snprintf(got, sizeof(got), fmt, __VA_ARGS__);                   \
        int rw = snprintf(want, sizeof(want), fmt, __VA_ARGS__);                   \
        checks++;                                                                  \
        if (rg != rw || strcmp(got, want) != 0) {                                  \
            failures++;                                                            \
            fprintf(stderr, "  FAIL %-12s got [%s] (%d) want [%s] (%d)\n", fmt, got, rg, want, rw); \
        }                                                                          \
    } while (0)

int main(void) {
    int32_t ints[] = {0, 1, -1, 42, -42, 123456789, INT32_MAX, INT32_MIN};
    for (unsigned i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        int32_t v = ints[i];
        SAME("%d", v); SAME("%i", v); SAME("%5d", v); SAME("%-5d|", v); SAME("%05d", v);
        SAME("%+d", v); SAME("% d", v); SAME("%.3d", v); SAME("%8.3d", v); SAME("%-+8d|", v);
        SAME("%u", (uint32_t) v); SAME("%x", (uint32_t) v); SAME("%X", (uint32_t) v); SAME("%o", (uint32_t) v);
        SAME("%08x", (uint32_t) v); SAME("%hd", v); SAME("%hhu", (uint32_t) v);
    }
    SAME("%s", "hello"); SAME("%10s|", "hi"); SAME("%-10s|", "hi"); SAME("%.2s", "hello"); SAME("%*s|", 6, "ab");
    SAME("%-*s|", 6, "ab"); SAME("%.*s", 3, "abcdef"); SAME("%c%c%c", 'a', 'b', 'c'); SAME("%3c|", 'z'); SAME("100%%", 0);
    SAME("%s and %d and %x", "mix", -7, 255u);
    SAME("%.0d|", 0);

    // Truncation keeps C99 semantics: returns the full length, always NUL terminates
    char small[6];
    int r = k_snprintf(small, sizeof(small), "%s", "0123456789");
    checks++;
    if (r != 10 || strcmp(small, "01234") != 0) { failures++; fprintf(stderr, "  FAIL truncation [%s] %d\n", small, r); }
    r = k_snprintf(NULL, 0, "%d", 12345);
    checks++;
    if (r != 5) { failures++; fprintf(stderr, "  FAIL size 0 -> %d\n", r); }

    printf("format: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
