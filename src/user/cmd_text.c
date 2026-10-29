#include "user/shell.h"
#include "user/regex.h"

/* ================================ echo / write ===================================== */

static void echo_escaped(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\\' && s[1] != '\0') {
            s++;
            switch (*s) {
                case 'n':  out_putc('\n'); break;
                case 't':  out_putc('\t'); break;
                case '\\': out_putc('\\'); break;
                default:   out_putc('\\'); out_putc(*s); break;
            }
        } else {
            out_putc(*s);
        }
    }
}

int cmd_echo(int argc, char **argv, struct Input *in) {
    (void) in;
    bool newline = true, escapes = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        bool option = true;
        for (const char *p = argv[i] + 1; *p; p++)
            if (*p != 'n' && *p != 'e')
                option = false;
        if (!option)
            break;
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n')
                newline = false;
            else
                escapes = true;
        }
    }
    for (int first = i; i < argc; i++) {
        if (i > first)
            out_putc(' ');
        if (escapes)
            echo_escaped(argv[i]);
        else
            out_puts(argv[i]);
    }
    if (newline)
        out_putc('\n');
    return 0;
}

int cmd_write(int argc, char **argv, struct Input *in) {
    if (argc < 2) {
        err_printf("usage: write <file> [text]...   (no text: writes the pipeline input)\n");
        return 2;
    }
    if (argc == 2) {
        if (in == NULL || !in->present) {
            err_printf("write: nothing to write\n");
            return 2;
        }
        return write_whole_file(argv[1], in->data, in->len, false, "write") ? 0 : 1;
    }
    uint32_t len = 0;
    for (int i = 2; i < argc; i++)
        len += (uint32_t) strlen(argv[i]) + 1;
    char *text = malloc(len + 1);
    if (text == NULL)
        return 1;
    text[0] = '\0';
    for (int i = 2; i < argc; i++) {
        strcat_s(text, len + 1, argv[i]);
        strcat_s(text, len + 1, i + 1 < argc ? " " : "\n");
    }
    bool ok = write_whole_file(argv[1], text, (uint32_t) strlen(text), false, "write");
    free(text);
    return ok ? 0 : 1;
}

/* ================================ line helpers ===================================== */

struct Lines {
    char   **line;
    uint32_t count;
};

// Split a buffer in place at '\n' (a '\r' before it is dropped too)
static bool split_lines(char *data, uint32_t len, struct Lines *lines) {
    uint32_t cap = 64;
    lines->count = 0;
    lines->line  = malloc(cap * sizeof(char*));
    if (lines->line == NULL)
        return false;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i < len && data[i] != '\n')
            continue;
        if (i == len && start == len)
            break; // no empty line after a final '\n'
        if (lines->count == cap) {
            cap *= 2;
            char **bigger = realloc(lines->line, cap * sizeof(char*));
            if (bigger == NULL)
                return false;
            lines->line = bigger;
        }
        if (i > start && data[i - 1] == '\r')
            data[i - 1] = '\0';
        if (i < len)
            data[i] = '\0';
        lines->line[lines->count++] = data + start;
        start = i + 1;
    }
    return true;
}

// out_printf() formats into a 512-byte buffer; text lines can be longer
static void print_line(const char *line) {
    out_puts(line);
    out_putc('\n');
}

static bool parse_count_option(int *argc, char ***argv, uint32_t *count, const char *tool) {
    // -n N, -nN or -N
    int i = 1;
    char **av = *argv;
    if (i < *argc && av[i][0] == '-' && is_digit(av[i][1])) {
        if (!parse_uint(av[i] + 1, count)) {
            err_printf("%s: bad count %s\n", tool, av[i]);
            return false;
        }
        i++;
    } else if (i < *argc && strncmp(av[i], "-n", 2) == 0) {
        const char *value = av[i][2] ? av[i] + 2 : (i + 1 < *argc ? av[i + 1] : NULL);
        if (value == NULL || !parse_uint(value, count)) {
            err_printf("%s: -n needs a number\n", tool);
            return false;
        }
        i += av[i][2] ? 1 : 2;
    }
    // shift the options away, keep argv[0]
    *argc -= i - 1;
    for (int k = 1; k < *argc; k++)
        av[k] = av[k + i - 1];
    return true;
}

int cmd_head(int argc, char **argv, struct Input *in) {
    uint32_t count = 10;
    if (!parse_count_option(&argc, &argv, &count, "head"))
        return 2;
    char *data;
    uint32_t len;
    if (!read_all_inputs(argc, argv, 1, in, &data, &len, "head"))
        return 1;
    struct Lines lines;
    if (split_lines(data, len, &lines)) {
        for (uint32_t i = 0; i < lines.count && i < count; i++)
            print_line(lines.line[i]);
        free(lines.line);
    }
    free(data);
    return 0;
}

int cmd_tail(int argc, char **argv, struct Input *in) {
    uint32_t count = 10;
    if (!parse_count_option(&argc, &argv, &count, "tail"))
        return 2;
    char *data;
    uint32_t len;
    if (!read_all_inputs(argc, argv, 1, in, &data, &len, "tail"))
        return 1;
    struct Lines lines;
    if (split_lines(data, len, &lines)) {
        uint32_t first = lines.count > count ? lines.count - count : 0;
        for (uint32_t i = first; i < lines.count; i++)
            print_line(lines.line[i]);
        free(lines.line);
    }
    free(data);
    return 0;
}

static void count_text(const char *data, uint32_t len, uint32_t *lines, uint32_t *words, uint32_t *bytes) {
    bool in_word = false;
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            (*lines)++;
        if (is_space(data[i])) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            (*words)++;
        }
    }
    *bytes += len;
}

static void print_counts(const bool f[128], uint32_t l, uint32_t w, uint32_t b, const char *name) {
    bool all = !f['l'] && !f['w'] && !f['c'];
    if (all || f['l'])
        out_printf("%7u ", l);
    if (all || f['w'])
        out_printf("%7u ", w);
    if (all || f['c'])
        out_printf("%7u ", b);
    out_printf("%s\n", name);
}

int cmd_wc(int argc, char **argv, struct Input *in) {
    bool f[128];
    int first = parse_flags(argc, argv, "lwc", f);
    if (first < 0)
        return 2;
    if (first >= argc) {
        if (in == NULL || !in->present) {
            err_printf("usage: wc [-l] [-w] [-c] [file]... (or pipe text in)\n");
            return 2;
        }
        uint32_t l = 0, w = 0, b = 0;
        count_text(in->data, in->len, &l, &w, &b);
        print_counts(f, l, w, b, "");
        return 0;
    }
    uint32_t tl = 0, tw = 0, tb = 0;
    int status = 0;
    for (int i = first; i < argc; i++) {
        char *data;
        uint32_t len;
        if (!read_whole_file(argv[i], &data, &len, "wc")) {
            status = 1;
            continue;
        }
        uint32_t l = 0, w = 0, b = 0;
        count_text(data, len, &l, &w, &b);
        free(data);
        print_counts(f, l, w, b, argv[i]);
        tl += l;
        tw += w;
        tb += b;
    }
    if (argc - first > 1)
        print_counts(f, tl, tw, tb, "total");
    return status;
}

/* ================================ sort ============================================= */

static bool sort_numeric = false;
static bool sort_reverse = false;

static int compare_lines(const char *a, const char *b) {
    int r;
    if (sort_numeric) {
        int x = atoi(a), y = atoi(b);
        r = x < y ? -1 : (x > y ? 1 : strcmp(a, b));
    } else {
        r = strcmp(a, b);
    }
    return sort_reverse ? -r : r;
}

// Merge sort: O(n log n), stable, scratch array from the heap
static void merge_sort(char **items, char **scratch, uint32_t n) {
    if (n < 2)
        return;
    uint32_t mid = n / 2;
    merge_sort(items, scratch, mid);
    merge_sort(items + mid, scratch, n - mid);
    uint32_t i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        scratch[k++] = compare_lines(items[j], items[i]) < 0 ? items[j++] : items[i++];
    while (i < mid)
        scratch[k++] = items[i++];
    while (j < n)
        scratch[k++] = items[j++];
    memcpy(items, scratch, n * sizeof(char*));
}

int cmd_sort(int argc, char **argv, struct Input *in) {
    bool f[128];
    int first = parse_flags(argc, argv, "rnu", f);
    if (first < 0)
        return 2;
    sort_reverse = f['r'];
    sort_numeric = f['n'];
    char *data;
    uint32_t len;
    if (!read_all_inputs(argc, argv, first, in, &data, &len, "sort"))
        return 1;
    struct Lines lines;
    if (split_lines(data, len, &lines)) {
        char **scratch = malloc((lines.count ? lines.count : 1) * sizeof(char*));
        if (scratch != NULL) {
            merge_sort(lines.line, scratch, lines.count);
            free(scratch);
        }
        for (uint32_t i = 0; i < lines.count; i++) {
            if (f['u'] && i > 0 && strcmp(lines.line[i], lines.line[i - 1]) == 0)
                continue;
            print_line(lines.line[i]);
        }
        free(lines.line);
    }
    free(data);
    return 0;
}

/* ================================ hexdump ========================================== */

int cmd_hexdump(int argc, char **argv, struct Input *in) {
    char *data;
    uint32_t len;
    if (!read_all_inputs(argc, argv, 1, in, &data, &len, "hexdump"))
        return 1;
    for (uint32_t off = 0; off < len; off += 16) {
        out_color(C_DARK_GRAY);
        out_printf("%08x  ", off);
        out_color(C_LIGHT_GRAY);
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < len)
                out_printf("%02x ", (uint8_t) data[off + i]);
            else
                out_puts("   ");
            if (i == 7)
                out_putc(' ');
        }
        out_color(C_LIGHT_CYAN);
        out_puts(" |");
        for (uint32_t i = 0; i < 16 && off + i < len; i++) {
            char c = data[off + i];
            out_putc((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        out_puts("|\n");
        out_color(C_LIGHT_GRAY);
    }
    out_printf("%08x\n", len);
    free(data);
    return 0;
}

/* ================================ grep (bonus: regex) ============================== */

struct GrepOptions {
    bool         ignore_case, invert, numbers, count, files_only, only_matching, fixed, recursive;
    bool         show_names;
    const char  *pattern;
    uint32_t     pattern_len;
    struct Regex regex;
};

static struct GrepOptions grep_opt;

static bool fixed_search(const char *line, uint32_t len, uint32_t from, uint32_t *start, uint32_t *end) {
    uint32_t n = grep_opt.pattern_len;
    if (n == 0) {
        *start = *end = from;
        return true;
    }
    for (uint32_t i = from; i + n <= len; i++) {
        bool hit = grep_opt.ignore_case ? strcasecmp_n(line + i, grep_opt.pattern, n) == 0
                                        : strncmp(line + i, grep_opt.pattern, n) == 0;
        if (hit) {
            *start = i;
            *end   = i + n;
            return true;
        }
    }
    return false;
}

// Next match at or after `from` (absolute offsets inside the line)
static bool grep_search(const char *line, uint32_t len, uint32_t from, uint32_t *start, uint32_t *end) {
    if (grep_opt.fixed)
        return fixed_search(line, len, from, start, end);
    return regex_search_from(&grep_opt.regex, line, len, from, start, end);
}

static void print_match_line(const char *name, uint32_t line_no, const char *line, uint32_t len) {
    if (grep_opt.show_names) {
        out_color(C_LIGHT_MAGENTA);
        out_printf("%s", name);
        out_color(C_LIGHT_CYAN);
        out_putc(':');
    }
    if (grep_opt.numbers) {
        out_color(C_LIGHT_GREEN);
        out_printf("%u", line_no);
        out_color(C_LIGHT_CYAN);
        out_putc(':');
    }
    out_color(C_LIGHT_GRAY);
    if (grep_opt.invert || !out_is_console()) {
        if (grep_opt.only_matching && !grep_opt.invert) {
            uint32_t pos = 0, s, e;
            while (pos <= len && grep_search(line, len, pos, &s, &e)) {
                if (e > s) {
                    out_write(line + s, e - s);
                    out_putc('\n');
                }
                pos = e > s ? e : e + 1;
            }
            return;
        }
        out_write(line, len);
        out_putc('\n');
        return;
    }
    // Highlight every match on the console
    uint32_t pos = 0, s, e;
    while (pos <= len && grep_search(line, len, pos, &s, &e)) {
        if (!grep_opt.only_matching)
            out_write(line + pos, s - pos);
        if (e > s) {
            out_color(C_LIGHT_RED);
            out_write(line + s, e - s);
            out_color(C_LIGHT_GRAY);
            if (grep_opt.only_matching)
                out_putc('\n');
        }
        if (e == s) { // empty match: step over one character
            if (s < len && !grep_opt.only_matching)
                out_putc(line[s]);
            pos = s + 1;
        } else {
            pos = e;
        }
    }
    if (!grep_opt.only_matching) {
        if (pos < len)
            out_write(line + pos, len - pos);
        out_putc('\n');
    }
}

// grep one buffer; returns the number of selected lines
static uint32_t grep_buffer(const char *name, char *data, uint32_t len) {
    struct Lines lines;
    if (!split_lines(data, len, &lines))
        return 0;
    uint32_t selected = 0;
    for (uint32_t i = 0; i < lines.count; i++) {
        const char *line = lines.line[i];
        uint32_t line_len = (uint32_t) strlen(line), s, e;
        bool hit = grep_search(line, line_len, 0, &s, &e);
        if (hit == grep_opt.invert)
            continue;
        selected++;
        if (grep_opt.count || grep_opt.files_only)
            continue;
        print_match_line(name, i + 1, line, line_len);
    }
    free(lines.line);
    if (grep_opt.count) {
        if (grep_opt.show_names)
            out_printf("%s:", name);
        out_printf("%u\n", selected);
    } else if (grep_opt.files_only && selected > 0) {
        out_printf("%s\n", name);
    }
    return selected;
}

static uint32_t grep_path(const char *path, uint32_t depth) {
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return 0;
    if (!r.exists) {
        err_printf("grep: %s: no such file or directory\n", path);
        return 0;
    }
    if (r.type == SYSCALL_TYPE_DIR) {
        if (!grep_opt.recursive) {
            err_printf("grep: %s: is a directory (use -r)\n", path);
            return 0;
        }
        if (depth > 32)
            return 0;
        struct DirEntry *entries;
        uint32_t count, total = 0;
        if (fs_list(r.inode, &entries, &count) != 0)
            return 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t plen = (uint32_t) strlen(path);
            char *child = malloc(plen + entries[i].name_len + 2);
            if (child == NULL)
                break;
            snprintf(child, plen + entries[i].name_len + 2, "%s%s%s", path,
                     (plen > 0 && path[plen - 1] == '/') ? "" : "/", entries[i].name);
            // the shell binary is not text
            if (!(entries[i].type == SYSCALL_TYPE_FILE && strcmp(entries[i].name, "shell") == 0))
                total += grep_path(child, depth + 1);
            free(child);
        }
        free(entries);
        return total;
    }
    char *data;
    uint32_t len;
    if (!read_whole_file(path, &data, &len, "grep"))
        return 0;
    uint32_t n = grep_buffer(path, data, len);
    free(data);
    return n;
}

int cmd_grep(int argc, char **argv, struct Input *in) {
    bool f[128];
    int first = parse_flags(argc, argv, "ivncloFrE", f);
    if (first < 0)
        return 2;
    if (first >= argc) {
        err_printf("usage: grep [-i] [-v] [-n] [-c] [-l] [-o] [-F] [-r] <pattern> [file]...\n");
        return 2;
    }
    memset(&grep_opt, 0, sizeof(grep_opt) - sizeof(grep_opt.regex));
    grep_opt.ignore_case   = f['i'];
    grep_opt.invert        = f['v'];
    grep_opt.numbers       = f['n'];
    grep_opt.count         = f['c'];
    grep_opt.files_only    = f['l'];
    grep_opt.only_matching = f['o'];
    grep_opt.fixed         = f['F'];
    grep_opt.recursive     = f['r'];
    grep_opt.pattern       = argv[first];
    grep_opt.pattern_len   = (uint32_t) strlen(argv[first]);
    if (!grep_opt.fixed && !regex_compile(&grep_opt.regex, grep_opt.pattern, grep_opt.ignore_case ? REGEX_ICASE : 0)) {
        err_printf("grep: invalid regular expression: %s\n", grep_opt.regex.error);
        return 2;
    }
    first++;
    uint32_t selected = 0;
    if (first >= argc) {
        if (in == NULL || !in->present) {
            if (!grep_opt.recursive) {
                err_printf("grep: no input (give files or pipe text in)\n");
                return 2;
            }
            grep_opt.show_names = true;
            selected = grep_path(".", 0);
        } else {
            char *copy = strndup(in->data ? in->data : "", in->len);
            if (copy == NULL)
                return 2;
            selected = grep_buffer("(stdin)", copy, in->len);
            free(copy);
        }
    } else {
        grep_opt.show_names = argc - first > 1 || grep_opt.recursive;
        for (int i = first; i < argc; i++)
            selected += grep_path(argv[i], 0);
    }
    return selected > 0 ? 0 : 1;
}
