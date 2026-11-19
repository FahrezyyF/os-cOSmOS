#include "user/shell.h"

/**
 * Shared by the shell and the separate command programs in /bin: buffered output to
 * stdout (console with colors, a pipe, or a capture buffer), paths and the working directory.
 */

uint8_t screen_cols = 80;
uint8_t screen_rows = 24;

/* ================================ OUTPUT =========================================== */

struct Output console_out = {.console = true, .color = C_LIGHT_GRAY};
struct Output *out = &console_out;

// Separate programs: stdout may be a pipe (the shell captures it); colors then make no sense
bool stdout_is_tty = true;

static void flush_console_line(struct Output *o) {
    if (o->line_len > 0) {
        fd_write_all(1, o->line, o->line_len, o->color);
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
    return out->console && stdout_is_tty;
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


/* ================================ GLOBBING ========================================= */

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


void cwd_sync_kernel(void) {
    char path[SHELL_PATH_MAX];
    path_format(&cwd, path, sizeof(path));
    chdir(path, cwd.inode[cwd.depth]);
}

/* ================================ STANDALONE PROGRAMS ============================== */

// The shell overrides this (status bar text with the working directory)
__attribute__((weak)) void shell_update_status(void) {
}

// Rebuild `cwd` from an absolute path (the working directory the kernel keeps per process)
static void cwd_from_path(const char *path) {
    if (path == NULL || path[0] != '/')
        return;
    struct Resolved r;
    struct PathStack st;
    if (!resolve_path(path, &r, &st, true))
        return;
    if (r.exists && r.type == SYSCALL_TYPE_DIR) {
        path_free(&cwd);
        cwd = st;
    } else {
        path_free(&st);
    }
}

void shlib_init(void) {
    struct SyscallScreenInfo screen;
    console_screen_info(&screen);
    screen_cols = screen.cols;
    screen_rows = screen.rows;
    stdout_is_tty = fd_type(1) == SYSCALL_FD_CONSOLE;
    char path[SHELL_PATH_MAX];
    if (getcwd(path, sizeof(path)))
        cwd_from_path(path);
}

bool shlib_read_stdin(struct Input *in, char **owned) {
    memset(in, 0, sizeof(*in));
    *owned = NULL;
    if (fd_type(0) != SYSCALL_FD_PIPE_READ)
        return true; // console: commands treat it as "no pipeline input"
    uint32_t len = 0;
    char *data = fd_read_all(0, &len);
    if (data == NULL)
        return false;
    in->present = true;
    in->data    = data;
    in->len     = len;
    *owned      = data;
    return true;
}

// Program lookup for separate programs (execinfo): PATH from the environment.
// The shell has its own (strong) version that uses its PATH dynamic array.
__attribute__((weak)) bool shell_find_program(const char *name, struct Resolved *r) {
    if (strchr(name, '/') != NULL)
        return resolve_path(name, r, NULL, true) && r->exists && r->type == SYSCALL_TYPE_FILE;
    const char *p = getenv("PATH");
    char path[SHELL_PATH_MAX];
    while (p != NULL && *p != '\0') {
        const char *colon = strchr(p, ':');
        uint32_t len = colon ? (uint32_t) (colon - p) : (uint32_t) strlen(p);
        snprintf(path, sizeof(path), "%.*s/%s", (int) len, p, name);
        if (len > 0 && resolve_path(path, r, NULL, true) && r->exists && r->type == SYSCALL_TYPE_FILE)
            return true;
        p += len;
        if (*p == ':')
            p++;
    }
    return false;
}
