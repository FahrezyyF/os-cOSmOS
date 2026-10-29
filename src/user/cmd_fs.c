#include "user/shell.h"

#define RECURSION_MAX 48

/* ================================ HELPERS ========================================== */

int parse_flags(int argc, char **argv, const char *allowed, bool flags[128]) {
    memset(flags, 0, 128 * sizeof(bool));
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0')
            break;
        if (strcmp(a, "--") == 0)
            return i + 1;
        for (int j = 1; a[j] != '\0'; j++) {
            if ((unsigned char) a[j] >= 128 || strchr(allowed, a[j]) == NULL) {
                err_printf("%s: unknown option -%c\n", argv[0], a[j]);
                return -1;
            }
            flags[(int) a[j]] = true;
        }
    }
    return i;
}

bool read_whole_file(const char *path, char **data, uint32_t *len, const char *tool) {
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return false;
    if (!r.exists) {
        err_printf("%s: %s: no such file\n", tool, path);
        return false;
    }
    if (r.type == SYSCALL_TYPE_DIR) {
        err_printf("%s: %s: is a directory\n", tool, path);
        return false;
    }
    int8_t ret = fs_read_file(r.parent, r.name, (uint8_t) strlen(r.name), data, len);
    if (ret != 0) {
        err_printf("%s: %s: %s\n", tool, path, fs_error("read", ret));
        return false;
    }
    return true;
}

bool write_whole_file(const char *path, const char *data, uint32_t len, bool append, const char *tool) {
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return false;
    if (r.name[0] == '\0' || (r.exists && r.type == SYSCALL_TYPE_DIR)) {
        err_printf("%s: %s: is a directory\n", tool, path);
        return false;
    }
    uint8_t name_len = (uint8_t) strlen(r.name);
    char *merged = NULL;
    if (r.exists) {
        // EXT2 has no in-place overwrite / append: replace the file
        if (append) {
            char *old;
            uint32_t old_len;
            if (fs_read_file(r.parent, r.name, name_len, &old, &old_len) != 0) {
                err_printf("%s: %s: cannot read the file to append to\n", tool, path);
                return false;
            }
            merged = malloc(old_len + len + 1);
            if (merged == NULL) {
                free(old);
                err_printf("%s: out of memory\n", tool);
                return false;
            }
            memcpy(merged, old, old_len);
            memcpy(merged + old_len, data, len);
            free(old);
            data = merged;
            len += old_len;
        }
        int8_t del = fs_delete(r.parent, r.name, name_len, false);
        if (del != 0) {
            free(merged);
            err_printf("%s: %s: %s\n", tool, path, fs_error("delete", del));
            return false;
        }
    }
    int8_t ret = fs_write(r.parent, r.name, name_len, data, len);
    free(merged);
    if (ret != 0) {
        err_printf("%s: %s: %s\n", tool, path, fs_error("write", ret));
        return false;
    }
    return true;
}

bool read_all_inputs(int argc, char **argv, int first, struct Input *in, char **data, uint32_t *len, const char *tool) {
    if (first >= argc) {
        if (in == NULL || !in->present) {
            err_printf("%s: no input (give a file or pipe something in)\n", tool);
            return false;
        }
        *data = strndup(in->data ? in->data : "", in->len);
        *len  = in->len;
        return *data != NULL;
    }
    // Several files are concatenated (like cat)
    char *all = NULL;
    uint32_t total = 0;
    for (int i = first; i < argc; i++) {
        char *part;
        uint32_t part_len;
        if (!read_whole_file(argv[i], &part, &part_len, tool)) {
            free(all);
            return false;
        }
        char *bigger = realloc(all, total + part_len + 1);
        if (bigger == NULL) {
            free(part);
            free(all);
            return false;
        }
        all = bigger;
        memcpy(all + total, part, part_len);
        total += part_len;
        all[total] = '\0';
        free(part);
    }
    *data = all;
    *len  = total;
    return true;
}

void format_time(uint32_t unix_time, char *buf, uint32_t cap) {
    // days -> civil date (Howard Hinnant's algorithm)
    int32_t  z   = (int32_t) (unix_time / 86400) + 719468;
    uint32_t rem = unix_time % 86400;
    int32_t  era = z / 146097;
    uint32_t doe = (uint32_t) (z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t  y   = (int32_t) yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp  = (5 * doy + 2) / 153;
    uint32_t d   = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m   = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(buf, cap, "%04d-%02u-%02u %02u:%02u", (int) y, m, d, rem / 3600, (rem / 60) % 60);
}

static bool is_script_name(const char *name) {
    uint32_t n = (uint32_t) strlen(name);
    return n > 3 && strcmp(name + n - 3, ".sh") == 0;
}

static uint8_t entry_color(uint8_t type, const char *name) {
    if (type == SYSCALL_TYPE_DIR)
        return C_LIGHT_BLUE;
    if (is_script_name(name))
        return C_LIGHT_GREEN;
    if (strcmp(name, "shell") == 0)
        return C_LIGHT_MAGENTA;
    return C_LIGHT_GRAY;
}

static void sort_entries(struct DirEntry *e, uint32_t n) {
    // Insertion sort is fine for directory sizes (EXT2 IF2130 has 512 inodes in total)
    for (uint32_t i = 1; i < n; i++) {
        struct DirEntry x = e[i];
        uint32_t j = i;
        while (j > 0 && strcmp(e[j - 1].name, x.name) > 0) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = x;
    }
}

/* ================================ ls / cd / pwd ==================================== */

static void print_long(uint32_t dir, const struct DirEntry *e, uint32_t *bytes) {
    struct SyscallStat st;
    char when[32] = "?";
    uint32_t size = 0;
    if (fs_stat(dir, e->name, e->name_len, &st) == 0) {
        size = st.size;
        format_time(st.mtime, when, sizeof(when));
    }
    if (e->type != SYSCALL_TYPE_DIR)
        *bytes += size;
    out_color(C_DARK_GRAY);
    if (e->type == SYSCALL_TYPE_DIR)
        out_printf("d %10s  %s  ", "-", when);
    else
        out_printf("- %10u  %s  ", size, when);
    out_color(entry_color(e->type, e->name));
    out_printf("%s%s\n", e->name, e->type == SYSCALL_TYPE_DIR ? "/" : "");
    out_color(C_LIGHT_GRAY);
}

static int list_path(const char *path, bool long_format, bool all, bool header) {
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return 1;
    if (!r.exists) {
        err_printf("ls: %s: no such file or directory\n", path);
        return 1;
    }
    if (r.type == SYSCALL_TYPE_FILE) {
        struct DirEntry e = {.inode = r.inode, .type = r.type, .name_len = (uint8_t) strlen(r.name)};
        memcpy(e.name, r.name, e.name_len + 1);
        uint32_t bytes = 0;
        if (long_format)
            print_long(r.parent, &e, &bytes);
        else
            out_printf("%s\n", r.name);
        return 0;
    }
    struct DirEntry *entries;
    uint32_t count;
    int8_t ret = fs_list(r.inode, &entries, &count);
    if (ret != 0) {
        err_printf("ls: %s: cannot read directory (%d)\n", path, ret);
        return 1;
    }
    sort_entries(entries, count);
    if (header)
        out_printf("%s:\n", path);

    uint32_t shown = 0, bytes = 0, width = 0;
    for (uint32_t i = 0; i < count; i++)
        if ((all || entries[i].name[0] != '.') && entries[i].name_len + 3u > width)
            width = entries[i].name_len + 3u;
    uint32_t per_row = width ? screen_cols / width : 1;
    if (per_row == 0)
        per_row = 1;
    for (uint32_t i = 0; i < count; i++) {
        struct DirEntry *e = &entries[i];
        if (!all && e->name[0] == '.')
            continue;
        if (long_format) {
            print_long(r.inode, e, &bytes);
        } else if (!out_is_console()) {
            out_printf("%s\n", e->name); // one name per line into pipes / files, like ls
        } else {
            out_color(entry_color(e->type, e->name));
            out_printf("%s%s", e->name, e->type == SYSCALL_TYPE_DIR ? "/" : "");
            out_color(C_LIGHT_GRAY);
            uint32_t used = e->name_len + (e->type == SYSCALL_TYPE_DIR ? 1u : 0u);
            if ((shown + 1) % per_row == 0) {
                out_putc('\n');
            } else {
                for (uint32_t k = used; k < width; k++)
                    out_putc(' ');
            }
        }
        shown++;
    }
    if (!long_format && out_is_console() && shown % per_row != 0)
        out_putc('\n');
    if (long_format) {
        out_color(C_DARK_GRAY);
        out_printf("%u entries, %u bytes in files\n", shown, bytes);
        out_color(C_LIGHT_GRAY);
    }
    free(entries);
    return 0;
}

static int cmd_ls(int argc, char **argv, struct Input *in) {
    (void) in;
    bool f[128];
    int first = parse_flags(argc, argv, "la", f);
    if (first < 0)
        return 2;
    if (first >= argc)
        return list_path(".", f['l'], f['a'], false);
    int status = 0;
    for (int i = first; i < argc; i++) {
        status |= list_path(argv[i], f['l'], f['a'], argc - first > 1);
        if (argc - first > 1 && i + 1 < argc)
            out_putc('\n');
    }
    return status;
}

static struct PathStack previous_dir = {.inode = {FS_ROOT}, .depth = 0};

static int cmd_cd(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc > 2) {
        err_printf("cd: too many arguments\n");
        return 1;
    }
    struct PathStack target;
    if (argc == 1 || strcmp(argv[1], "/") == 0) {
        target.depth    = 0;
        target.inode[0] = FS_ROOT;
    } else if (strcmp(argv[1], "-") == 0) {
        path_copy(&target, &previous_dir);
    } else {
        struct Resolved r;
        if (!resolve_path(argv[1], &r, &target, false))
            return 1;
        if (!r.exists || r.type != SYSCALL_TYPE_DIR) {
            err_printf("cd: %s: %s\n", argv[1], r.exists ? "not a directory" : "no such directory");
            path_free(&target);
            return 1;
        }
    }
    path_free(&previous_dir);
    previous_dir = cwd;
    cwd = target;
    shell_update_status();
    return 0;
}

static int cmd_pwd(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    char path[SHELL_PATH_MAX];
    path_format(&cwd, path, sizeof(path));
    out_printf("%s\n", path);
    return 0;
}

/* ================================ mkdir / touch / cat ============================== */

static int make_dir(const char *path, bool parents) {
    if (parents) {
        // create every missing component in turn
        char partial[SHELL_PATH_MAX];
        uint32_t n = (uint32_t) strlen(path);
        if (n >= sizeof(partial)) {
            err_printf("mkdir: path too long\n");
            return 1;
        }
        for (uint32_t i = 1; i <= n; i++) {
            if (i < n && path[i] != '/')
                continue;
            memcpy(partial, path, i);
            partial[i] = '\0';
            struct Resolved r;
            if (!resolve_path(partial, &r, NULL, false))
                return 1;
            if (r.exists) {
                if (r.type != SYSCALL_TYPE_DIR) {
                    err_printf("mkdir: %s: not a directory\n", partial);
                    return 1;
                }
                continue;
            }
            int8_t ret = fs_mkdir(r.parent, r.name, (uint8_t) strlen(r.name));
            if (ret != 0) {
                err_printf("mkdir: %s: %s\n", partial, fs_error("write", ret));
                return 1;
            }
        }
        return 0;
    }
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return 1;
    if (r.exists || r.name[0] == '\0') {
        err_printf("mkdir: %s: already exists\n", path);
        return 1;
    }
    int8_t ret = fs_mkdir(r.parent, r.name, (uint8_t) strlen(r.name));
    if (ret != 0) {
        err_printf("mkdir: %s: %s\n", path, fs_error("write", ret));
        return 1;
    }
    return 0;
}

static int cmd_mkdir(int argc, char **argv, struct Input *in) {
    (void) in;
    bool f[128];
    int first = parse_flags(argc, argv, "p", f);
    if (first < 0)
        return 2;
    if (first >= argc) {
        err_printf("usage: mkdir [-p] <dir>...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++)
        status |= make_dir(argv[i], f['p']);
    return status;
}

static int cmd_touch(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc < 2) {
        err_printf("usage: touch <file>...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct Resolved r;
        if (!resolve_path(argv[i], &r, NULL, false)) {
            status = 1;
            continue;
        }
        if (r.exists)
            continue;
        int8_t ret = fs_write(r.parent, r.name, (uint8_t) strlen(r.name), "", 0);
        if (ret != 0) {
            err_printf("touch: %s: %s\n", argv[i], fs_error("write", ret));
            status = 1;
        }
    }
    return status;
}

static void write_numbered(const char *data, uint32_t len, uint32_t *line_no, bool *at_line_start) {
    for (uint32_t i = 0; i < len; i++) {
        if (*at_line_start) {
            out_color(C_DARK_GRAY);
            out_printf("%6u  ", (*line_no)++);
            out_color(C_LIGHT_GRAY);
            *at_line_start = false;
        }
        out_putc(data[i]);
        if (data[i] == '\n')
            *at_line_start = true;
    }
}

static int cmd_cat(int argc, char **argv, struct Input *in) {
    bool f[128];
    int first = parse_flags(argc, argv, "n", f);
    if (first < 0)
        return 2;
    uint32_t line_no = 1;
    bool at_line_start = true;
    char last = '\n';
    if (first >= argc) {
        if (in == NULL || !in->present) {
            err_printf("usage: cat [-n] <file>... (or pipe text in)\n");
            return 2;
        }
        if (f['n'])
            write_numbered(in->data, in->len, &line_no, &at_line_start);
        else
            out_write(in->data, in->len);
        if (in->len > 0)
            last = in->data[in->len - 1];
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        char *data;
        uint32_t len;
        if (!read_whole_file(argv[i], &data, &len, "cat")) {
            status = 1;
            continue;
        }
        if (f['n'])
            write_numbered(data, len, &line_no, &at_line_start);
        else
            out_write(data, len);
        if (len > 0)
            last = data[len - 1];
        free(data);
    }
    // keep the prompt on its own line when a file does not end with LF
    if (last != '\n' && out_is_console())
        out_putc('\n');
    return status;
}

/* ================================ cp / rm / mv ===================================== */

static int copy_entry(uint32_t src_parent, const char *src_name, uint8_t type, uint32_t dst_parent,
                      const char *dst_name, bool recursive, uint32_t depth, const char *shown) {
    uint8_t src_len = (uint8_t) strlen(src_name), dst_len = (uint8_t) strlen(dst_name);
    if (type == SYSCALL_TYPE_FILE) {
        char *data;
        uint32_t len;
        int8_t ret = fs_read_file(src_parent, src_name, src_len, &data, &len);
        if (ret != 0) {
            err_printf("cp: %s: %s\n", shown, fs_error("read", ret));
            return 1;
        }
        // an existing file at the destination is replaced (like cp)
        struct SyscallStat st;
        if (fs_stat(dst_parent, dst_name, dst_len, &st) == 0) {
            if (st.type == SYSCALL_TYPE_DIR) {
                free(data);
                err_printf("cp: %s: a directory with that name exists\n", dst_name);
                return 1;
            }
            fs_delete(dst_parent, dst_name, dst_len, false);
        }
        ret = fs_write(dst_parent, dst_name, dst_len, data, len);
        free(data);
        if (ret != 0) {
            err_printf("cp: %s: %s\n", dst_name, fs_error("write", ret));
            return 1;
        }
        return 0;
    }
    if (!recursive) {
        err_printf("cp: %s: is a directory (use cp -r)\n", shown);
        return 1;
    }
    if (depth > RECURSION_MAX) {
        err_printf("cp: %s: too deep\n", shown);
        return 1;
    }
    struct SyscallStat src_st, dst_st;
    if (fs_stat(src_parent, src_name, src_len, &src_st) != 0)
        return 1;
    if (fs_stat(dst_parent, dst_name, dst_len, &dst_st) != 0) {
        int8_t ret = fs_mkdir(dst_parent, dst_name, dst_len);
        if (ret != 0 || fs_stat(dst_parent, dst_name, dst_len, &dst_st) != 0) {
            err_printf("cp: %s: %s\n", dst_name, fs_error("write", ret));
            return 1;
        }
    } else if (dst_st.type != SYSCALL_TYPE_DIR) {
        err_printf("cp: %s: not a directory\n", dst_name);
        return 1;
    }
    struct DirEntry *entries;
    uint32_t count;
    if (fs_list(src_st.inode, &entries, &count) != 0) {
        err_printf("cp: %s: cannot read directory\n", shown);
        return 1;
    }
    int status = 0;
    for (uint32_t i = 0; i < count; i++)
        status |= copy_entry(src_st.inode, entries[i].name, entries[i].type, dst_st.inode, entries[i].name,
                             true, depth + 1, entries[i].name);
    free(entries);
    return status;
}

/**
 * Where does `src` go when the user names `dst`? An existing directory receives it under
 * its own name, anything else is the new name itself.
 */
static bool destination(const char *dst, const char *src_leaf, bool many, uint32_t *parent, char *name,
                        struct PathStack *dst_stack, const char *tool) {
    struct Resolved d;
    if (!resolve_path(dst, &d, dst_stack, false))
        return false;
    if (d.exists && d.type == SYSCALL_TYPE_DIR) {
        *parent = d.inode;
        strncpy(name, src_leaf, SHELL_NAME_MAX);
        name[SHELL_NAME_MAX] = '\0';
        return true;
    }
    if (many) {
        err_printf("%s: %s: target is not a directory\n", tool, dst);
        path_free(dst_stack);
        return false;
    }
    if (d.name[0] == '\0') {
        err_printf("%s: %s: invalid destination\n", tool, dst);
        path_free(dst_stack);
        return false;
    }
    *parent = d.parent;
    strncpy(name, d.name, SHELL_NAME_MAX);
    name[SHELL_NAME_MAX] = '\0';
    return true;
}

static int cmd_cp(int argc, char **argv, struct Input *in) {
    (void) in;
    bool f[128];
    int first = parse_flags(argc, argv, "rR", f);
    if (first < 0)
        return 2;
    bool recursive = f['r'] || f['R'];
    if (argc - first < 2) {
        err_printf("usage: cp [-r] <source>... <destination>\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    bool many = argc - first > 2;
    int status = 0;
    for (int i = first; i < argc - 1; i++) {
        struct Resolved s;
        if (!resolve_path(argv[i], &s, NULL, false)) {
            status = 1;
            continue;
        }
        if (!s.exists || s.name[0] == '\0') {
            err_printf("cp: %s: %s\n", argv[i], s.exists ? "cannot copy the root" : "no such file or directory");
            status = 1;
            continue;
        }
        uint32_t parent;
        char name[SHELL_NAME_MAX + 1];
        struct PathStack dst_stack;
        if (!destination(dst, s.name, many, &parent, name, &dst_stack, "cp")) {
            status = 1;
            continue;
        }
        // Copying a folder into itself would never end
        bool into_self = s.type == SYSCALL_TYPE_DIR && (path_contains(&dst_stack, s.inode) || parent == s.inode);
        path_free(&dst_stack);
        if (into_self) {
            err_printf("cp: cannot copy %s into itself\n", argv[i]);
            status = 1;
            continue;
        }
        if (parent == s.parent && strcmp(name, s.name) == 0) {
            err_printf("cp: %s and %s are the same file\n", argv[i], dst);
            status = 1;
            continue;
        }
        status |= copy_entry(s.parent, s.name, s.type, parent, name, recursive, 0, argv[i]);
    }
    return status;
}

static int remove_entry(uint32_t parent, const char *name, uint8_t type, uint32_t depth, const char *shown) {
    uint8_t len = (uint8_t) strlen(name);
    if (type == SYSCALL_TYPE_DIR) {
        if (depth > RECURSION_MAX) {
            err_printf("rm: %s: too deep\n", shown);
            return 1;
        }
        struct SyscallStat st;
        if (fs_stat(parent, name, len, &st) != 0)
            return 1;
        struct DirEntry *entries;
        uint32_t count;
        if (fs_list(st.inode, &entries, &count) != 0) {
            err_printf("rm: %s: cannot read directory\n", shown);
            return 1;
        }
        int status = 0;
        for (uint32_t i = 0; i < count; i++)
            status |= remove_entry(st.inode, entries[i].name, entries[i].type, depth + 1, entries[i].name);
        free(entries);
        if (status != 0)
            return status;
    }
    int8_t ret = fs_delete(parent, name, len, type == SYSCALL_TYPE_DIR);
    if (ret != 0) {
        err_printf("rm: %s: %s\n", shown, fs_error("delete", ret));
        return 1;
    }
    return 0;
}

static int cmd_rm(int argc, char **argv, struct Input *in) {
    (void) in;
    bool f[128];
    int first = parse_flags(argc, argv, "rRf", f);
    if (first < 0)
        return 2;
    bool recursive = f['r'] || f['R'];
    if (first >= argc) {
        err_printf("usage: rm [-r] [-f] <path>...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        struct Resolved r;
        if (!resolve_path(argv[i], &r, NULL, f['f'])) {
            status |= f['f'] ? 0 : 1;
            continue;
        }
        if (!r.exists) {
            if (!f['f']) {
                err_printf("rm: %s: no such file or directory\n", argv[i]);
                status = 1;
            }
            continue;
        }
        if (r.name[0] == '\0' || r.inode == FS_ROOT) {
            err_printf("rm: refusing to remove the root directory\n");
            status = 1;
            continue;
        }
        if (r.type == SYSCALL_TYPE_DIR && !recursive) {
            err_printf("rm: %s: is a directory (use rm -r)\n", argv[i]);
            status = 1;
            continue;
        }
        if (path_contains(&cwd, r.inode)) {
            err_printf("rm: %s: is the working directory or one of its parents (cd out first)\n", argv[i]);
            status = 1;
            continue;
        }
        status |= remove_entry(r.parent, r.name, r.type, 0, argv[i]);
    }
    return status;
}

static int cmd_mv(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc < 3) {
        err_printf("usage: mv <source>... <destination>\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    bool many = argc > 3;
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        struct Resolved s;
        if (!resolve_path(argv[i], &s, NULL, false)) {
            status = 1;
            continue;
        }
        if (!s.exists || s.name[0] == '\0' || s.inode == FS_ROOT) {
            err_printf("mv: %s: %s\n", argv[i], s.exists ? "cannot move the root" : "no such file or directory");
            status = 1;
            continue;
        }
        uint32_t parent;
        char name[SHELL_NAME_MAX + 1];
        struct PathStack dst_stack;
        if (!destination(dst, s.name, many, &parent, name, &dst_stack, "mv")) {
            status = 1;
            continue;
        }
        bool into_self = s.type == SYSCALL_TYPE_DIR && (path_contains(&dst_stack, s.inode) || parent == s.inode);
        path_free(&dst_stack);
        if (into_self) {
            err_printf("mv: cannot move %s into itself\n", argv[i]);
            status = 1;
            continue;
        }
        if (parent == s.parent && strcmp(name, s.name) == 0)
            continue; // same place
        // Like mv: an existing file at the destination is replaced
        struct SyscallStat st;
        uint8_t name_len = (uint8_t) strlen(name);
        if (fs_stat(parent, name, name_len, &st) == 0) {
            if (st.type == SYSCALL_TYPE_DIR || s.type == SYSCALL_TYPE_DIR) {
                err_printf("mv: %s: target already exists\n", name);
                status = 1;
                continue;
            }
            fs_delete(parent, name, name_len, false);
        }
        int8_t ret = fs_rename(s.parent, s.name, (uint8_t) strlen(s.name), parent, name, name_len);
        if (ret != 0) {
            err_printf("mv: %s: %s\n", argv[i], fs_error("rename", ret));
            status = 1;
        }
    }
    cwd_refresh(); // renamed folders may be part of the working directory
    shell_update_status();
    return status;
}

/* ================================ find / tree / stat =============================== */

static uint32_t find_walk(uint32_t dir, const char *prefix, const char *pattern, uint32_t depth) {
    if (depth > RECURSION_MAX)
        return 0;
    struct DirEntry *entries;
    uint32_t count, found = 0;
    if (fs_list(dir, &entries, &count) != 0)
        return 0;
    sort_entries(entries, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t plen = (uint32_t) strlen(prefix);
        char *path = malloc(plen + entries[i].name_len + 2);
        if (path == NULL)
            break;
        snprintf(path, plen + entries[i].name_len + 2, "%s%s%s", prefix,
                 (plen > 0 && prefix[plen - 1] == '/') ? "" : "/", entries[i].name);
        if (glob_match(pattern, entries[i].name)) {
            out_color(entry_color(entries[i].type, entries[i].name));
            out_puts(path); // deep paths can be longer than out_printf()'s buffer
            out_puts(entries[i].type == SYSCALL_TYPE_DIR ? "/\n" : "\n");
            out_color(C_LIGHT_GRAY);
            found++;
        }
        if (entries[i].type == SYSCALL_TYPE_DIR)
            found += find_walk(entries[i].inode, path, pattern, depth + 1);
        free(path);
    }
    free(entries);
    return found;
}

static bool has_wildcard(const char *s) {
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

static int cmd_find(int argc, char **argv, struct Input *in) {
    (void) in;
    // find <name> | find <dir> <name> | find <dir> -name <pattern>
    const char *dir = NULL, *name = NULL;
    if (argc == 2) {
        name = argv[1];
    } else if (argc == 3) {
        dir  = argv[1];
        name = argv[2];
    } else if (argc == 4 && strcmp(argv[2], "-name") == 0) {
        dir  = argv[1];
        name = argv[3];
    } else {
        err_printf("usage: find <name> | find <dir> <name|pattern>   (* and ? allowed, quote them)\n");
        return 2;
    }
    uint32_t base = FS_ROOT;
    char base_path[SHELL_PATH_MAX] = "/";
    if (dir != NULL) {
        struct Resolved r;
        struct PathStack st;
        if (!resolve_path(dir, &r, &st, false))
            return 1;
        if (!r.exists || r.type != SYSCALL_TYPE_DIR) {
            err_printf("find: %s: no such directory\n", dir);
            path_free(&st);
            return 1;
        }
        base = r.inode;
        path_format(&st, base_path, sizeof(base_path));
        path_free(&st);
    }
    if (strlen(name) > SHELL_NAME_MAX) {
        err_printf("find: name too long\n");
        return 2;
    }

    if (has_wildcard(name)) {
        // Pattern: depth-first walk of the tree (EXT2 is a tree, book "EXT2, Graph, Tree")
        uint32_t found = find_walk(base, strcmp(base_path, "/") == 0 ? "" : base_path, name, 0);
        return found > 0 ? 0 : 1;
    }

    // Exact name: answered by the file system's on-disk name index (Milestone 2 bonus)
    uint32_t max_hits = 256;
    struct SyscallFindHit *hits = malloc(max_hits * sizeof(*hits));
    if (hits == NULL)
        return 1;
    uint32_t total = fs_find(name, (uint8_t) strlen(name), hits, max_hits);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < total && i < max_hits; i++) {
        char path[SHELL_PATH_MAX];
        path_of_inode(hits[i].parent, path, sizeof(path));
        // limit to the requested directory (an ancestor check via its path prefix)
        if (base != FS_ROOT) {
            uint32_t bl = (uint32_t) strlen(base_path);
            if (strncmp(path, base_path, bl) != 0 || (path[bl] != '\0' && path[bl] != '/'))
                continue;
        }
        out_color(entry_color(hits[i].type, name));
        out_puts(path);
        if (strcmp(path, "/") != 0)
            out_putc('/');
        out_puts(name);
        out_puts(hits[i].type == SYSCALL_TYPE_DIR ? "/\n" : "\n");
        out_color(C_LIGHT_GRAY);
        shown++;
    }
    if (total > max_hits)
        note_printf(C_DARK_GRAY, "(%u more not shown)\n", total - max_hits);
    free(hits);
    if (shown == 0 && out_is_console())
        note_printf(C_DARK_GRAY, "find: no file or folder named '%s'\n", name);
    return shown > 0 ? 0 : 1;
}

// CP437 box drawing characters of the console font
#define TREE_TEE    "\xC3\xC4\xC4 "
#define TREE_LAST   "\xC0\xC4\xC4 "
#define TREE_PIPE   "\xB3   "
#define TREE_BLANK  "    "

static void tree_walk(uint32_t dir, char *prefix, uint32_t prefix_len, uint32_t depth, uint32_t *dirs, uint32_t *files) {
    if (depth > RECURSION_MAX)
        return;
    struct DirEntry *entries;
    uint32_t count;
    if (fs_list(dir, &entries, &count) != 0)
        return;
    sort_entries(entries, count);
    for (uint32_t i = 0; i < count; i++) {
        bool last = i + 1 == count;
        out_color(C_DARK_GRAY);
        out_write(prefix, prefix_len);
        out_puts(last ? TREE_LAST : TREE_TEE);
        out_color(entry_color(entries[i].type, entries[i].name));
        out_printf("%s\n", entries[i].name);
        out_color(C_LIGHT_GRAY);
        if (entries[i].type == SYSCALL_TYPE_DIR) {
            (*dirs)++;
            if (prefix_len + 4 < SHELL_PATH_MAX) {
                memcpy(prefix + prefix_len, last ? TREE_BLANK : TREE_PIPE, 4);
                tree_walk(entries[i].inode, prefix, prefix_len + 4, depth + 1, dirs, files);
            }
        } else {
            (*files)++;
        }
    }
    free(entries);
}

static int cmd_tree(int argc, char **argv, struct Input *in) {
    (void) in;
    const char *path = argc > 1 ? argv[1] : ".";
    struct Resolved r;
    if (!resolve_path(path, &r, NULL, false))
        return 1;
    if (!r.exists || r.type != SYSCALL_TYPE_DIR) {
        err_printf("tree: %s: no such directory\n", path);
        return 1;
    }
    out_color(C_LIGHT_BLUE);
    out_printf("%s\n", path);
    out_color(C_LIGHT_GRAY);
    char prefix[SHELL_PATH_MAX];
    uint32_t dirs = 0, files = 0;
    tree_walk(r.inode, prefix, 0, 0, &dirs, &files);
    out_printf("\n%u directories, %u files\n", dirs, files);
    return 0;
}

static int cmd_stat(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc < 2) {
        err_printf("usage: stat <path>...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct Resolved r;
        if (!resolve_path(argv[i], &r, NULL, false) || !r.exists) {
            if (!r.exists)
                err_printf("stat: %s: no such file or directory\n", argv[i]);
            status = 1;
            continue;
        }
        char ctime[32], mtime[32];
        format_time(r.st.ctime, ctime, sizeof(ctime));
        format_time(r.st.mtime, mtime, sizeof(mtime));
        out_printf("  File: %s\n", argv[i]);
        out_printf("  Type: %s    Inode: %u    Parent: %u\n", r.type == SYSCALL_TYPE_DIR ? "directory" : "regular file",
                   r.inode, r.parent);
        out_printf("  Size: %u bytes    Blocks: %u x 512 B", r.st.size, r.st.blocks);
        if (r.st.extents > 0)
            out_printf("    Extents: %u", r.st.extents);
        if (r.st.links > 0)
            out_printf("    Links: %u", r.st.links);
        out_printf("\nCreate: %s UTC\nModify: %s UTC\n", ctime, mtime);
    }
    return status;
}

/* ================================ df / fsck ======================================== */

static int cmd_df(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallFsInfo info;
    fs_info(&info);
    uint32_t used = info.total_blocks - info.free_blocks;
    out_printf("File system : %s\n", info.fs_name);
    out_printf("Device      : %s\n", info.device);
    out_printf("Blocks      : %u x %u B = %u KiB, used %u KiB (%u%%), free %u KiB\n", info.total_blocks,
               info.block_size, info.total_blocks / 2, used / 2,
               info.total_blocks ? used * 100 / info.total_blocks : 0, info.free_blocks / 2);
    out_printf("Inodes      : %u total, %u free\n", info.total_nodes, info.free_nodes);
    out_printf("Mounted     : %u times", info.mount_count);
    if (info.journal_sequence > 0)
        out_printf(", journal sequence %u", info.journal_sequence);
    out_putc('\n');
    return 0;
}

static int cmd_fsck(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallFsCheck result;
    fs_check(&result);
    out_color(result.clean ? C_LIGHT_GREEN : C_LIGHT_RED);
    out_printf("fsck: %s\n", result.message);
    out_color(C_LIGHT_GRAY);
    return result.clean ? 0 : 1;
}

/* ================================ COMMAND TABLE ==================================== */

int cmd_echo(int argc, char **argv, struct Input *in);
int cmd_grep(int argc, char **argv, struct Input *in);
int cmd_head(int argc, char **argv, struct Input *in);
int cmd_tail(int argc, char **argv, struct Input *in);
int cmd_wc(int argc, char **argv, struct Input *in);
int cmd_sort(int argc, char **argv, struct Input *in);
int cmd_hexdump(int argc, char **argv, struct Input *in);
int cmd_write(int argc, char **argv, struct Input *in);
int cmd_help(int argc, char **argv, struct Input *in);
int cmd_clear(int argc, char **argv, struct Input *in);
int cmd_history(int argc, char **argv, struct Input *in);
int cmd_sh(int argc, char **argv, struct Input *in);
int cmd_set(int argc, char **argv, struct Input *in);
int cmd_unset(int argc, char **argv, struct Input *in);
int cmd_exit(int argc, char **argv, struct Input *in);
int cmd_true(int argc, char **argv, struct Input *in);
int cmd_false(int argc, char **argv, struct Input *in);
int cmd_date(int argc, char **argv, struct Input *in);
int cmd_uptime(int argc, char **argv, struct Input *in);
int cmd_sleep(int argc, char **argv, struct Input *in);
int cmd_beep(int argc, char **argv, struct Input *in);
int cmd_free(int argc, char **argv, struct Input *in);
int cmd_heap(int argc, char **argv, struct Input *in);
int cmd_memtest(int argc, char **argv, struct Input *in);
int cmd_sysinfo(int argc, char **argv, struct Input *in);
int cmd_crash(int argc, char **argv, struct Input *in);
int cmd_reboot(int argc, char **argv, struct Input *in);
int cmd_shutdown(int argc, char **argv, struct Input *in);

const struct Command shell_commands[] = {
    {"ls",       cmd_ls,       "[-l] [-a] [path]...",      "list a directory"},
    {"cd",       cmd_cd,       "[path | -]",               "change the working directory (.. goes up)"},
    {"pwd",      cmd_pwd,      "",                         "print the working directory"},
    {"mkdir",    cmd_mkdir,    "[-p] <dir>...",            "create folders"},
    {"touch",    cmd_touch,    "<file>...",                "create empty files"},
    {"cat",      cmd_cat,      "[-n] [file]...",           "print files (or the pipeline input)"},
    {"cp",       cmd_cp,       "[-r] <src>... <dst>",      "copy files, -r copies folders"},
    {"rm",       cmd_rm,       "[-r] [-f] <path>...",      "remove files, -r removes folders"},
    {"mv",       cmd_mv,       "<src>... <dst>",           "move / rename files and folders"},
    {"find",     cmd_find,     "[dir] <name|pattern>",     "find by name in the whole file system"},
    {"grep",     cmd_grep,     "[-ivncloFr] <regex> [file]...", "search lines (regular expressions)"},
    {"tree",     cmd_tree,     "[path]",                   "show a directory tree"},
    {"stat",     cmd_stat,     "<path>...",                "file details (inode, size, times)"},
    {"echo",     cmd_echo,     "[-n] [text]...",           "print text"},
    {"write",    cmd_write,    "<file> [text]...",         "write text (or the pipeline input) to a file"},
    {"head",     cmd_head,     "[-n N] [file]...",         "first lines"},
    {"tail",     cmd_tail,     "[-n N] [file]...",         "last lines"},
    {"wc",       cmd_wc,       "[-lwc] [file]...",         "count lines, words and bytes"},
    {"sort",     cmd_sort,     "[-r] [-n] [file]...",      "sort lines"},
    {"hexdump",  cmd_hexdump,  "[file]",                   "hex + ASCII dump"},
    {"sh",       cmd_sh,       "<script.sh> [args]...",    "run a shell script (also: ./x.sh, source)"},
    {"source",   cmd_sh,       "<script.sh> [args]...",    "same as sh"},
    {"set",      cmd_set,      "[NAME value]",             "list / set shell variables (also NAME=value)"},
    {"unset",    cmd_unset,    "<NAME>...",                "remove shell variables"},
    {"history",  cmd_history,  "",                         "previous commands (!! and !n repeat them)"},
    {"clear",    cmd_clear,    "",                         "clear the screen"},
    {"help",     cmd_help,     "[command]",                "this list"},
    {"date",     cmd_date,     "",                         "current date and time (CMOS RTC)"},
    {"uptime",   cmd_uptime,   "",                         "time since boot"},
    {"sleep",    cmd_sleep,    "<ms>",                     "wait"},
    {"beep",     cmd_beep,     "[Hz] [ms]",                "PC speaker"},
    {"free",     cmd_free,     "",                         "physical memory, paging and swap"},
    {"heap",     cmd_heap,     "[check]",                  "user and kernel heap statistics"},
    {"memtest",  cmd_memtest,  "<MiB>",                    "allocate and verify memory (exercises swap)"},
    {"sysinfo",  cmd_sysinfo,  "",                         "hardware and kernel summary"},
    {"df",       cmd_df,       "",                         "file system usage"},
    {"fsck",     cmd_fsck,     "",                         "check the file system"},
    {"crash",    cmd_crash,    "[kernel|gpf|div]",         "fault on purpose (the kernel restarts the shell)"},
    {"true",     cmd_true,     "",                         "exit status 0"},
    {"false",    cmd_false,    "",                         "exit status 1"},
    {"exit",     cmd_exit,     "[code]",                   "exit the shell (the kernel starts a new one)"},
    {"reboot",   cmd_reboot,   "",                         "restart the machine"},
    {"shutdown", cmd_shutdown, "",                         "power off"},
};

const uint32_t shell_command_count = sizeof(shell_commands) / sizeof(shell_commands[0]);
