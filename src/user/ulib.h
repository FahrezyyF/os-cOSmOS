#ifndef _ULIB_H
#define _ULIB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "header/kernel/syscall.h"
#include "header/filesystem/ext2.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"
#include "header/stdlib/heap.h"

/**
 * cOSmOS user library: everything a ring 3 program needs on top of `int 0x30`.
 * Pure C helpers (string.c, format.c, heap.c) are the same sources the kernel uses.
 */

/* -- VGA attribute colors (fg | bg << 4) -- */
#define C_BLACK         0x0
#define C_BLUE          0x1
#define C_GREEN         0x2
#define C_CYAN          0x3
#define C_RED           0x4
#define C_MAGENTA       0x5
#define C_BROWN         0x6
#define C_LIGHT_GRAY    0x7
#define C_DARK_GRAY     0x8
#define C_LIGHT_BLUE    0x9
#define C_LIGHT_GREEN   0xA
#define C_LIGHT_CYAN    0xB
#define C_LIGHT_RED     0xC
#define C_LIGHT_MAGENTA 0xD
#define C_YELLOW        0xE
#define C_WHITE         0xF

/* -- Special keys delivered by SYS_GETCHAR (keyboard.h) -- */
#define KEY_UP        ((char) 0x80)
#define KEY_DOWN      ((char) 0x81)
#define KEY_LEFT      ((char) 0x82)
#define KEY_RIGHT     ((char) 0x83)
#define KEY_HOME      ((char) 0x84)
#define KEY_END       ((char) 0x85)
#define KEY_PGUP      ((char) 0x86)
#define KEY_PGDN      ((char) 0x87)
#define KEY_INS       ((char) 0x88)
#define KEY_DEL       ((char) 0x89)
#define KEY_CTRL(c)   ((char) ((c) & 0x1F))

/* -- Raw system call (book 3.4.1.3) -- */
void syscall(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

/* -- Process (chapter 4) -- */
void exit(int code) __attribute__((noreturn));

extern char **environ;                        // set by crt0.s (envp of main)
const char *getenv(const char *name);         // NULL when not set

/**
 * Start a program. `parent`/`name` locate the executable; argv[0] should be its name.
 * @return pid (> 0) or -PROCESS_CREATE_* error code
 */
int32_t  process_spawn(uint32_t parent, const char *name, uint8_t name_len, int argc, char **argv,
                       int envc, char **envp, int32_t stdin_fd, int32_t stdout_fd, bool foreground);
// Same, `require_header`: fail with -PROCESS_NOT_EXECUTABLE for files without a CEX / ELF header
int32_t  process_spawn_ex(uint32_t parent, const char *name, uint8_t name_len, int argc, char **argv,
                          int envc, char **envp, int32_t stdin_fd, int32_t stdout_fd, bool foreground,
                          bool require_header);
#define PROCESS_NOT_EXECUTABLE 7
const char *process_error_name(int32_t negative_code);
// Wait for a child (pid 0 = any): returns its pid and stores the status, -1 = no such child
int32_t  process_wait(uint32_t pid, int32_t *status, bool nohang);
int8_t   process_kill(uint32_t pid);          // 0 ok, 1 no such process, 2 init
uint32_t process_list(struct SyscallProcessInfo *array, uint32_t max);
int8_t   process_memory(struct SyscallProcessMemory *info);
int8_t   process_set_priority(uint32_t pid, uint8_t priority);
uint32_t getpid(void);
uint32_t getppid(void);
void     yield(void);
void     detach(void);                        // continue in the background (the shell stops waiting)
void     sched_info(struct SyscallSchedInfo *info);
void     widget_set(const char *text, uint8_t color); // taskbar widget ("" removes it)

/* -- Network (UDP / ICMP) -- */
void    net_info(struct SyscallNetInfo *info);
int32_t udp_bind(uint16_t port);                       // socket (< 0 = error)
int32_t udp_send(int32_t socket, const uint8_t ip[4], uint16_t port, const void *buf, uint32_t len);
// bytes received (ip / port of the sender filled), -2 timeout, -1 error
int32_t udp_recv(int32_t socket, uint8_t ip[4], uint16_t *port, void *buf, uint32_t len, uint32_t timeout_ms);
void    udp_close(int32_t socket);
int32_t net_ping(const uint8_t ip[4], uint16_t sequence, uint32_t timeout_ms); // rtt ms, -2 timeout
bool    parse_ip(const char *s, uint8_t ip[4]);

/* -- stdout helpers for programs (fd 1: the console or a pipe) -- */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int printf_color(uint8_t color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* -- File descriptors: 0 stdin, 1 stdout, 2 stderr (console) -- */
int8_t   pipe_open(int32_t fds[2]);           // fds[0] read end, fds[1] write end
int32_t  fd_read(int32_t fd, void *buf, uint32_t len);   // bytes, 0 = EOF, -1 error, -2 console
int32_t  fd_write(int32_t fd, const void *buf, uint32_t len, uint8_t color);
bool     fd_write_all(int32_t fd, const void *buf, uint32_t len, uint8_t color);
char    *fd_read_all(int32_t fd, uint32_t *len); // until EOF, malloc'd, null-terminated
void     fd_close(int32_t fd);
uint8_t  fd_type(int32_t fd);                 // SYSCALL_FD_*
void     chdir(const char *absolute_path, uint32_t inode);
bool     getcwd(char *buf, uint32_t size);

/* -- Console -- */
char     getchar_wait(void);                  // blocks; returns ASCII or a KEY_* code
char     getchar_nowait(void);                // 0 when no key is pending
void     console_raw_write(const char *s, uint32_t len, uint8_t color); // unbuffered
void     console_clear_screen(void);
void     console_set_cursor(uint8_t row, uint8_t col);
void     console_get_cursor(uint8_t *row, uint8_t *col);
void     console_screen_info(struct SyscallScreenInfo *info);
uint32_t console_scroll(int32_t lines);       // < 0 back, > 0 forward, 0 = live view
void     set_status(const char *text);

/* -- Time, sound, power, system information -- */
uint32_t uptime_ms(void);
void     sleep_ms(uint32_t ms);
void     get_time(struct SyscallTime *t);
void     beep(uint32_t frequency_hz, uint32_t ms);
void     power(uint32_t action) __attribute__((noreturn));
void     get_meminfo(struct SyscallMemInfo *info);
void     get_kheap_info(struct SyscallHeapInfo *info);
void     get_sysinfo(struct SyscallSysInfo *info);
void     get_mouse(struct SyscallMouse *mouse);

/* -- Memory (bonus: heap): the shared heap.c allocator on top of sbrk() -- */
void  *sbrk(int32_t increment);               // (void*) -1 on failure
void  *malloc(size_t size);
void  *calloc(size_t count, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
void   user_heap_stats(struct HeapStats *stats);
bool   user_heap_check(void);

/* -- More string helpers -- */
int    atoi(const char *s);
bool   parse_uint(const char *s, uint32_t *out);
char   to_lower(char c);
bool   is_space(char c);
bool   is_digit(char c);
bool   is_alpha(char c);
bool   is_alnum(char c);
int    strcasecmp_n(const char *a, const char *b, size_t n);
char  *strcat_s(char *dst, size_t cap, const char *src); // bounded append

/* -- File system (book syscalls 0-3 + extensions); names need not be null-terminated -- */
#define FS_ROOT 1u // root inode / node of both file systems

struct DirEntry {
    uint32_t inode;
    uint8_t  type;        // SYSCALL_TYPE_FILE / SYSCALL_TYPE_DIR
    uint8_t  name_len;
    char     name[256];   // null-terminated
};

int8_t fs_read(uint32_t parent, const char *name, uint8_t name_len, void *buf, uint32_t buf_size);
int8_t fs_write(uint32_t parent, const char *name, uint8_t name_len, const void *buf, uint32_t size);
int8_t fs_mkdir(uint32_t parent, const char *name, uint8_t name_len);
int8_t fs_delete(uint32_t parent, const char *name, uint8_t name_len, bool is_directory);
int8_t fs_rename(uint32_t parent, const char *name, uint8_t name_len,
                 uint32_t new_parent, const char *new_name, uint8_t new_name_len);
// 0 found (st filled), 1 not found, 2 invalid parent
int8_t fs_stat(uint32_t parent, const char *name, uint8_t name_len, struct SyscallStat *st);
// Whole file into a malloc'd buffer (+1 byte, null-terminated): 0 ok, else read() error code
int8_t fs_read_file(uint32_t parent, const char *name, uint8_t name_len, char **data, uint32_t *size);
// Directory listing (without . and ..) into a malloc'd array: 0 ok, else read_directory() code
int8_t fs_list(uint32_t dir, struct DirEntry **entries, uint32_t *count);
bool   fs_name_of(uint32_t inode, uint32_t *parent, char *name);
uint32_t fs_find(const char *name, uint8_t name_len, struct SyscallFindHit *hits, uint32_t max_hits);
void   fs_info(struct SyscallFsInfo *info);
void   fs_check(struct SyscallFsCheck *result);

#endif
