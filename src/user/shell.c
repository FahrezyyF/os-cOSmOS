#include "user/shell.h"

#define HISTORY_MAX      500
#define SCRIPT_DEPTH_MAX 8
#define VAR_NAME_MAX     32

uint8_t screen_cols = 80;
uint8_t screen_rows = 24;

static char fs_label[16] = "fs";

/* ================================ OUTPUT =========================================== */

static struct Output console_out = {.console = true, .color = C_LIGHT_GRAY};
struct Output *out = &console_out;

static void flush_console_line(struct Output *o) {
    if (o->line_len > 0) {
        console_raw_write(o->line, o->line_len, o->color);
        o->line_len = 0;
    }
}

void out_write(const char *s, uint32_t n) {
    struct Output *o = out;
    if (!o->console) {
        if (o->len + n + 1 > o->cap) {
            uint32_t cap = o->cap < 256 ? 256 : o->cap;
            while (cap < o->len + n + 1)
                cap *= 2;
            char *bigger = realloc(o->data, cap);
            if (bigger == NULL)
                return; // out of memory: the rest of the output is dropped
            o->data = bigger;
            o->cap  = cap;
        }
        memcpy(o->data + o->len, s, n);
        o->len += n;
        o->data[o->len] = '\0';
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        o->line[o->line_len++] = s[i];
        if (s[i] == '\n' || o->line_len == sizeof(o->line))
            flush_console_line(o);
    }
}

void out_puts(const char *s) {
    out_write(s, strlen(s));
}

void out_putc(char c) {
    out_write(&c, 1);
}

void out_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    out_write(buf, (uint32_t) n < sizeof(buf) ? (uint32_t) n : sizeof(buf) - 1);
}

void out_color(uint8_t color) {
    if (!out->console)
        return;
    flush_console_line(out);
    out->color = color;
}

void out_flush(void) {
    if (out->console)
        flush_console_line(out);
}

bool out_is_console(void) {
    return out->console;
}

static void vcolor_print(uint8_t color, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return;
    flush_console_line(&console_out);
    console_raw_write(buf, (uint32_t) n < sizeof(buf) ? (uint32_t) n : sizeof(buf) - 1, color);
}

void err_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vcolor_print(C_LIGHT_RED, fmt, ap);
    va_end(ap);
}

void note_printf(uint8_t color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vcolor_print(color, fmt, ap);
    va_end(ap);
}

/* ================================ PATHS ============================================ */

struct PathStack cwd = {.inode = {FS_ROOT}, .depth = 0};

void path_copy(struct PathStack *dst, const struct PathStack *src) {
    dst->depth    = src->depth;
    dst->inode[0] = src->inode[0];
    for (uint32_t i = 1; i <= src->depth; i++) {
        dst->inode[i] = src->inode[i];
        dst->name[i]  = strdup(src->name[i]);
    }
}

void path_free(struct PathStack *p) {
    for (uint32_t i = 1; i <= p->depth; i++)
        free(p->name[i]);
    p->depth = 0;
}

static void path_root(struct PathStack *p) {
    p->depth    = 0;
    p->inode[0] = FS_ROOT;
}

static bool path_push(struct PathStack *p, uint32_t inode, const char *name) {
    if (p->depth >= SHELL_DEPTH_MAX)
        return false;
    p->depth++;
    p->inode[p->depth] = inode;
    p->name[p->depth]  = strdup(name);
    return true;
}

static void path_pop(struct PathStack *p) {
    if (p->depth > 0)
        free(p->name[p->depth--]);
}

void path_format(const struct PathStack *p, char *buf, uint32_t cap) {
    buf[0] = '\0';
    if (p->depth == 0) {
        strcat_s(buf, cap, "/");
        return;
    }
    for (uint32_t i = 1; i <= p->depth; i++) {
        strcat_s(buf, cap, "/");
        strcat_s(buf, cap, p->name[i]);
    }
}

bool path_contains(const struct PathStack *p, uint32_t inode) {
    for (uint32_t i = 0; i <= p->depth; i++)
        if (p->inode[i] == inode)
            return true;
    return false;
}

void path_of_inode(uint32_t inode, char *buf, uint32_t cap) {
    char *names[SHELL_DEPTH_MAX];
    uint32_t depth = 0;
    char name[SHELL_NAME_MAX + 1];
    while (inode != FS_ROOT && depth < SHELL_DEPTH_MAX) {
        uint32_t parent;
        if (!fs_name_of(inode, &parent, name))
            break;
        names[depth++] = strdup(name);
        inode = parent;
    }
    buf[0] = '\0';
    if (depth == 0)
        strcat_s(buf, cap, "/");
    while (depth > 0) {
        strcat_s(buf, cap, "/");
        strcat_s(buf, cap, names[--depth]);
        free(names[depth]);
    }
}

void cwd_refresh(void) {
    uint32_t chain[SHELL_DEPTH_MAX + 1];
    char *names[SHELL_DEPTH_MAX + 1];
    uint32_t depth = 0, inode = cwd.inode[cwd.depth];
    char name[SHELL_NAME_MAX + 1];
    bool ok = true;
    while (inode != FS_ROOT) {
        uint32_t parent;
        if (depth >= SHELL_DEPTH_MAX || !fs_name_of(inode, &parent, name)) {
            ok = false;
            break;
        }
        chain[depth]   = inode;
        names[depth++] = strdup(name);
        inode = parent;
    }
    path_free(&cwd);
    path_root(&cwd);
    if (ok) {
        while (depth > 0) {
            depth--;
            path_push(&cwd, chain[depth], names[depth]);
            free(names[depth]);
        }
    } else {
        while (depth > 0)
            free(names[--depth]);
    }
}

const char *fs_error(const char *op, int8_t code) {
    if (strcmp(op, "read") == 0) {
        switch (code) {
            case 1: return "is a directory";
            case 2: return "file too large for the buffer";
            case 3: return "no such file";
            case 4: return "invalid parent directory";
        }
    } else if (strcmp(op, "write") == 0) {
        switch (code) {
            case 1: return "already exists";
            case 2: return "invalid parent directory";
            case -1: return "invalid name or file system full";
        }
    } else if (strcmp(op, "delete") == 0) {
        switch (code) {
            case 1: return "no such file or directory";
            case 2: return "directory not empty";
            case 3: return "invalid parent directory";
        }
    } else if (strcmp(op, "rename") == 0) {
        switch (code) {
            case 1: return "no such file or directory";
            case 2: return "target already exists";
            case 3: return "invalid directory";
            case 4: return "cannot move a directory into itself";
        }
    }
    return "unknown error";
}

bool resolve_path(const char *path, struct Resolved *r, struct PathStack *stack_out, bool quiet) {
    memset(r, 0, sizeof(*r));
    struct PathStack st;
    if (path[0] == '/')
        path_root(&st);
    else
        path_copy(&st, &cwd);

    // Split into components; the last one is the leaf
    const char *p = path;
    char comp[SHELL_NAME_MAX + 2];
    bool have_leaf = false;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        uint32_t n = 0;
        while (p[n] != '\0' && p[n] != '/')
            n++;
        if (n > SHELL_NAME_MAX) {
            if (!quiet)
                err_printf("%s: name too long\n", path);
            path_free(&st);
            return false;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        p += n;
        const char *rest = p;
        while (*rest == '/')
            rest++;
        bool last = *rest == '\0';

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            path_pop(&st);
            continue;
        }
        if (last) {
            memcpy(r->name, comp, n + 1);
            have_leaf = true;
            break;
        }
        struct SyscallStat s;
        if (fs_stat(st.inode[st.depth], comp, (uint8_t) n, &s) != 0 || s.type != SYSCALL_TYPE_DIR) {
            if (!quiet)
                err_printf("%s: no such directory: %s\n", path, comp);
            path_free(&st);
            return false;
        }
        if (!path_push(&st, s.inode, comp)) {
            if (!quiet)
                err_printf("%s: path too deep\n", path);
            path_free(&st);
            return false;
        }
    }

    if (have_leaf) {
        r->parent = st.inode[st.depth];
        if (fs_stat(r->parent, r->name, (uint8_t) strlen(r->name), &r->st) == 0) {
            r->exists = true;
            r->inode  = r->st.inode;
            r->type   = r->st.type;
            if (r->type == SYSCALL_TYPE_DIR)
                path_push(&st, r->inode, r->name);
        }
    } else {
        // "/", "", "." or ".." at the end: the directory on top of the stack
        r->exists = true;
        r->inode  = st.inode[st.depth];
        r->type   = SYSCALL_TYPE_DIR;
        r->parent = st.depth > 0 ? st.inode[st.depth - 1] : FS_ROOT;
        if (st.depth > 0)
            strncpy(r->name, st.name[st.depth], SHELL_NAME_MAX);
        fs_stat(r->inode, ".", 1, &r->st);
        r->st.inode = r->inode;
        r->st.type  = SYSCALL_TYPE_DIR;
    }
    if (stack_out != NULL)
        *stack_out = st; // ownership of the names moves to the caller
    else
        path_free(&st);
    return true;
}

/* ================================ VARIABLES ======================================== */

struct Var {
    char  name[VAR_NAME_MAX + 1];
    char *value;
};

static struct Var *vars      = NULL;
static uint32_t    var_count = 0;
static uint32_t    var_cap   = 0;
static int         last_status = 0;

// Positional parameters of the running script ($0 $1 ... $#)
static int    script_argc = 0;
static char **script_argv = NULL;

const char *shell_get_var(const char *name) {
    for (uint32_t i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    return NULL;
}

void shell_set_var(const char *name, const char *value) {
    for (uint32_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            free(vars[i].value);
            vars[i].value = strdup(value);
            return;
        }
    }
    if (var_count == var_cap) {
        uint32_t cap = var_cap == 0 ? 8 : var_cap * 2;
        struct Var *bigger = realloc(vars, cap * sizeof(*vars));
        if (bigger == NULL)
            return;
        vars    = bigger;
        var_cap = cap;
    }
    strncpy(vars[var_count].name, name, VAR_NAME_MAX);
    vars[var_count].name[VAR_NAME_MAX] = '\0';
    vars[var_count].value = strdup(value);
    var_count++;
}

void shell_unset_var(const char *name) {
    for (uint32_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            free(vars[i].value);
            vars[i] = vars[--var_count];
            return;
        }
    }
}

void shell_print_vars(void) {
    for (uint32_t i = 0; i < var_count; i++) {
        out_printf("%s=", vars[i].name);
        out_puts(vars[i].value); // values have no length limit
        out_putc('\n');
    }
}

static bool is_var_start(char c) {
    return is_alpha(c) || c == '_';
}

static bool is_var_char(char c) {
    return is_alnum(c) || c == '_';
}

/* ================================ HISTORY ========================================== */

static char   **history       = NULL;
static uint32_t history_count = 0;  // entries stored
static uint32_t history_total = 0;  // commands ever entered (numbering)

static void history_add(const char *line) {
    if (line[0] == '\0' || (history_count > 0 && strcmp(history[history_count - 1], line) == 0))
        return;
    if (history == NULL) {
        history = malloc(HISTORY_MAX * sizeof(char*));
        if (history == NULL)
            return;
    }
    if (history_count == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(char*));
        history_count--;
    }
    history[history_count++] = strdup(line);
    history_total++;
}

void shell_print_history(void) {
    uint32_t first = history_total - history_count + 1;
    for (uint32_t i = 0; i < history_count; i++) {
        out_printf("%5u  ", first + i);
        out_puts(history[i]);
        out_putc('\n');
    }
}

/* ================================ PROMPT & STATUS ================================== */

void shell_update_status(void) {
    char path[SHELL_PATH_MAX], text[SHELL_PATH_MAX + 32];
    path_format(&cwd, path, sizeof(path));
    snprintf(text, sizeof(text), "%s:%s", fs_label, path);
    set_status(text);
}

static void print_prompt(void) {
    char path[SHELL_PATH_MAX];
    path_format(&cwd, path, sizeof(path));
    note_printf(C_YELLOW, "[%s] ", fs_label);
    note_printf(C_LIGHT_BLUE, "%s", path);
    note_printf(C_WHITE, " $ ");
}

/* ================================ LINE EDITOR ====================================== */

struct Editor {
    char    *buf;
    uint32_t len;
    uint32_t cap;
    uint32_t cursor;
    int32_t  row0;   // where the input starts on screen (may scroll)
    uint8_t  col0;
    uint32_t drawn;  // characters currently on screen
};

static struct Editor ed;

static bool ed_reserve(uint32_t len) {
    if (len + 1 <= ed.cap)
        return true;
    uint32_t cap = ed.cap < 128 ? 128 : ed.cap;
    while (cap < len + 1)
        cap *= 2;
    char *bigger = realloc(ed.buf, cap); // the command line has no fixed limit (heap)
    if (bigger == NULL)
        return false;
    ed.buf = bigger;
    ed.cap = cap;
    return true;
}

static void ed_place(uint32_t index) {
    uint32_t pos = ed.col0 + index;
    int32_t row = ed.row0 + (int32_t) (pos / screen_cols);
    if (row < 0)
        row = 0;
    console_set_cursor((uint8_t) row, (uint8_t) (pos % screen_cols));
}

// Rewrite buf[from, len) and erase what is left of a longer previous text
static void ed_redraw(uint32_t from) {
    out_flush();
    ed_place(from);
    uint32_t extra = ed.drawn > ed.len ? ed.drawn - ed.len : 0;
    console_raw_write(ed.buf + from, ed.len - from, C_WHITE);
    static const char spaces[32] = "                                ";
    for (uint32_t left = extra; left > 0;) {
        uint32_t n = left > sizeof(spaces) ? sizeof(spaces) : left;
        console_raw_write(spaces, n, C_WHITE);
        left -= n;
    }
    if (ed.len - from + extra > 0) {
        // the console may have scrolled: find the start again from the end position
        uint8_t row, col;
        console_get_cursor(&row, &col);
        uint32_t end = ed.col0 + ed.len + extra;
        ed.row0 = (int32_t) row - (int32_t) (end / screen_cols);
    }
    ed.drawn = ed.len;
    ed_place(ed.cursor);
}

static void ed_begin(void) {
    out_flush();
    uint8_t row, col;
    console_get_cursor(&row, &col);
    ed.row0   = row;
    ed.col0   = col;
    ed.len    = 0;
    ed.cursor = 0;
    ed.drawn  = 0;
    ed_reserve(0);
    ed.buf[0] = '\0';
}

static void ed_set_text(const char *text) {
    uint32_t n = (uint32_t) strlen(text);
    if (!ed_reserve(n))
        return;
    memcpy(ed.buf, text, n + 1);
    ed.len    = n;
    ed.cursor = n;
    ed_redraw(0);
}

static void ed_insert(const char *s, uint32_t n) {
    if (n == 0 || !ed_reserve(ed.len + n))
        return;
    memmove(ed.buf + ed.cursor + n, ed.buf + ed.cursor, ed.len - ed.cursor + 1);
    memcpy(ed.buf + ed.cursor, s, n);
    ed.len    += n;
    uint32_t from = ed.cursor;
    ed.cursor += n;
    ed_redraw(from);
}

static void ed_delete(uint32_t at, uint32_t n) {
    if (at + n > ed.len)
        n = ed.len - at;
    if (n == 0)
        return;
    memmove(ed.buf + at, ed.buf + at + n, ed.len - at - n + 1);
    ed.len -= n;
    if (ed.cursor > at + n)
        ed.cursor -= n;
    else if (ed.cursor > at)
        ed.cursor = at;
    ed_redraw(at);
}

static void ed_reprint(void) {
    print_prompt();
    char *saved = strndup(ed.buf, ed.len);
    uint32_t cursor = ed.cursor;
    ed_begin();
    if (saved != NULL) {
        ed_set_text(saved);
        free(saved);
    }
    ed.cursor = cursor <= ed.len ? cursor : ed.len;
    ed_place(ed.cursor);
}

/* -- Tab completion (bonus) -- */

struct Candidates {
    char   **items;
    uint32_t count;
    uint32_t cap;
};

static void cand_add(struct Candidates *c, const char *s) {
    for (uint32_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i], s) == 0)
            return;
    if (c->count == c->cap) {
        uint32_t cap = c->cap == 0 ? 16 : c->cap * 2;
        char **bigger = realloc(c->items, cap * sizeof(char*));
        if (bigger == NULL)
            return;
        c->items = bigger;
        c->cap   = cap;
    }
    c->items[c->count++] = strdup(s);
}

static void cand_free(struct Candidates *c) {
    for (uint32_t i = 0; i < c->count; i++)
        free(c->items[i]);
    free(c->items);
}

static void sort_strings(char **items, uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        char *x = items[i];
        uint32_t j = i;
        while (j > 0 && strcmp(items[j - 1], x) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = x;
    }
}

static void print_columns(char **items, uint32_t count) {
    uint32_t width = 1;
    for (uint32_t i = 0; i < count; i++)
        if (strlen(items[i]) + 2 > width)
            width = (uint32_t) strlen(items[i]) + 2;
    uint32_t per_row = screen_cols / width;
    if (per_row == 0)
        per_row = 1;
    for (uint32_t i = 0; i < count; i++) {
        out_printf("%-*s", (int) width, items[i]);
        if ((i + 1) % per_row == 0 || i + 1 == count)
            out_putc('\n');
    }
    out_flush();
}

static bool is_separator_char(char c) {
    return c == ' ' || c == '|' || c == ';' || c == '&' || c == '<' || c == '>';
}

static void complete(void) {
    uint32_t start = ed.cursor;
    while (start > 0 && !is_separator_char(ed.buf[start - 1]))
        start--;
    // Is this the command word? (only separators / spaces before it, back to | ; && or line start)
    bool command_word = true;
    for (uint32_t i = start; i > 0; i--) {
        char c = ed.buf[i - 1];
        if (c == ' ')
            continue;
        command_word = c == '|' || c == ';' || c == '&';
        break;
    }
    char word[SHELL_PATH_MAX];
    uint32_t wlen = ed.cursor - start;
    if (wlen >= sizeof(word))
        return;
    memcpy(word, ed.buf + start, wlen);
    word[wlen] = '\0';

    struct Candidates cands = {0};
    char *slash = strrchr(word, '/');
    if (command_word && slash == NULL) {
        for (uint32_t i = 0; i < shell_command_count; i++)
            if (strncmp(shell_commands[i].name, word, wlen) == 0)
                cand_add(&cands, shell_commands[i].name);
    }
    // File names: in the directory part of the word (or the working directory). Every
    // candidate is a bare name, so the typed part to complete is what follows the last '/'
    char dir_part[SHELL_PATH_MAX] = "";
    const char *file_prefix = word;
    if (slash != NULL) {
        uint32_t dlen = (uint32_t) (slash - word) + 1;
        memcpy(dir_part, word, dlen);
        dir_part[dlen] = '\0';
        file_prefix = slash + 1;
    }
    uint32_t prefix_len = (uint32_t) strlen(file_prefix);
    struct Resolved dir;
    if (resolve_path(dir_part[0] ? dir_part : ".", &dir, NULL, true) && dir.exists && dir.type == SYSCALL_TYPE_DIR) {
        struct DirEntry *entries;
        uint32_t n;
        if (fs_list(dir.inode, &entries, &n) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (strncmp(entries[i].name, file_prefix, strlen(file_prefix)) != 0)
                    continue;
                // as a command, only scripts and folders make sense
                bool script = entries[i].name_len > 3 && strcmp(entries[i].name + entries[i].name_len - 3, ".sh") == 0;
                if (command_word && slash == NULL && entries[i].type != SYSCALL_TYPE_DIR && !script)
                    continue;
                char item[SHELL_NAME_MAX + 2];
                snprintf(item, sizeof(item), "%s%s", entries[i].name, entries[i].type == SYSCALL_TYPE_DIR ? "/" : "");
                cand_add(&cands, item);
            }
            free(entries);
        }
    }

    if (cands.count == 0) {
        beep(220, 30);
    } else if (cands.count == 1) {
        const char *c = cands.items[0];
        uint32_t clen = (uint32_t) strlen(c);
        ed_insert(c + prefix_len, clen - prefix_len);
        if (clen > 0 && c[clen - 1] != '/')
            ed_insert(" ", 1);
    } else {
        // Extend to the longest common prefix, or list the choices
        uint32_t common = (uint32_t) strlen(cands.items[0]);
        for (uint32_t i = 1; i < cands.count; i++) {
            uint32_t k = 0;
            while (k < common && cands.items[i][k] == cands.items[0][k])
                k++;
            common = k;
        }
        if (common > prefix_len) {
            ed_insert(cands.items[0] + prefix_len, common - prefix_len);
        } else {
            sort_strings(cands.items, cands.count);
            ed_place(ed.len);
            out_putc('\n');
            out_color(C_LIGHT_CYAN);
            print_columns(cands.items, cands.count);
            out_color(C_LIGHT_GRAY);
            ed_reprint();
        }
    }
    cand_free(&cands);
}

/**
 * Read one command line with editing keys, history (Up / Down), tab completion and console
 * scrolling (Page Up / Page Down, mouse wheel in the kernel).
 * @return false when the line was cancelled with Ctrl+C
 */
static bool read_line(void) {
    ed_begin();
    uint32_t browse = history_count;
    char *pending = NULL; // the line being typed while browsing the history
    for (;;) {
        char c = getchar_wait();
        if (c == '\n') {
            ed_place(ed.len);
            out_putc('\n');
            out_flush();
            free(pending);
            return true;
        }
        switch (c) {
            case '\b':
                if (ed.cursor > 0)
                    ed_delete(ed.cursor - 1, 1);
                break;
            case KEY_DEL:
                ed_delete(ed.cursor, 1);
                break;
            case KEY_LEFT:
                if (ed.cursor > 0)
                    ed_place(--ed.cursor);
                break;
            case KEY_RIGHT:
                if (ed.cursor < ed.len)
                    ed_place(++ed.cursor);
                break;
            case KEY_HOME:
            case KEY_CTRL('a'):
                ed.cursor = 0;
                ed_place(0);
                break;
            case KEY_END:
            case KEY_CTRL('e'):
                ed.cursor = ed.len;
                ed_place(ed.len);
                break;
            case KEY_UP:
            case KEY_DOWN:
                if (c == KEY_UP && browse > 0) {
                    if (browse == history_count) {
                        free(pending);
                        pending = strndup(ed.buf, ed.len);
                    }
                    ed_set_text(history[--browse]);
                } else if (c == KEY_DOWN && browse < history_count) {
                    browse++;
                    ed_set_text(browse == history_count ? (pending ? pending : "") : history[browse]);
                }
                break;
            case KEY_PGUP:
                console_scroll(-(int32_t) (screen_rows - 2));
                break;
            case KEY_PGDN:
                if (console_scroll((int32_t) (screen_rows - 2)) == 0)
                    ed_place(ed.cursor);
                break;
            case '\t':
                complete();
                break;
            case KEY_CTRL('c'):
                ed_place(ed.len);
                note_printf(C_DARK_GRAY, "^C\n");
                free(pending);
                ed.len = 0;
                ed.buf[0] = '\0';
                return false;
            case KEY_CTRL('l'):
                console_clear_screen();
                ed_reprint();
                break;
            case KEY_CTRL('u'):
                ed_delete(0, ed.cursor);
                break;
            case KEY_CTRL('k'):
                ed_delete(ed.cursor, ed.len - ed.cursor);
                break;
            case KEY_CTRL('w'): {
                uint32_t start = ed.cursor;
                while (start > 0 && ed.buf[start - 1] == ' ')
                    start--;
                while (start > 0 && ed.buf[start - 1] != ' ')
                    start--;
                ed_delete(start, ed.cursor - start);
                break;
            }
            default:
                if ((unsigned char) c >= 0x20 && (unsigned char) c < 0x7F)
                    ed_insert(&c, 1);
                break;
        }
    }
}

/* ================================ PARSER =========================================== */

enum TokenType { T_WORD, T_PIPE, T_OUT, T_APPEND, T_IN, T_SEMI, T_AND, T_OR };

struct Token {
    enum TokenType type;
    char          *text;   // T_WORD (heap); glob characters outside quotes are marked
};

struct TokenList {
    struct Token *items;
    uint32_t      count;
    uint32_t      cap;
};

#define GLOB_STAR  '\x01'
#define GLOB_QMARK '\x02'

struct StrBuf {
    char    *data;
    uint32_t len;
    uint32_t cap;
};

static void sb_add(struct StrBuf *b, const char *s, uint32_t n) {
    if (b->len + n + 1 > b->cap) {
        uint32_t cap = b->cap < 64 ? 64 : b->cap;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *bigger = realloc(b->data, cap);
        if (bigger == NULL)
            return;
        b->data = bigger;
        b->cap  = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_addc(struct StrBuf *b, char c) {
    sb_add(b, &c, 1);
}

static void tokens_push(struct TokenList *l, enum TokenType type, char *text) {
    if (l->count == l->cap) {
        uint32_t cap = l->cap == 0 ? 16 : l->cap * 2;
        struct Token *bigger = realloc(l->items, cap * sizeof(*bigger));
        if (bigger == NULL) {
            free(text);
            return;
        }
        l->items = bigger;
        l->cap   = cap;
    }
    l->items[l->count].type = type;
    l->items[l->count].text = text;
    l->count++;
}

static void tokens_free(struct TokenList *l) {
    for (uint32_t i = 0; i < l->count; i++)
        free(l->items[i].text);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

// $NAME, ${NAME}, $?, $#, $0-$9; p points after '$'. Returns characters consumed
static uint32_t expand_variable(const char *p, struct StrBuf *b) {
    char num[16];
    if (*p == '?') {
        snprintf(num, sizeof(num), "%d", last_status);
        sb_add(b, num, (uint32_t) strlen(num));
        return 1;
    }
    if (*p == '#') {
        snprintf(num, sizeof(num), "%d", script_argc > 0 ? script_argc - 1 : 0);
        sb_add(b, num, (uint32_t) strlen(num));
        return 1;
    }
    if (is_digit(*p)) {
        int index = *p - '0';
        if (index < script_argc)
            sb_add(b, script_argv[index], (uint32_t) strlen(script_argv[index]));
        return 1;
    }
    bool braces = *p == '{';
    const char *start = p + (braces ? 1 : 0);
    if (!is_var_start(*start)) {
        sb_addc(b, '$');
        return 0;
    }
    uint32_t n = 0;
    while (is_var_char(start[n]) && n < VAR_NAME_MAX)
        n++;
    char name[VAR_NAME_MAX + 1];
    memcpy(name, start, n);
    name[n] = '\0';
    uint32_t used = n + (braces ? 1 : 0);
    if (braces) {
        if (start[n] != '}') {
            sb_addc(b, '$');
            return 0;
        }
        used++;
    }
    const char *value = shell_get_var(name);
    if (value != NULL)
        sb_add(b, value, (uint32_t) strlen(value));
    return used;
}

static bool tokenize(const char *line, struct TokenList *tokens) {
    const char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p == '\0' || *p == '#')
            return true;
        if (p[0] == '|' && p[1] == '|') { tokens_push(tokens, T_OR, NULL); p += 2; continue; }
        if (p[0] == '&' && p[1] == '&') { tokens_push(tokens, T_AND, NULL); p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { tokens_push(tokens, T_APPEND, NULL); p += 2; continue; }
        if (*p == '|') { tokens_push(tokens, T_PIPE, NULL); p++; continue; }
        if (*p == '>') { tokens_push(tokens, T_OUT, NULL); p++; continue; }
        if (*p == '<') { tokens_push(tokens, T_IN, NULL); p++; continue; }
        if (*p == ';') { tokens_push(tokens, T_SEMI, NULL); p++; continue; }

        struct StrBuf word = {0};
        sb_add(&word, "", 0);
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '|' && *p != ';' &&
               *p != '<' && *p != '>' && !(p[0] == '&' && p[1] == '&')) {
            if (*p == '\'') {
                const char *end = strchr(p + 1, '\'');
                if (end == NULL) {
                    err_printf("syntax error: missing '\n");
                    free(word.data);
                    return false;
                }
                sb_add(&word, p + 1, (uint32_t) (end - p - 1));
                p = end + 1;
            } else if (*p == '"') {
                p++;
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        sb_addc(&word, p[1]);
                        p += 2;
                    } else if (*p == '$') {
                        p += 1 + expand_variable(p + 1, &word);
                    } else {
                        sb_addc(&word, *p++);
                    }
                }
                if (*p != '"') {
                    err_printf("syntax error: missing \"\n");
                    free(word.data);
                    return false;
                }
                p++;
            } else if (*p == '\\' && p[1] != '\0') {
                sb_addc(&word, p[1]);
                p += 2;
            } else if (*p == '$') {
                p += 1 + expand_variable(p + 1, &word);
            } else if (*p == '*') {
                sb_addc(&word, GLOB_STAR);
                p++;
            } else if (*p == '?') {
                sb_addc(&word, GLOB_QMARK);
                p++;
            } else {
                sb_addc(&word, *p++);
            }
        }
        tokens_push(tokens, T_WORD, word.data);
    }
}

/* -- Globbing: * and ? in the last path component of a word -- */

bool glob_match(const char *pattern, const char *name) {
    // Iterative wildcard match with backtracking to the last '*'
    const char *star = NULL, *resume = NULL;
    while (*name != '\0') {
        char pc = *pattern;
        if (pc == GLOB_STAR || pc == '*') {
            star   = pattern++;
            resume = name;
        } else if (pc == GLOB_QMARK || pc == '?' || pc == *name) {
            pattern++;
            name++;
        } else if (star != NULL) {
            pattern = star + 1;
            name    = ++resume;
        } else {
            return false;
        }
    }
    while (*pattern == GLOB_STAR || *pattern == '*')
        pattern++;
    return *pattern == '\0';
}

static bool has_glob(const char *s) {
    return strchr(s, GLOB_STAR) != NULL || strchr(s, GLOB_QMARK) != NULL;
}

static void unmark(char *s) {
    for (; *s != '\0'; s++) {
        if (*s == GLOB_STAR)
            *s = '*';
        else if (*s == GLOB_QMARK)
            *s = '?';
    }
}

struct ArgList {
    char   **argv;
    uint32_t argc;
    uint32_t cap;
};

static void args_push(struct ArgList *a, char *s) {
    if (a->argc + 1 >= a->cap) {
        uint32_t cap = a->cap == 0 ? 8 : a->cap * 2;
        char **bigger = realloc(a->argv, cap * sizeof(char*));
        if (bigger == NULL) {
            free(s);
            return;
        }
        a->argv = bigger;
        a->cap  = cap;
    }
    a->argv[a->argc++] = s;
    a->argv[a->argc]   = NULL;
}

static void args_free(struct ArgList *a) {
    for (uint32_t i = 0; i < a->argc; i++)
        free(a->argv[i]);
    free(a->argv);
    a->argv = NULL;
    a->argc = a->cap = 0;
}

static void expand_word(const char *word, struct ArgList *args) {
    if (!has_glob(word)) {
        args_push(args, strdup(word));
        return;
    }
    const char *slash = strrchr(word, '/');
    char dir[SHELL_PATH_MAX] = "";
    const char *pattern = word;
    if (slash != NULL) {
        uint32_t n = (uint32_t) (slash - word) + 1;
        if (n >= sizeof(dir))
            n = sizeof(dir) - 1;
        memcpy(dir, word, n);
        dir[n] = '\0';
        pattern = slash + 1;
    }
    uint32_t before = args->argc;
    struct Resolved r;
    char lookup[SHELL_PATH_MAX];
    strncpy(lookup, dir[0] ? dir : ".", sizeof(lookup) - 1);
    lookup[sizeof(lookup) - 1] = '\0';
    unmark(lookup);
    if (!has_glob(dir) && resolve_path(lookup, &r, NULL, true) && r.exists && r.type == SYSCALL_TYPE_DIR) {
        struct DirEntry *entries;
        uint32_t n;
        if (fs_list(r.inode, &entries, &n) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                // like sh: * does not match names starting with '.'
                if (entries[i].name[0] == '.' && pattern[0] != '.')
                    continue;
                if (glob_match(pattern, entries[i].name)) {
                    char path[SHELL_PATH_MAX + SHELL_NAME_MAX + 2];
                    snprintf(path, sizeof(path), "%s%s", dir, entries[i].name);
                    unmark(path);
                    args_push(args, strdup(path));
                }
            }
            free(entries);
        }
    }
    if (args->argc == before) { // no match: keep the word as typed
        char *literal = strdup(word);
        unmark(literal);
        args_push(args, literal);
    } else {
        sort_strings(args->argv + before, args->argc - before);
    }
}

/* ================================ EXECUTION ======================================== */

static bool exit_requested = false;
static int  exit_code      = 0;
static int  script_depth   = 0;

void shell_request_exit(int code) {
    exit_requested = true;
    exit_code      = code;
}

static const struct Command *find_command(const char *name) {
    for (uint32_t i = 0; i < shell_command_count; i++)
        if (strcmp(shell_commands[i].name, name) == 0)
            return &shell_commands[i];
    return NULL;
}

static bool is_assignment(const char *word, uint32_t *name_len) {
    if (!is_var_start(word[0]))
        return false;
    uint32_t n = 0;
    while (is_var_char(word[n]))
        n++;
    if (word[n] != '=' || n > VAR_NAME_MAX)
        return false;
    *name_len = n;
    return true;
}

static int run_command(int argc, char **argv, struct Input *in) {
    if (argc == 0)
        return 0;
    uint32_t name_len;
    if (argc == 1 && is_assignment(argv[0], &name_len)) {
        argv[0][name_len] = '\0';
        shell_set_var(argv[0], argv[0] + name_len + 1);
        return 0;
    }
    const struct Command *cmd = find_command(argv[0]);
    if (cmd != NULL)
        return cmd->run(argc, argv, in);
    // ./script.sh, dir/script.sh or script.sh: run it as a shell script
    uint32_t len = (uint32_t) strlen(argv[0]);
    if (strchr(argv[0], '/') != NULL || (len > 3 && strcmp(argv[0] + len - 3, ".sh") == 0)) {
        struct Resolved r;
        if (resolve_path(argv[0], &r, NULL, true) && r.exists && r.type == SYSCALL_TYPE_FILE)
            return shell_run_script(argv[0], argc, argv);
        if (r.exists && r.type == SYSCALL_TYPE_DIR) {
            err_printf("%s: is a directory\n", argv[0]);
            return 126;
        }
    }
    err_printf("%s: command not found (try `help`)\n", argv[0]);
    return 127;
}

struct Stage {
    struct ArgList args;
    char          *in_file;
    char          *out_file;
    bool           append;
};

static void stage_free(struct Stage *s) {
    args_free(&s->args);
    free(s->in_file);
    free(s->out_file);
}

// tokens[from, to) hold one pipeline: stage (| stage)*
static int run_pipeline(struct Token *tokens, uint32_t from, uint32_t to) {
    // Split into stages
    uint32_t stage_count = 1;
    for (uint32_t i = from; i < to; i++)
        if (tokens[i].type == T_PIPE)
            stage_count++;
    struct Stage *stages = calloc(stage_count, sizeof(struct Stage));
    if (stages == NULL)
        return 1;
    uint32_t s = 0;
    int status = 0;
    bool ok = true;
    for (uint32_t i = from; i < to && ok; i++) {
        struct Token *t = &tokens[i];
        if (t->type == T_PIPE) {
            if (stages[s].args.argc == 0) {
                err_printf("syntax error near `|'\n");
                ok = false;
            }
            s++;
        } else if (t->type == T_OUT || t->type == T_APPEND || t->type == T_IN) {
            if (i + 1 >= to || tokens[i + 1].type != T_WORD) {
                err_printf("syntax error: redirection without a file name\n");
                ok = false;
                break;
            }
            char *file = strdup(tokens[++i].text);
            unmark(file);
            if (t->type == T_IN) {
                free(stages[s].in_file);
                stages[s].in_file = file;
            } else {
                free(stages[s].out_file);
                stages[s].out_file = file;
                stages[s].append   = t->type == T_APPEND;
            }
        } else if (t->type == T_WORD) {
            expand_word(t->text, &stages[s].args);
        }
    }
    if (ok && stages[stage_count - 1].args.argc == 0 && stage_count > 1) {
        err_printf("syntax error: pipeline ends with `|'\n");
        ok = false;
    }

    // Run: every stage but the last writes into a capture buffer that becomes the next input
    struct Input input = {0};
    char *input_owned = NULL;
    for (uint32_t i = 0; i < stage_count && ok; i++) {
        struct Stage *st = &stages[i];
        if (st->in_file != NULL) {
            char *data;
            uint32_t len;
            if (!read_whole_file(st->in_file, &data, &len, "shell")) {
                status = 1;
                break;
            }
            free(input_owned);
            input_owned   = data;
            input.present = true;
            input.data    = data;
            input.len     = len;
        }
        bool last = i + 1 == stage_count;
        struct Output capture = {.console = false};
        out = (!last || st->out_file != NULL) ? &capture : &console_out;
        status = run_command((int) st->args.argc, st->args.argv, &input);
        out_flush();
        out_color(C_LIGHT_GRAY);
        out = &console_out;

        free(input_owned);
        input_owned = NULL;
        memset(&input, 0, sizeof(input));
        if (st->out_file != NULL) {
            if (!write_whole_file(st->out_file, capture.data ? capture.data : "", capture.len, st->append, "shell"))
                status = 1;
            free(capture.data);
            input.present = !last; // a redirected stage sends nothing down the pipe
            input.data    = "";
            input.len     = 0;
        } else if (!last) {
            input_owned   = capture.data;
            input.present = true;
            input.data    = capture.data ? capture.data : "";
            input.len     = capture.len;
        }
        if (exit_requested)
            break;
    }
    free(input_owned);
    for (uint32_t i = 0; i < stage_count; i++)
        stage_free(&stages[i]);
    free(stages);
    return ok ? status : 2;
}

/**
 * Find the next list operator (; && ||) outside quotes. Variables are expanded only when a
 * pipeline is about to run, so `false || echo $?` sees the status of `false` (like sh).
 */
static void scan_segment(const char *p, const char **end, enum TokenType *op, const char **next, bool *at_end) {
    char quote = 0;
    bool word_start = true;
    for (; *p != '\0'; p++) {
        char c = *p;
        if (quote) {
            if (c == quote)
                quote = 0;
            else if (c == '\\' && quote == '"' && p[1] != '\0')
                p++;
            continue;
        }
        if (c == '\\' && p[1] != '\0') {
            p++;
            word_start = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            word_start = false;
            continue;
        }
        if (c == '#' && word_start)
            break; // comment: the rest of the line is ignored
        bool and = c == '&' && p[1] == '&', or = c == '|' && p[1] == '|';
        if (c == ';' || and || or) {
            *end    = p;
            *op     = c == ';' ? T_SEMI : (and ? T_AND : T_OR);
            *next   = p + (c == ';' ? 1 : 2);
            *at_end = false;
            return;
        }
        word_start = c == ' ' || c == '\t' || c == '|' || c == '<' || c == '>';
    }
    *end    = p;
    *op     = T_SEMI;
    *next   = p;
    *at_end = true;
}

static bool is_blank(const char *s) {
    for (; *s != '\0'; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r')
            return false;
    return true;
}

// pipeline ((; | && | ||) pipeline)*
static int run_line(const char *line) {
    int status = last_status;
    enum TokenType pending = T_SEMI;
    const char *p = line;
    for (;;) {
        const char *end, *next;
        enum TokenType op;
        bool at_end;
        scan_segment(p, &end, &op, &next, &at_end);
        char *segment = strndup(p, (size_t) (end - p));
        if (segment == NULL)
            return 1;
        if (!is_blank(segment)) {
            bool run = pending == T_SEMI || (pending == T_AND && status == 0) || (pending == T_OR && status != 0);
            if (run) {
                struct TokenList tokens = {0};
                if (!tokenize(segment, &tokens))
                    status = 2;
                else if (tokens.count > 0)
                    status = run_pipeline(tokens.items, 0, tokens.count);
                tokens_free(&tokens);
                last_status = status;
            }
        } else if (pending != T_SEMI || (op != T_SEMI && !at_end)) {
            if (at_end)
                err_printf("syntax error: unexpected end of line\n");
            else
                err_printf("syntax error near `%s'\n", op == T_AND ? "&&" : (op == T_OR ? "||" : ";"));
            free(segment);
            last_status = 2;
            return 2;
        }
        free(segment);
        if (at_end || exit_requested)
            break;
        pending = op;
        p = next;
    }
    return status;
}

/* ================================ SCRIPTS (bonus) ================================== */

int shell_run_script(const char *path, int argc, char **argv) {
    if (script_depth >= SCRIPT_DEPTH_MAX) {
        err_printf("%s: scripts nested too deep\n", path);
        return 1;
    }
    char *data;
    uint32_t len;
    if (!read_whole_file(path, &data, &len, "sh"))
        return 1;

    int    saved_argc = script_argc;
    char **saved_argv = script_argv;
    script_argc = argc;
    script_argv = argv;
    script_depth++;

    int status = 0;
    char *line = data;
    while (line < data + len && !exit_requested) {
        char *nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';
        status = run_line(line);
        last_status = status;
        if (nl == NULL)
            break;
        line = nl + 1;
    }
    if (exit_requested) { // `exit` inside a script ends the script only
        status = exit_code;
        exit_requested = false;
    }

    script_depth--;
    script_argc = saved_argc;
    script_argv = saved_argv;
    free(data);
    return status;
}

/* ================================ MAIN (REPL) ====================================== */

static void banner(void) {
    note_printf(C_LIGHT_CYAN, "      ___  ____  __  __  ___  ____\n");
    note_printf(C_LIGHT_CYAN, "  ___/ _ \\/ ___||  \\/  |/ _ \\/ ___|\n");
    note_printf(C_LIGHT_BLUE, " / __| | | \\___ \\| |\\/| | | | \\___ \\\n");
    note_printf(C_LIGHT_BLUE, "| (__| |_| |___) | |  | | |_| |___) |\n");
    note_printf(C_LIGHT_MAGENTA, " \\___|\\___/|____/|_|  |_|\\___/|____/\n\n");
    note_printf(C_WHITE, "cOSmOS shell - running in user mode (ring 3) on the %s root file system.\n", fs_label);
    note_printf(C_DARK_GRAY, "`help` lists the commands. Tab completes, Up/Down recalls, PgUp/PgDn or the wheel scrolls.\n\n");
}

int main(void) {
    syscall(SYS_KEYBOARD_ACTIVATE, 0, 0, 0);
    struct SyscallScreenInfo screen;
    console_screen_info(&screen);
    screen_cols = screen.cols;
    screen_rows = screen.rows;

    struct SyscallSysInfo info;
    get_sysinfo(&info);
    strncpy(fs_label, info.fs_name[0] ? info.fs_name : "fs", sizeof(fs_label) - 1);

    banner();
    shell_update_status();
    for (;;) {
        print_prompt();
        bool entered = read_line();
        if (!entered)
            continue;
        // !! and !n recall a previous command
        char *line = strdup(ed.buf);
        if (line == NULL)
            continue;
        if (line[0] == '!' && history_count > 0) {
            const char *recalled = NULL;
            uint32_t n;
            if (strcmp(line, "!!") == 0)
                recalled = history[history_count - 1];
            else if (parse_uint(line + 1, &n) && n > history_total - history_count && n <= history_total)
                recalled = history[n - (history_total - history_count) - 1];
            if (recalled != NULL) {
                free(line);
                line = strdup(recalled);
                note_printf(C_DARK_GRAY, "%s\n", line);
            }
        }
        history_add(line);
        last_status = run_line(line);
        free(line);
        if (exit_requested)
            exit(exit_code);
        shell_update_status();
    }
}
