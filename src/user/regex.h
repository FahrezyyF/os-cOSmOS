#ifndef _USER_REGEX_H
#define _USER_REGEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Regular expressions for grep (bonus: grep with regex). POSIX ERE syntax:
 *
 *   literals, `.`, `[abc]` `[^a-z]` `[[:digit:]]`, `\d \w \s \D \W \S`, `\b \B`,
 *   anchors `^ $`, groups `( )`, alternation `|`,
 *   quantifiers `* + ? {n} {n,} {n,m}` and their lazy forms (`*?` ...).
 *
 * The pattern is parsed into a syntax tree, compiled to a small instruction program and run
 * by a Pike VM (Thompson NFA simulation): every input position is visited once per
 * instruction, so matching is O(pattern x text) with no exponential backtracking, and the
 * leftmost-first match span is reported (used to highlight matches).
 * No OS dependencies: the same file is unit tested on the host (test/test_regex.c).
 */

#define REGEX_MAX_PROGRAM 1024
#define REGEX_MAX_CLASSES 64
#define REGEX_ICASE       0x1u // ignore case

struct RegexInst {
    uint8_t  op;
    uint8_t  c;     // literal / class index
    uint16_t x, y;  // jump targets
};

struct Regex {
    struct RegexInst prog[REGEX_MAX_PROGRAM];
    uint32_t         length;
    uint8_t          classes[REGEX_MAX_CLASSES][32]; // 256-bit character sets
    uint32_t         class_count;
    bool             icase;
    char             error[64];                     // set when compilation fails
};

/**
 * @param flags REGEX_ICASE or 0
 * @return false on a syntax error (re->error says why)
 */
bool regex_compile(struct Regex *re, const char *pattern, uint32_t flags);

/**
 * Find the leftmost match in text[0, len).
 * @param match_start, match_end Receive the span [start, end) (may be NULL)
 */
bool regex_search(const struct Regex *re, const char *text, uint32_t len, uint32_t *match_start, uint32_t *match_end);

/**
 * Same, but the match must start at or after `from`; anchors (^, \b) still see the whole
 * text, so a later match of "^a" is impossible (used to highlight every match of a line).
 */
bool regex_search_from(const struct Regex *re, const char *text, uint32_t len, uint32_t from,
                       uint32_t *match_start, uint32_t *match_end);

#endif
