#include "user/shell.h"

#define HISTORY_MAX      500
#define SCRIPT_DEPTH_MAX 8
#define VAR_NAME_MAX     32

static char fs_label[16] = "fs";

/* ================================ VARIABLES ======================================== */

struct Var {
    char  name[VAR_NAME_MAX + 1];
    char *value;
    bool  exported; // passed to programs in envp (export NAME=value)
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

static void path_array_rebuild(const char *value);

void shell_set_var(const char *name, const char *value) {
    if (strcmp(name, "PATH") == 0)
        path_array_rebuild(value);
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
    vars[var_count].value    = strdup(value);
    vars[var_count].exported = false;
    var_count++;
}

static void environment_save(void);

void shell_unset_var(const char *name) {
    if (strcmp(name, "PATH") == 0)
        path_array_rebuild("");
    for (uint32_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            bool was_exported = vars[i].exported;
            free(vars[i].value);
            vars[i] = vars[--var_count];
            if (was_exported)
                environment_save();
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

/* ================================ ENVIRONMENT (bonus: PATH) ====================== */

#define ENVIRONMENT_FILE "/etc/environment"
#define DEFAULT_PATH     "/bin:/apps"

/**
 * PATH as a dynamic array (bonus: PATH array dinamis): every directory is a heap string,
 * the array doubles its capacity with realloc() when it is full. `path` dumps it.
 */
static struct {
    char   **dirs;
    uint32_t count;
    uint32_t capacity;
    uint32_t grows;
} path_array = {NULL, 0, 0, 0};

static void path_array_clear(void) {
    for (uint32_t i = 0; i < path_array.count; i++)
        free(path_array.dirs[i]);
    path_array.count = 0;
}

static bool path_array_push(const char *dir, uint32_t len) {
    if (len == 0)
        return true;
    if (path_array.count == path_array.capacity) {
        uint32_t cap = path_array.capacity == 0 ? 2 : path_array.capacity * 2;
        char **bigger = realloc(path_array.dirs, cap * sizeof(char*));
        if (bigger == NULL)
            return false;
        path_array.dirs     = bigger;
        path_array.capacity = cap;
        path_array.grows++;
    }
    path_array.dirs[path_array.count++] = strndup(dir, len);
    return true;
}

static void path_array_rebuild(const char *value) {
    path_array_clear();
    const char *p = value;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        uint32_t len = colon ? (uint32_t) (colon - p) : (uint32_t) strlen(p);
        path_array_push(p, len);
        p += len;
        if (*p == ':')
            p++;
    }
}

static char *path_array_join(void) {
    uint32_t total = 1;
    for (uint32_t i = 0; i < path_array.count; i++)
        total += (uint32_t) strlen(path_array.dirs[i]) + 1;
    char *joined = malloc(total);
    if (joined == NULL)
        return NULL;
    joined[0] = '\0';
    for (uint32_t i = 0; i < path_array.count; i++) {
        if (i > 0)
            strcat_s(joined, total, ":");
        strcat_s(joined, total, path_array.dirs[i]);
    }
    return joined;
}

static struct Var *find_var(const char *name) {
    for (uint32_t i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    return NULL;
}

static bool environment_loading = false;

// Exported variables survive a reboot in /etc/environment (book 4.3.5: persistent PATH)
static void environment_save(void) {
    if (environment_loading)
        return;
    struct StrBufLite { char *data; uint32_t len, cap; } b = {NULL, 0, 0};
    for (uint32_t i = 0; i < var_count; i++) {
        if (!vars[i].exported || strcmp(vars[i].name, "PWD") == 0)
            continue;
        uint32_t need = (uint32_t) (strlen(vars[i].name) + strlen(vars[i].value) + 3);
        if (b.len + need > b.cap) {
            uint32_t cap = b.cap ? b.cap * 2 : 128;
            while (cap < b.len + need)
                cap *= 2;
            char *bigger = realloc(b.data, cap);
            if (bigger == NULL)
                break;
            b.data = bigger;
            b.cap  = cap;
        }
        b.len += (uint32_t) snprintf(b.data + b.len, b.cap - b.len, "%s=%s\n", vars[i].name, vars[i].value);
    }
    struct SyscallStat st;
    if (fs_stat(FS_ROOT, "etc", 3, &st) != 0)
        fs_mkdir(FS_ROOT, "etc", 3);
    write_whole_file(ENVIRONMENT_FILE, b.data ? b.data : "", b.len, false, "export");
    free(b.data);
}

void shell_export_var(const char *name, const char *value) {
    if (value != NULL)
        shell_set_var(name, value);
    struct Var *v = find_var(name);
    if (v == NULL) {
        shell_set_var(name, "");
        v = find_var(name);
    }
    if (v != NULL)
        v->exported = true;
    environment_save();
}

static void environment_load(void) {
    char *data;
    uint32_t len;
    struct Resolved r;
    environment_loading = true;
    bool have_file = resolve_path(ENVIRONMENT_FILE, &r, NULL, true) && r.exists && r.type == SYSCALL_TYPE_FILE;
    if (have_file && read_whole_file(ENVIRONMENT_FILE, &data, &len, "shell")) {
        char *line = data;
        while (line < data + len) {
            char *nl = strchr(line, '\n');
            if (nl != NULL)
                *nl = '\0';
            char *eq = strchr(line, '=');
            if (eq != NULL && eq != line && eq - line <= VAR_NAME_MAX) {
                *eq = '\0';
                shell_export_var(line, eq + 1);
            }
            if (nl == NULL)
                break;
            line = nl + 1;
        }
        free(data);
    }
    environment_loading = false;
    if (shell_get_var("PATH") == NULL) {
        shell_export_var("PATH", DEFAULT_PATH); // first boot: create /etc/environment
    }
}

// envp for a new program: NAME=value of every exported variable (+ PWD), heap strings
static char **environment_build(int *count) {
    char **envp = calloc(var_count + 2, sizeof(char*));
    int n = 0;
    if (envp == NULL) {
        *count = 0;
        return NULL;
    }
    for (uint32_t i = 0; i < var_count && n < 30; i++) {
        if (!vars[i].exported)
            continue;
        uint32_t len = (uint32_t) (strlen(vars[i].name) + strlen(vars[i].value) + 2);
        envp[n] = malloc(len);
        if (envp[n] != NULL)
            snprintf(envp[n++], len, "%s=%s", vars[i].name, vars[i].value);
    }
    char pwd[SHELL_PATH_MAX];
    path_format(&cwd, pwd, sizeof(pwd));
    uint32_t len = (uint32_t) strlen(pwd) + 5;
    envp[n] = malloc(len);
    if (envp[n] != NULL)
        snprintf(envp[n++], len, "PWD=%s", pwd);
    *count = n;
    return envp;
}

static void environment_free(char **envp, int count) {
    for (int i = 0; i < count; i++)
        free(envp[i]);
    free(envp);
}

const char *shell_path_value(void) {
    return shell_get_var("PATH");
}

bool shell_find_program(const char *name, struct Resolved *r) {
    if (strchr(name, '/') != NULL)
        return resolve_path(name, r, NULL, true) && r->exists && r->type == SYSCALL_TYPE_FILE;
    char path[SHELL_PATH_MAX];
    for (uint32_t i = 0; i < path_array.count; i++) {
        snprintf(path, sizeof(path), "%s/%s", path_array.dirs[i], name);
        if (resolve_path(path, r, NULL, true) && r->exists && r->type == SYSCALL_TYPE_FILE)
            return true;
    }
    return false;
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
        // Programs in the PATH directories (hello, clock, snake, ...)
        for (uint32_t d = 0; d < path_array.count; d++) {
            struct Resolved r;
            if (!resolve_path(path_array.dirs[d], &r, NULL, true) || !r.exists || r.type != SYSCALL_TYPE_DIR)
                continue;
            struct DirEntry *entries;
            uint32_t count;
            if (fs_list(r.inode, &entries, &count) != 0)
                continue;
            for (uint32_t i = 0; i < count; i++)
                if (entries[i].type == SYSCALL_TYPE_FILE && strncmp(entries[i].name, word, wlen) == 0)
                    cand_add(&cands, entries[i].name);
            free(entries);
        }
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
// Did another process print while the line was being edited (background jobs)?
static bool ed_cursor_moved(void) {
    uint8_t row, col;
    console_get_cursor(&row, &col);
    uint32_t pos = ed.col0 + ed.cursor;
    int32_t expected = ed.row0 + (int32_t) (pos / screen_cols);
    if (expected < 0)
        expected = 0;
    return row != (uint8_t) expected || col != (uint8_t) (pos % screen_cols);
}

static bool read_line(void) {
    ed_begin();
    uint32_t browse = history_count;
    char *pending = NULL; // the line being typed while browsing the history
    for (;;) {
        char c = getchar_wait();
        if (ed_cursor_moved()) { // redisplay the prompt and the line below the foreign output
            uint8_t row, col;
            console_get_cursor(&row, &col);
            if (col != 0)
                out_putc('\n');
            out_flush();
            ed_reprint();
        }
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

enum TokenType { T_WORD, T_PIPE, T_OUT, T_APPEND, T_IN, T_SEMI, T_AND, T_OR, T_BG };

struct Token {
    enum TokenType type;
    char          *text;   // T_WORD (heap); glob characters outside quotes are marked
};

struct TokenList {
    struct Token *items;
    uint32_t      count;
    uint32_t      cap;
};


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
    if (*p == '$') {
        snprintf(num, sizeof(num), "%u", getpid());
        sb_add(b, num, (uint32_t) strlen(num));
        return 1;
    }
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

static bool exec_background = false; // `exec prog &`: the & reaches the exec built-in

static bool ends_with_sh(const char *name) {
    uint32_t len = (uint32_t) strlen(name);
    return len > 3 && strcmp(name + len - 3, ".sh") == 0;
}

/**
 * Resolution order:
 *   1. NAME=value assignment
 *   2. shell built-ins that need the shell's own state (cd, export, exit, history, ...)
 *   3. a program: "dir/prog" / "./prog" by path, a bare name through the PATH directories
 *      (book 4.3.5); CEX / ELF files run as separate processes, other files as scripts
 *   4. the built-in copy of a /bin command (only when /bin is missing or not in PATH)
 *   5. "x.sh" in the working directory
 */
static int run_command(int argc, char **argv, struct Input *in, bool background) {
    if (argc == 0)
        return 0;
    uint32_t name_len;
    if (argc == 1 && is_assignment(argv[0], &name_len)) {
        argv[0][name_len] = '\0';
        shell_set_var(argv[0], argv[0] + name_len + 1);
        if (find_var(argv[0])->exported)
            environment_save();
        return 0;
    }
    const struct Command *cmd = find_command(argv[0]);
    if (cmd != NULL && (cmd->flags & COMMAND_BUILTIN)) {
        exec_background = background;
        int status = cmd->run(argc, argv, in);
        exec_background = false;
        return status;
    }

    struct Resolved r;
    bool has_slash = strchr(argv[0], '/') != NULL;
    if (shell_find_program(argv[0], &r)) {
        if (ends_with_sh(r.name))
            return shell_run_script(argv[0], argc, argv);
        int status = shell_exec_program(argc, argv, in, background, false);
        if (status != -PROCESS_NOT_EXECUTABLE)
            return status;
        // No CEX / ELF header: a "#!" script runs in this shell, anything else is refused
        char *data = NULL;
        uint32_t len = 0;
        bool script = false;
        if (fs_read_file(r.parent, r.name, (uint8_t) strlen(r.name), &data, &len) == 0) {
            script = len >= 2 && data[0] == '#' && data[1] == '!';
            free(data);
        }
        if (script)
            return shell_run_script(argv[0], argc, argv);
        err_printf("%s: not an executable (no CEX / ELF header; `exec` runs flat binaries)\n", argv[0]);
        return 126;
    }
    if (has_slash) {
        if (resolve_path(argv[0], &r, NULL, true) && r.exists && r.type == SYSCALL_TYPE_DIR) {
            err_printf("%s: is a directory\n", argv[0]);
            return 126;
        }
    } else if (cmd != NULL) {
        return cmd->run(argc, argv, in); // built-in fallback of a /bin program
    } else if (ends_with_sh(argv[0]) && resolve_path(argv[0], &r, NULL, true) && r.exists &&
               r.type == SYSCALL_TYPE_FILE) {
        return shell_run_script(argv[0], argc, argv);
    }
    err_printf("%s: command not found (try `help`)\n", argv[0]);
    return 127;
}

/* ================================ PROGRAMS (chapter 4) ============================= */

/**
 * Run a program as a separate process. Pipeline input goes through a kernel pipe on its
 * stdin; when the shell is capturing (a later pipeline stage or `> file` follows), its
 * stdout is a pipe the shell drains, otherwise it writes to the console itself. The shell
 * blocks in SYS_WAIT until the program ends (Ctrl+C kills it), unless `background`.
 */
int shell_exec_program(int argc, char **argv, struct Input *in, bool background, bool allow_flat) {
    struct Resolved r;
    if (!shell_find_program(argv[0], &r)) {
        err_printf("%s: no such program (PATH=%s)\n", argv[0], shell_get_var("PATH") ? shell_get_var("PATH") : "");
        return 127;
    }
    int32_t in_fds[2]  = {-1, -1};
    int32_t out_fds[2] = {-1, -1};
    bool feed    = in != NULL && in->present;
    bool capture = !out->console;
    if ((feed && pipe_open(in_fds) != 0) || (capture && pipe_open(out_fds) != 0)) {
        err_printf("%s: cannot create a pipe\n", argv[0]);
        if (in_fds[0] >= 0) { fd_close(in_fds[0]); fd_close(in_fds[1]); }
        return 1;
    }
    if (feed) {
        fd_write_all(in_fds[1], in->data, in->len, 0);
        fd_close(in_fds[1]); // the program sees end of file after the data
    }
    int envc;
    char **envp = environment_build(&envc);
    out_flush();
    int32_t pid = process_spawn_ex(r.parent, r.name, (uint8_t) strlen(r.name), argc, argv, envc, envp,
                                   in_fds[0], capture ? out_fds[1] : -1, !background, !allow_flat);
    environment_free(envp, envc);
    if (in_fds[0] >= 0)
        fd_close(in_fds[0]);
    if (out_fds[1] >= 0)
        fd_close(out_fds[1]);
    if (pid < 0) {
        if (out_fds[0] >= 0)
            fd_close(out_fds[0]);
        if (pid == -PROCESS_NOT_EXECUTABLE && !allow_flat)
            return -PROCESS_NOT_EXECUTABLE;
        err_printf("%s: %s\n", argv[0], process_error_name(pid));
        return 126;
    }
    if (background) {
        note_printf(C_DARK_GRAY, "[%d] %s running in the background (`jobs`, `kill %d`)\n", pid, argv[0], pid);
        return 0;
    }
    if (capture) {
        uint32_t len;
        char *data = fd_read_all(out_fds[0], &len); // until the program closes stdout (exits)
        fd_close(out_fds[0]);
        if (data != NULL) {
            out_write(data, len);
            free(data);
        }
    }
    int32_t status = 0;
    process_wait((uint32_t) pid, &status, false);
    if (status == 137)
        note_printf(C_LIGHT_RED, "^C [%d] %s killed\n", pid, argv[0]);
    else if (status == 139)
        note_printf(C_LIGHT_RED, "[%d] %s crashed\n", pid, argv[0]);
    else if (status == -2) {
        note_printf(C_DARK_GRAY, "[%d] %s continues in the background\n", pid, argv[0]);
        status = 0;
    }
    cwd_refresh(); // the program may have renamed a directory of the working directory
    shell_update_status();
    return status;
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
static int run_pipeline(struct Token *tokens, uint32_t from, uint32_t to, bool background) {
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
        bool bg = background && stage_count == 1 && st->out_file == NULL && st->in_file == NULL;
        if (background && !bg && i == 0)
            err_printf("&: only a single program without redirection can run in the background\n");
        status = run_command((int) st->args.argc, st->args.argv, &input, bg);
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
        bool bg  = c == '&' && !and; // `prog &` runs the program in the background
        if (c == ';' || and || or || bg) {
            *end    = p;
            *op     = c == ';' ? T_SEMI : (and ? T_AND : (or ? T_OR : T_BG));
            *next   = p + ((c == ';' || bg) ? 1 : 2);
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
            bool run = pending == T_SEMI || pending == T_BG || (pending == T_AND && status == 0) ||
                       (pending == T_OR && status != 0);
            if (run) {
                struct TokenList tokens = {0};
                if (!tokenize(segment, &tokens))
                    status = 2;
                else if (tokens.count > 0)
                    status = run_pipeline(tokens.items, 0, tokens.count, op == T_BG);
                tokens_free(&tokens);
                last_status = status;
            }
        } else if ((pending != T_SEMI && pending != T_BG) || (op != T_SEMI && !at_end)) {
            if (at_end)
                err_printf("syntax error: unexpected end of line\n");
            else
                err_printf("syntax error near `%s'\n", op == T_AND ? "&&" : (op == T_OR ? "||" : (op == T_BG ? "&" : ";")));
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

/* ================================ BUILT-INS (chapter 4) ============================ */

// exec [-b] <program> [args...]: run a program from the file system (book 4.3.2)
int cmd_exec(int argc, char **argv, struct Input *in) {
    int first = 1;
    bool background = false;
    if (first < argc && strcmp(argv[first], "-b") == 0) {
        background = true;
        first++;
    }
    if (first >= argc) {
        err_printf("usage: exec [-b] <program> [args]...   (-b or a trailing & = background)\n");
        return 2;
    }
    struct Resolved r;
    if (!shell_find_program(argv[first], &r)) {
        err_printf("exec: %s: no such program\n", argv[first]);
        return 127;
    }
    return shell_exec_program(argc - first, argv + first, in, background || exec_background, true);
}

// export [NAME[=value]]...: environment variables of programs, saved in /etc/environment
int cmd_export(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc == 1) {
        for (uint32_t i = 0; i < var_count; i++)
            if (vars[i].exported) {
                out_printf("export %s=", vars[i].name);
                out_puts(vars[i].value);
                out_putc('\n');
            }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        uint32_t n = eq ? (uint32_t) (eq - argv[i]) : (uint32_t) strlen(argv[i]);
        char name[VAR_NAME_MAX + 1];
        if (n == 0 || n > VAR_NAME_MAX || !is_var_start(argv[i][0])) {
            err_printf("export: %s: not a valid name\n", argv[i]);
            status = 1;
            continue;
        }
        memcpy(name, argv[i], n);
        name[n] = '\0';
        shell_export_var(name, eq ? eq + 1 : NULL);
    }
    return status;
}

int cmd_env(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    int envc;
    char **envp = environment_build(&envc);
    for (int i = 0; i < envc; i++)
        out_printf("%s\n", envp[i]);
    environment_free(envp, envc);
    return 0;
}

// path [add <dir> | remove <dir>]: show / edit PATH through its dynamic array
int cmd_path(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc == 3 && (strcmp(argv[1], "add") == 0 || strcmp(argv[1], "remove") == 0)) {
        const char *dir = argv[2];
        if (dir[0] != '/') {
            err_printf("path: %s: use an absolute directory\n", dir);
            return 1;
        }
        if (strcmp(argv[1], "add") == 0) {
            path_array_push(dir, (uint32_t) strlen(dir));
        } else {
            bool found = false;
            for (uint32_t i = 0; i < path_array.count; i++) {
                if (strcmp(path_array.dirs[i], dir) == 0) {
                    free(path_array.dirs[i]);
                    memmove(&path_array.dirs[i], &path_array.dirs[i + 1], (path_array.count - i - 1) * sizeof(char*));
                    path_array.count--;
                    found = true;
                    break;
                }
            }
            if (!found) {
                err_printf("path: %s is not in PATH\n", dir);
                return 1;
            }
        }
        char *joined = path_array_join();
        if (joined != NULL) {
            shell_export_var("PATH", joined); // rebuilds the array from the new value, saves it
            free(joined);
        }
    } else if (argc != 1) {
        err_printf("usage: path [add <dir> | remove <dir>]\n");
        return 2;
    }
    out_printf("PATH=%s\n", shell_get_var("PATH") ? shell_get_var("PATH") : "");
    out_printf("dynamic array @0x%08x: count %u, capacity %u (grown %u times, saved in " ENVIRONMENT_FILE ")\n",
               (uint32_t) path_array.dirs, path_array.count, path_array.capacity, path_array.grows);
    for (uint32_t i = 0; i < path_array.capacity; i++) {
        if (i < path_array.count)
            out_printf("  [%u] @0x%08x -> 0x%08x \"%s\"\n", i, (uint32_t) &path_array.dirs[i],
                       (uint32_t) path_array.dirs[i], path_array.dirs[i]);
        else
            out_printf("  [%u] @0x%08x    (free)\n", i, (uint32_t) &path_array.dirs[i]);
    }
    return 0;
}

// jobs: programs started by this shell that still run (background ones, mostly)
int cmd_jobs(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallProcessInfo *list = calloc(32, sizeof(*list));
    if (list == NULL)
        return 1;
    uint32_t n = process_list(list, 32), me = getpid(), shown = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        if (list[i].ppid != me)
            continue;
        out_printf("[%u] %-8s %s\n", list[i].pid, list[i].state, list[i].cmdline);
        shown++;
    }
    if (shown == 0 && out_is_console())
        note_printf(C_DARK_GRAY, "no jobs\n");
    free(list);
    return 0;
}

// wait [pid]: block until a background program (or all of them) ends
int cmd_wait(int argc, char **argv, struct Input *in) {
    (void) in;
    uint32_t pid = 0;
    if (argc > 1 && !parse_uint(argv[1], &pid)) {
        err_printf("usage: wait [pid]\n");
        return 2;
    }
    int32_t status = 0, r;
    do {
        r = process_wait(pid, &status, false);
        if (r > 0 && out_is_console())
            note_printf(C_DARK_GRAY, "[%d] done, status %d\n", r, status);
    } while (r > 0 && pid == 0);
    return pid != 0 && r < 0 ? 127 : status;
}

// type <name>...: how the shell runs a name
int cmd_type(int argc, char **argv, struct Input *in) {
    (void) in;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const struct Command *cmd = find_command(argv[i]);
        struct Resolved r;
        if (cmd != NULL && (cmd->flags & COMMAND_BUILTIN)) {
            out_printf("%s is a shell built-in\n", argv[i]);
        } else if (shell_find_program(argv[i], &r)) {
            char where[SHELL_PATH_MAX];
            path_of_inode(r.parent, where, sizeof(where));
            out_printf("%s is %s%s%s (separate program)\n", argv[i], where, strcmp(where, "/") ? "/" : "", r.name);
        } else if (cmd != NULL) {
            out_printf("%s is a built-in fallback (no /bin/%s in PATH)\n", argv[i], argv[i]);
        } else {
            err_printf("type: %s: not found\n", argv[i]);
            status = 1;
        }
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
    note_printf(C_WHITE, "cOSmOS shell (pid %u, init) - user mode, %s root file system, multitasking.\n", getpid(), fs_label);
    note_printf(C_DARK_GRAY, "`help` lists the commands, `ls /bin /apps` the programs. `clock` starts the clock, `ps` lists processes.\n");
    note_printf(C_DARK_GRAY, "Tab completes, Up/Down recalls, PgUp/PgDn or the wheel scrolls, Ctrl+C stops a program.\n\n");
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    syscall(SYS_KEYBOARD_ACTIVATE, 0, 0, 0);
    shlib_init();
    cwd_sync_kernel();
    environment_load(); // PATH and the other exported variables from /etc/environment

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
