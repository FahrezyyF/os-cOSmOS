#include "user/regex.h"

// regex.c builds both for cOSmOS user programs and for the host unit test, so it only uses
// these three library functions (same prototypes as the C standard library)
void *memset(void *s, int c, size_t n);

enum {
    OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_MATCH, OP_BOL, OP_EOL, OP_WORDB, OP_NWORDB,
};

/* ================================ PARSER (pattern -> syntax tree) ================== */

enum {
    N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_CAT, N_ALT, N_REPEAT,
};

#define MAX_NODES   512
#define MAX_DEPTH   64
#define MAX_REPEAT  255
#define REPEAT_INF  0xFFFFu
#define NO_NODE     0xFFFFu

struct Node {
    uint8_t  type;
    uint8_t  c;           // N_CHAR literal, N_CLASS class index
    bool     greedy;
    uint16_t left, right; // N_CAT / N_ALT operands, N_REPEAT child in left
    uint16_t min, max;
};

struct Parser {
    const char   *p;
    struct Regex *re;
    struct Node   nodes[MAX_NODES];
    uint32_t      count;
    uint32_t      depth;
    bool          failed;
};

static struct Parser parser; // ~6 KiB, kept off the stack

static char fold(const struct Regex *re, char c) {
    return (re->icase && c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void fail(struct Parser *ps, const char *message) {
    if (ps->failed)
        return;
    ps->failed = true;
    uint32_t i = 0;
    for (; message[i] != '\0' && i + 1 < sizeof(ps->re->error); i++)
        ps->re->error[i] = message[i];
    ps->re->error[i] = '\0';
}

static uint16_t new_node(struct Parser *ps, uint8_t type) {
    if (ps->count >= MAX_NODES) {
        fail(ps, "pattern too long");
        return NO_NODE;
    }
    struct Node *n = &ps->nodes[ps->count];
    memset(n, 0, sizeof(*n));
    n->type  = type;
    n->left  = NO_NODE;
    n->right = NO_NODE;
    return (uint16_t) ps->count++;
}

static uint16_t binary(struct Parser *ps, uint8_t type, uint16_t left, uint16_t right) {
    if (left == NO_NODE)
        return right;
    if (right == NO_NODE)
        return left;
    uint16_t n = new_node(ps, type);
    if (n != NO_NODE) {
        ps->nodes[n].left  = left;
        ps->nodes[n].right = right;
    }
    return n;
}

/* -- character classes -- */

static void class_set(uint8_t *cls, uint8_t c) {
    cls[c >> 3] |= (uint8_t) (1u << (c & 7));
}

static void class_range(uint8_t *cls, uint8_t lo, uint8_t hi) {
    for (uint32_t c = lo; c <= hi; c++)
        class_set(cls, (uint8_t) c);
}

// \d \w \s (lowercase letter) into a set
static void class_add_shorthand(uint8_t *cls, char kind) {
    switch (kind) {
        case 'd':
            class_range(cls, '0', '9');
            break;
        case 'w':
            class_range(cls, 'a', 'z');
            class_range(cls, 'A', 'Z');
            class_range(cls, '0', '9');
            class_set(cls, '_');
            break;
        case 's':
            class_set(cls, ' ');
            class_range(cls, '\t', '\r');
            break;
    }
}

static bool streq_n(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

// [:name:] inside a bracket expression; p points after "[:"
static bool class_add_posix(struct Parser *ps, uint8_t *cls) {
    static const char *names[] = {"alpha", "digit", "alnum", "upper", "lower", "space", "punct", "xdigit", "blank"};
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint32_t len = 0;
        while (names[i][len] != '\0')
            len++;
        if (!streq_n(ps->p, names[i], len) || ps->p[len] != ':' || ps->p[len + 1] != ']')
            continue;
        switch (i) {
            case 0: class_range(cls, 'a', 'z'); class_range(cls, 'A', 'Z'); break;
            case 1: class_range(cls, '0', '9'); break;
            case 2: class_add_shorthand(cls, 'w'); break;
            case 3: class_range(cls, 'A', 'Z'); break;
            case 4: class_range(cls, 'a', 'z'); break;
            case 5: class_add_shorthand(cls, 's'); break;
            case 6:
                for (uint32_t c = 33; c < 127; c++)
                    if (!is_word_char((char) c) || c == '_')
                        class_set(cls, (uint8_t) c);
                break;
            case 7: class_range(cls, '0', '9'); class_range(cls, 'a', 'f'); class_range(cls, 'A', 'F'); break;
            case 8: class_set(cls, ' '); class_set(cls, '\t'); break;
        }
        ps->p += len + 2;
        return true;
    }
    fail(ps, "unknown [:class:]");
    return false;
}

static int32_t new_class(struct Parser *ps) {
    struct Regex *re = ps->re;
    if (re->class_count >= REGEX_MAX_CLASSES) {
        fail(ps, "too many character classes");
        return -1;
    }
    memset(re->classes[re->class_count], 0, 32);
    return (int32_t) re->class_count++;
}

// Make a set case-insensitive, then optionally invert it
static void class_finish(struct Regex *re, uint8_t *cls, bool negate) {
    if (re->icase) {
        for (uint32_t c = 'a'; c <= 'z'; c++) {
            uint8_t upper = (uint8_t) (c - 32);
            bool any = (cls[c >> 3] >> (c & 7) & 1) || (cls[upper >> 3] >> (upper & 7) & 1);
            if (any) {
                class_set(cls, (uint8_t) c);
                class_set(cls, upper);
            }
        }
    }
    if (negate) {
        for (uint32_t i = 0; i < 32; i++)
            cls[i] = (uint8_t) ~cls[i];
        cls['\n' >> 3] &= (uint8_t) ~(1u << ('\n' & 7));
    }
}

static char escape_char(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        default:  return c;
    }
}

// p points after '['
static uint16_t parse_bracket(struct Parser *ps) {
    int32_t index = new_class(ps);
    if (index < 0)
        return NO_NODE;
    uint8_t *cls = ps->re->classes[index];
    bool negate = false;
    if (*ps->p == '^') {
        negate = true;
        ps->p++;
    }
    bool first = true;
    while (*ps->p != '\0' && (*ps->p != ']' || first)) {
        first = false;
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            ps->p += 2;
            if (!class_add_posix(ps, cls))
                return NO_NODE;
            continue;
        }
        uint8_t lo;
        if (*ps->p == '\\' && ps->p[1] != '\0') {
            char e = ps->p[1];
            ps->p += 2;
            if (e == 'd' || e == 'w' || e == 's') {
                class_add_shorthand(cls, e);
                continue;
            }
            lo = (uint8_t) escape_char(e);
        } else {
            lo = (uint8_t) *ps->p++;
        }
        uint8_t hi = lo;
        if (ps->p[0] == '-' && ps->p[1] != ']' && ps->p[1] != '\0') {
            ps->p++;
            if (*ps->p == '\\' && ps->p[1] != '\0') {
                hi = (uint8_t) escape_char(ps->p[1]);
                ps->p += 2;
            } else {
                hi = (uint8_t) *ps->p++;
            }
            if (hi < lo) {
                fail(ps, "invalid range in [ ]");
                return NO_NODE;
            }
        }
        class_range(cls, lo, hi);
    }
    if (*ps->p != ']') {
        fail(ps, "missing ]");
        return NO_NODE;
    }
    ps->p++;
    class_finish(ps->re, cls, negate);
    uint16_t n = new_node(ps, N_CLASS);
    if (n != NO_NODE)
        ps->nodes[n].c = (uint8_t) index;
    return n;
}

static uint16_t shorthand_class(struct Parser *ps, char e) {
    int32_t index = new_class(ps);
    if (index < 0)
        return NO_NODE;
    uint8_t *cls = ps->re->classes[index];
    char lower = (char) (e | 0x20);
    class_add_shorthand(cls, lower);
    class_finish(ps->re, cls, e != lower); // \D \W \S are the complements
    uint16_t n = new_node(ps, N_CLASS);
    if (n != NO_NODE)
        ps->nodes[n].c = (uint8_t) index;
    return n;
}

static uint16_t parse_alternation(struct Parser *ps);

static uint16_t literal(struct Parser *ps, char c) {
    uint16_t n = new_node(ps, N_CHAR);
    if (n != NO_NODE)
        ps->nodes[n].c = (uint8_t) fold(ps->re, c);
    return n;
}

static uint16_t parse_atom(struct Parser *ps) {
    char c = *ps->p++;
    switch (c) {
        case '.':
            return new_node(ps, N_ANY);
        case '^':
            return new_node(ps, N_BOL);
        case '$':
            return new_node(ps, N_EOL);
        case '[':
            return parse_bracket(ps);
        case '(': {
            if (++ps->depth > MAX_DEPTH) {
                fail(ps, "groups nested too deep");
                return NO_NODE;
            }
            uint16_t inner = parse_alternation(ps);
            ps->depth--;
            if (*ps->p != ')') {
                fail(ps, "missing )");
                return NO_NODE;
            }
            ps->p++;
            return inner == NO_NODE ? new_node(ps, N_EMPTY) : inner;
        }
        case '\\': {
            char e = *ps->p;
            if (e == '\0')
                return literal(ps, '\\');
            ps->p++;
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S')
                return shorthand_class(ps, e);
            if (e == 'b')
                return new_node(ps, N_WORDB);
            if (e == 'B')
                return new_node(ps, N_NWORDB);
            return literal(ps, escape_char(e));
        }
        default:
            return literal(ps, c);
    }
}

// {n}, {n,}, {n,m}; p points at '{'. false (and p unchanged) when it is a plain '{'
static bool parse_braces(struct Parser *ps, uint32_t *min, uint32_t *max) {
    const char *p = ps->p + 1;
    if (*p < '0' || *p > '9')
        return false;
    uint32_t lo = 0, hi;
    while (*p >= '0' && *p <= '9' && lo <= MAX_REPEAT)
        lo = lo * 10 + (uint32_t) (*p++ - '0');
    if (*p == '}') {
        hi = lo;
    } else if (*p == ',') {
        p++;
        if (*p == '}') {
            hi = REPEAT_INF;
        } else {
            if (*p < '0' || *p > '9')
                return false;
            hi = 0;
            while (*p >= '0' && *p <= '9' && hi <= MAX_REPEAT)
                hi = hi * 10 + (uint32_t) (*p++ - '0');
        }
        if (*p != '}')
            return false;
    } else {
        return false;
    }
    if (lo > MAX_REPEAT || (hi != REPEAT_INF && (hi > MAX_REPEAT || hi < lo))) {
        fail(ps, "invalid {n,m} repeat");
        return false;
    }
    ps->p = p + 1;
    *min  = lo;
    *max  = hi;
    return true;
}

static uint16_t parse_repeat(struct Parser *ps) {
    uint16_t atom = parse_atom(ps);
    for (;;) {
        uint32_t min, max;
        char q = *ps->p;
        if (q == '*') {
            min = 0, max = REPEAT_INF;
            ps->p++;
        } else if (q == '+') {
            min = 1, max = REPEAT_INF;
            ps->p++;
        } else if (q == '?') {
            min = 0, max = 1;
            ps->p++;
        } else if (q == '{' && parse_braces(ps, &min, &max)) {
            // parsed
        } else {
            return atom;
        }
        bool greedy = true;
        if (*ps->p == '?') { // lazy quantifier
            greedy = false;
            ps->p++;
        }
        if (atom == NO_NODE)
            return NO_NODE;
        uint8_t t = ps->nodes[atom].type;
        if (t == N_BOL || t == N_EOL || t == N_WORDB || t == N_NWORDB)
            continue; // repeating an anchor changes nothing
        uint16_t r = new_node(ps, N_REPEAT);
        if (r == NO_NODE)
            return NO_NODE;
        ps->nodes[r].left   = atom;
        ps->nodes[r].min    = (uint16_t) min;
        ps->nodes[r].max    = (uint16_t) max;
        ps->nodes[r].greedy = greedy;
        atom = r;
    }
}

static uint16_t parse_concat(struct Parser *ps) {
    uint16_t seq = NO_NODE;
    while (*ps->p != '\0' && *ps->p != '|' && *ps->p != ')' && !ps->failed) {
        char c = *ps->p;
        if ((c == '*' || c == '+' || c == '?') && seq == NO_NODE) {
            // nothing to repeat: take it literally (like GNU grep)
            ps->p++;
            seq = literal(ps, c);
            continue;
        }
        uint16_t item = parse_repeat(ps);
        if (item == NO_NODE)
            return NO_NODE;
        seq = binary(ps, N_CAT, seq, item);
    }
    return seq;
}

static uint16_t parse_alternation(struct Parser *ps) {
    uint16_t left = parse_concat(ps);
    while (*ps->p == '|' && !ps->failed) {
        ps->p++;
        uint16_t right = parse_concat(ps);
        if (left == NO_NODE)
            left = new_node(ps, N_EMPTY);
        if (right == NO_NODE)
            right = new_node(ps, N_EMPTY);
        uint16_t n = new_node(ps, N_ALT);
        if (n == NO_NODE)
            return NO_NODE;
        ps->nodes[n].left  = left;
        ps->nodes[n].right = right;
        left = n;
    }
    return left;
}

/* ================================ COMPILER (tree -> program) ======================= */

static uint32_t emit(struct Parser *ps, uint8_t op, uint8_t c) {
    struct Regex *re = ps->re;
    if (re->length >= REGEX_MAX_PROGRAM) {
        fail(ps, "pattern too complex");
        return 0;
    }
    struct RegexInst *in = &re->prog[re->length];
    in->op = op;
    in->c  = c;
    in->x  = in->y = 0;
    return re->length++;
}

static void set_split(struct Regex *re, uint32_t at, uint32_t body, uint32_t skip, bool greedy) {
    re->prog[at].x = (uint16_t) (greedy ? body : skip);
    re->prog[at].y = (uint16_t) (greedy ? skip : body);
}

static void compile_node(struct Parser *ps, uint16_t index, uint32_t depth) {
    if (index == NO_NODE || ps->failed)
        return;
    if (depth > 2 * MAX_DEPTH + MAX_NODES) {
        fail(ps, "pattern too complex");
        return;
    }
    struct Regex *re = ps->re;
    struct Node *n = &ps->nodes[index];
    switch (n->type) {
        case N_EMPTY:  break;
        case N_CHAR:   emit(ps, OP_CHAR, n->c); break;
        case N_ANY:    emit(ps, OP_ANY, 0); break;
        case N_CLASS:  emit(ps, OP_CLASS, n->c); break;
        case N_BOL:    emit(ps, OP_BOL, 0); break;
        case N_EOL:    emit(ps, OP_EOL, 0); break;
        case N_WORDB:  emit(ps, OP_WORDB, 0); break;
        case N_NWORDB: emit(ps, OP_NWORDB, 0); break;
        case N_CAT:
            compile_node(ps, n->left, depth + 1);
            compile_node(ps, n->right, depth + 1);
            break;
        case N_ALT: {
            // SPLIT L1, L2; L1: left; JMP L3; L2: right; L3:
            uint32_t split = emit(ps, OP_SPLIT, 0);
            compile_node(ps, n->left, depth + 1);
            uint32_t jump = emit(ps, OP_JMP, 0);
            uint32_t right = re->length;
            compile_node(ps, n->right, depth + 1);
            if (ps->failed)
                return;
            set_split(re, split, split + 1, right, true);
            re->prog[jump].x = (uint16_t) re->length;
            break;
        }
        case N_REPEAT: {
            for (uint32_t i = 0; i < n->min; i++)
                compile_node(ps, n->left, depth + 1);
            if (n->max == REPEAT_INF) {
                // L1: SPLIT L2, L3; L2: body; JMP L1; L3:
                uint32_t split = emit(ps, OP_SPLIT, 0);
                compile_node(ps, n->left, depth + 1);
                uint32_t jump = emit(ps, OP_JMP, 0);
                if (ps->failed)
                    return;
                re->prog[jump].x = (uint16_t) split;
                set_split(re, split, split + 1, re->length, n->greedy);
            } else {
                // (max - min) optional copies, each one may skip to the end
                uint32_t splits[MAX_REPEAT];
                uint32_t count = 0;
                for (uint32_t i = n->min; i < n->max && !ps->failed; i++) {
                    splits[count++] = emit(ps, OP_SPLIT, 0);
                    compile_node(ps, n->left, depth + 1);
                }
                if (ps->failed)
                    return;
                for (uint32_t i = 0; i < count; i++)
                    set_split(re, splits[i], splits[i] + 1, re->length, n->greedy);
            }
            break;
        }
    }
}

bool regex_compile(struct Regex *re, const char *pattern, uint32_t flags) {
    struct Parser *ps = &parser;
    re->length      = 0;
    re->class_count = 0;
    re->icase       = (flags & REGEX_ICASE) != 0;
    re->error[0]    = '\0';
    ps->p      = pattern;
    ps->re     = re;
    ps->count  = 0;
    ps->depth  = 0;
    ps->failed = false;

    uint16_t root = parse_alternation(ps);
    if (!ps->failed && *ps->p == ')')
        fail(ps, "unmatched )");
    if (!ps->failed)
        compile_node(ps, root, 0);
    if (!ps->failed)
        emit(ps, OP_MATCH, 0);
    return !ps->failed;
}

/* ================================ PIKE VM ========================================== */

struct Thread {
    uint16_t pc;
    uint32_t start;
};

static struct Thread thread_lists[2][REGEX_MAX_PROGRAM];
static uint32_t      marks[REGEX_MAX_PROGRAM];
static uint32_t      generation = 0;

struct VMContext {
    const struct Regex *re;
    const char         *text;
    uint32_t            len;
};

// Follow jumps / splits / assertions (epsilon moves) and queue the next consuming instruction
static void add_thread(const struct VMContext *vm, struct Thread *list, uint32_t *count, uint32_t pc,
                       uint32_t sp, uint32_t start) {
    if (marks[pc] == generation)
        return; // already queued for this position (by a higher-priority thread)
    marks[pc] = generation;
    const struct RegexInst *in = &vm->re->prog[pc];
    switch (in->op) {
        case OP_JMP:
            add_thread(vm, list, count, in->x, sp, start);
            return;
        case OP_SPLIT:
            add_thread(vm, list, count, in->x, sp, start);
            add_thread(vm, list, count, in->y, sp, start);
            return;
        case OP_BOL:
            if (sp == 0)
                add_thread(vm, list, count, pc + 1, sp, start);
            return;
        case OP_EOL:
            if (sp == vm->len)
                add_thread(vm, list, count, pc + 1, sp, start);
            return;
        case OP_WORDB:
        case OP_NWORDB: {
            bool before   = sp > 0 && is_word_char(vm->text[sp - 1]);
            bool after    = sp < vm->len && is_word_char(vm->text[sp]);
            bool boundary = before != after;
            if (boundary == (in->op == OP_WORDB))
                add_thread(vm, list, count, pc + 1, sp, start);
            return;
        }
        default:
            list[*count].pc    = (uint16_t) pc;
            list[*count].start = start;
            (*count)++;
            return;
    }
}

bool regex_search(const struct Regex *re, const char *text, uint32_t len, uint32_t *match_start, uint32_t *match_end) {
    return regex_search_from(re, text, len, 0, match_start, match_end);
}

bool regex_search_from(const struct Regex *re, const char *text, uint32_t len, uint32_t from,
                       uint32_t *match_start, uint32_t *match_end) {
    if (re->length == 0 || from > len)
        return false;
    struct VMContext vm = {.re = re, .text = text, .len = len};
    struct Thread *clist = thread_lists[0], *nlist = thread_lists[1];
    uint32_t ccount = 0, ncount;
    bool matched = false;
    uint32_t best_start = 0, best_end = 0;

    generation++;
    add_thread(&vm, clist, &ccount, 0, from, from);
    for (uint32_t sp = from;; sp++) {
        generation++;
        ncount = 0;
        for (uint32_t i = 0; i < ccount; i++) {
            const struct Thread *t = &clist[i];
            const struct RegexInst *in = &re->prog[t->pc];
            bool step = false;
            if (in->op == OP_MATCH) {
                matched    = true;
                best_start = t->start;
                best_end   = sp;
                break; // lower-priority threads lose (leftmost-first)
            }
            if (sp < len) {
                char c = text[sp];
                if (in->op == OP_CHAR)
                    step = fold(re, c) == (char) in->c;
                else if (in->op == OP_ANY)
                    step = c != '\n';
                else if (in->op == OP_CLASS)
                    step = (re->classes[in->c][(uint8_t) c >> 3] >> ((uint8_t) c & 7)) & 1;
            }
            if (step)
                add_thread(&vm, nlist, &ncount, t->pc + 1u, sp + 1, t->start);
        }
        if (sp >= len)
            break;
        // Unanchored search: a new attempt at the next position, with the lowest priority
        // (keep going even when no thread is alive: e.g. \bfoo can only start at a word)
        if (!matched)
            add_thread(&vm, nlist, &ncount, 0, sp + 1, sp + 1);
        else if (ncount == 0)
            break;
        struct Thread *tmp = clist;
        clist  = nlist;
        nlist  = tmp;
        ccount = ncount;
    }
    if (matched) {
        if (match_start)
            *match_start = best_start;
        if (match_end)
            *match_end = best_end;
    }
    return matched;
}
