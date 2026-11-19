// Host test for src/user/inflate.c: random and structured data compressed by the system
// zlib at every level (stored, fixed and dynamic Huffman blocks) must inflate exactly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "user/inflate.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void roundtrip(const unsigned char *data, uLong n, int level) {
    uLongf clen = compressBound(n);
    unsigned char *c = malloc(clen), *out = malloc(n + 1);
    CHECK(compress2(c, &clen, data, n, level) == Z_OK);
    uint32_t got = 0;
    CHECK(inflate_zlib(c, (uint32_t) clen, out, (uint32_t) n, &got) == 0 && got == n && memcmp(out, data, n) == 0);
    if (n > 10) { // output buffer too small, corrupted stream
        CHECK(inflate_zlib(c, (uint32_t) clen, out, (uint32_t) n - 1, &got) != 0);
        c[clen / 2] ^= 0x55;
        CHECK(inflate_zlib(c, (uint32_t) clen, out, (uint32_t) n, &got) != 0 || memcmp(out, data, n) != 0);
    }
    free(c);
    free(out);
}

int main(void) {
    printf("[inflate] zlib levels 0-9 on random, text-like and run-heavy data\n");
    srand(2130);
    for (int round = 0; round < 60; round++) {
        uLong n = (uLong) (rand() % 200000) + (round < 5 ? 0 : 1);
        unsigned char *d = malloc(n + 1);
        int kind = round % 3;
        for (uLong i = 0; i < n; i++)
            d[i] = kind == 0 ? (unsigned char) rand() : kind == 1 ? (unsigned char) "the cOSmOS kernel "[i % 18 + (rand() % 7 == 0)]
                                                                 : (unsigned char) ((i / (1 + rand() % 50)) & 1 ? 0xFF : 0);
        roundtrip(d, n, round % 10);
        free(d);
    }
    printf("inflate: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
