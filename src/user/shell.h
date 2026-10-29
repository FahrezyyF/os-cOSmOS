#ifndef _USER_SHELL_H
#define _USER_SHELL_H

#include "user/ulib.h"

/**
 * cOSmOS shell, the user program started by the kernel (book 3.4.2).
 *
 * shell.c     REPL, line editor (history, tab completion, scrolling), parser, pipelines,
 *             redirection, variables and .sh scripts
 * cmd_fs.c    file system commands (ls cd mkdir cat cp rm mv find ...)
 * cmd_text.c  text tools (echo grep head tail wc sort hexdump)
 * cmd_sys.c   system commands (help history free heap memtest sysinfo ...)
 */

#define SHELL_PATH_MAX   1024
#define SHELL_DEPTH_MAX  64
#define SHELL_NAME_MAX   255

/* -- Output: every command writes through `out` (console with colors, or a capture
 *    buffer feeding the next pipeline stage / a redirection) -- */
struct Output {
    bool     console;
    // capture (heap, grows without limit)
    char    *data;
    uint32_t len;
    uint32_t cap;
    // console line buffer (book: the shell buffers its output)
    char     line[256];
    uint32_t line_len;
    uint8_t  color;
};

extern struct Output *out;

void out_write(const char *s, uint32_t n);
void out_puts(const char *s);
void out_putc(char c);
void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void out_color(uint8_t color);          // console only (captured text stays plain)
void out_flush(void);
bool out_is_console(void);
void err_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2))); // console, red
void note_printf(uint8_t color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* -- Input of a pipeline stage (previous stage's output or `< file`) -- */
struct Input {
    bool        present;
    const char *data;
    uint32_t    len;
};

/* -- Paths -- */
struct PathStack {
    uint32_t inode[SHELL_DEPTH_MAX + 1]; // inode[0] = root
    char    *name[SHELL_DEPTH_MAX + 1];  // name[0] unused
    uint32_t depth;
};

struct Resolved {
    bool             exists;
    uint32_t         inode;   // target (when exists)
    uint8_t          type;    // SYSCALL_TYPE_*
    uint32_t         parent;  // directory that holds (or would hold) the target
    char             name[SHELL_NAME_MAX + 1]; // leaf name, "" for the root
    struct SyscallStat st;    // when exists
};

extern struct PathStack cwd;

void     path_copy(struct PathStack *dst, const struct PathStack *src);
void     path_free(struct PathStack *p);
void     path_format(const struct PathStack *p, char *buf, uint32_t cap);
// Directory path of an inode, built with SYS_FS_NAME_OF (for find results)
void     path_of_inode(uint32_t inode, char *buf, uint32_t cap);
/**
 * Resolve an absolute or relative path (., .. and repeated / allowed).
 * @param stack Optional: receives the directory stack of the target (or of its parent
 *              when the target does not exist)
 * @return false when an intermediate directory is missing (message printed unless quiet)
 */
bool     resolve_path(const char *path, struct Resolved *r, struct PathStack *stack, bool quiet);
bool     path_contains(const struct PathStack *p, uint32_t inode);
void     cwd_refresh(void);           // rebuild names after a rename
const char *fs_error(const char *op, int8_t code);

/* -- Commands -- */
typedef int (*CommandFunction)(int argc, char **argv, struct Input *in);

struct Command {
    const char     *name;
    CommandFunction run;
    const char     *usage;
    const char     *help;
};

extern const struct Command shell_commands[];
extern const uint32_t       shell_command_count;

// Option parsing helper: argv[i] like "-rf" sets flags; returns the index of the first operand
int  parse_flags(int argc, char **argv, const char *allowed, bool flags[128]);
// Whole input of a text tool: files from argv, or the pipeline input
bool read_all_inputs(int argc, char **argv, int first, struct Input *in, char **data, uint32_t *len, const char *tool);
bool read_whole_file(const char *path, char **data, uint32_t *len, const char *tool);
bool write_whole_file(const char *path, const char *data, uint32_t len, bool append, const char *tool);
void format_time(uint32_t unix_time, char *buf, uint32_t cap);
bool glob_match(const char *pattern, const char *name);

/* -- Shell state used by commands -- */
int  shell_run_script(const char *path, int argc, char **argv);
void shell_print_history(void);
void shell_set_var(const char *name, const char *value);
const char *shell_get_var(const char *name);
void shell_unset_var(const char *name);
void shell_print_vars(void);
void shell_request_exit(int code);
void shell_update_status(void);
extern uint8_t screen_cols;
extern uint8_t screen_rows;

#endif
