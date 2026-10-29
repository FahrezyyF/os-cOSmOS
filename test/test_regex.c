// Host tests for src/user/regex.c (grep -E): fixed cases with expected spans, plus a
// differential test against glibc's POSIX regcomp / regexec on random patterns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <regex.h>
#include "user/regex.h"

static int failures = 0, checks = 0;
static struct Regex re;

#define CHECK(cond, ...) do {                                    \
        checks++;                                                \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "  FAIL line %d: ", __LINE__);       \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

// expect_start < 0: no match expected
static void expect(const char *pattern, uint32_t flags, const char *text, int expect_start, int expect_end) {
    bool compiled = regex_compile(&re, pattern, flags);
    CHECK(compiled, "compile %s: %s", pattern, re.error);
    if (!compiled)
        return;
    uint32_t s = 0, e = 0;
    bool hit = regex_search(&re, text, (uint32_t) strlen(text), &s, &e);
    if (expect_start < 0)
        CHECK(!hit, "/%s/ should not match \"%s\" (got %u-%u)", pattern, text, s, e);
    else
        CHECK(hit && (int) s == expect_start && (int) e == expect_end, "/%s/ on \"%s\": got %s %u-%u, want %d-%d",
              pattern, text, hit ? "match" : "no match", s, e, expect_start, expect_end);
}

static void expect_error(const char *pattern) {
    CHECK(!regex_compile(&re, pattern, 0), "/%s/ should be rejected", pattern);
}

/* -- random pattern generator for the differential test -- */

static uint32_t rng = 2463534242u;
static uint32_t rnd(uint32_t n) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % n;
}

static void gen_alt(char *out, int depth);

static void gen_atom(char *out, int depth) {
    static const char *atoms[] = {"a", "b", "c", ".", "[ab]", "[^a]", "[a-c]", "[bc]", "A"};
    if (depth < 3 && rnd(5) == 0) {
        strcat(out, "(");
        gen_alt(out, depth + 1);
        strcat(out, ")");
    } else {
        strcat(out, atoms[rnd(sizeof(atoms) / sizeof(atoms[0]))]);
    }
    static const char *quants[] = {"", "", "", "*", "+", "?", "{2}", "{1,2}", "{0,2}", "{2,}"};
    strcat(out, quants[rnd(sizeof(quants) / sizeof(quants[0]))]);
}

static void gen_seq(char *out, int depth) {
    uint32_t n = 1 + rnd(3);
    for (uint32_t i = 0; i < n; i++)
        gen_atom(out, depth);
}

static void gen_alt(char *out, int depth) {
    uint32_t n = 1 + rnd(3);
    for (uint32_t i = 0; i < n; i++) {
        if (i > 0)
            strcat(out, "|");
        gen_seq(out, depth);
    }
}

int main(void) {
    // Literals, anchors, classes
    expect("cat", 0, "concatenate", 3, 6);
    expect("cat", 0, "dog", -1, 0);
    expect("^con", 0, "concatenate", 0, 3);
    expect("^cat", 0, "concatenate", -1, 0);
    expect("ate$", 0, "concatenate", 8, 11);
    expect("^$", 0, "", 0, 0);
    expect("^$", 0, "x", -1, 0);
    expect("a.c", 0, "xxabcxx", 2, 5);
    expect("[0-9]+", 0, "room 101b", 5, 8);
    expect("[^a-z ]+", 0, "abc DEF ghi", 4, 7);
    expect("[[:digit:]][[:alpha:]]", 0, "x1y", 1, 3);
    expect("\\d\\d:\\d\\d", 0, "at 12:34 UTC", 3, 8);
    expect("\\w+@\\w+\\.com", 0, "mail me@cosmos.com now", 5, 18);
    expect("\\s+", 0, "a \t b", 1, 4);
    expect("\\bcat\\b", 0, "concat cat", 7, 10);
    expect("\\Bcat", 0, "cat concat", 7, 10);
    expect("a\\.b", 0, "axb a.b", 4, 7);
    expect("[]a]", 0, "x]", 1, 2);
    // Groups, alternation, quantifiers
    expect("gas|ice", 0, "Uranus ice giant", 7, 10);
    expect("(ab)+", 0, "xababab", 1, 7);
    expect("(a|b)*c", 0, "zzababc", 2, 7);
    expect("colou?r", 0, "the color", 4, 9);
    expect("colou?r", 0, "the colour", 4, 10);
    expect("a{3}", 0, "aa aaa", 3, 6);
    expect("a{2,3}", 0, "aaaa", 0, 3);
    expect("a{2,}", 0, "baaaaa", 1, 6);
    expect("x(a|bc){1,2}y", 0, "xbcay", 0, 5);
    expect("a*?b", 0, "aaab", 0, 4);
    expect("<.+?>", 0, "<a><b>", 0, 3);
    expect("<.+>", 0, "<a><b>", 0, 6);
    expect("(a*)*b", 0, "aaab", 0, 4);
    expect("(a*)+$", 0, "b", 1, 1);
    expect("", 0, "anything", 0, 0);
    // Case-insensitive
    expect("JUPITER", REGEX_ICASE, "planet jupiter", 7, 14);
    expect("[a-c]+", REGEX_ICASE, "xxABCxx", 2, 5);
    expect("[^a]", REGEX_ICASE, "aAb", 2, 3);
    expect("JUPITER", 0, "planet jupiter", -1, 0);
    // Leading quantifier is taken literally, like GNU grep
    expect("*star", 0, "a *star", 2, 7);
    // Errors
    expect_error("(abc");
    expect_error("abc)");
    expect_error("[abc");
    expect_error("[z-a]");
    expect_error("a{3,1}");
    expect_error("[[:nope:]]");
    // Pathological pattern stays linear (no exponential backtracking)
    {
        char text[201];
        memset(text, 'a', 200);
        text[200] = '\0';
        expect("(a|aa)*(a|aa)*(a|aa)*b", 0, text, -1, 0);
        expect("(a?){30}a{30}", 0, text, 0, 60); // leftmost-first: every a? takes its 'a'
        expect("(a?){30}a{30}", 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, 39);
    }
    // regex_search_from keeps anchors relative to the whole text
    {
        regex_compile(&re, "^ab", 0);
        uint32_t s, e;
        CHECK(!regex_search_from(&re, "abab", 4, 1, &s, &e), "^ab must not match from offset 1");
        regex_compile(&re, "ab", 0);
        CHECK(regex_search_from(&re, "abab", 4, 1, &s, &e) && s == 2 && e == 4, "second ab at 2-4");
    }

    // Differential test against glibc (match existence and leftmost start agree)
    uint32_t compared = 0, too_complex = 0;
    for (int round = 0; round < 4000; round++) {
        char pattern[512] = "";
        gen_alt(pattern, 0);
        if (rnd(6) == 0) {
            memmove(pattern + 1, pattern, strlen(pattern) + 1);
            pattern[0] = '^';
        }
        if (rnd(6) == 0)
            strcat(pattern, "$");
        bool icase = rnd(4) == 0;
        regex_t posix;
        if (regcomp(&posix, pattern, REG_EXTENDED | (icase ? REG_ICASE : 0)) != 0)
            continue;
        bool ok = regex_compile(&re, pattern, icase ? REGEX_ICASE : 0);
        if (!ok && strcmp(re.error, "pattern too complex") == 0) {
            too_complex++; // nested counted repeats exceed REGEX_MAX_PROGRAM: a resource limit
            regfree(&posix);
            continue;
        }
        CHECK(ok, "compile failed for /%s/: %s", pattern, re.error);
        for (int t = 0; ok && t < 8; t++) {
            char text[16];
            uint32_t len = rnd(13);
            for (uint32_t i = 0; i < len; i++)
                text[i] = "abcdA"[rnd(5)];
            text[len] = '\0';
            regmatch_t m;
            bool want = regexec(&posix, text, 1, &m, 0) == 0;
            uint32_t s = 0, e = 0;
            bool got = regex_search(&re, text, len, &s, &e);
            CHECK(got == want, "/%s/%s on \"%s\": got %d want %d", pattern, icase ? "i" : "", text, got, want);
            if (got && want)
                CHECK((int) s == (int) m.rm_so, "/%s/ on \"%s\": start %u, glibc %d", pattern, text, s, (int) m.rm_so);
            compared++;
        }
        regfree(&posix);
    }
    printf("  differential test: %u pattern/text pairs compared with glibc (%u huge patterns over the size limit)\n",
           compared, too_complex);
    printf("regex: %d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
