#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * cOSmOS system call ABI, shared by the kernel and user programs.
 * Invoked with `int 0x30`: eax = service number, ebx / ecx / edx = arguments.
 * Results are written through pointer arguments (book 3.4.1.1); every pointer coming from
 * ring 3 is validated by the kernel before use.
 *
 * Services 0-7 follow the book's table exactly; 8+ are cOSmOS extensions.
 */

#define SYSCALL_INTERRUPT_VECTOR 0x30

/* -- Book table -- */
#define SYS_FS_READ            0  // ebx = struct EXT2DriverRequest*, ecx = int8_t* retcode
#define SYS_FS_READ_DIRECTORY  1  // ebx = struct EXT2DriverRequest*, ecx = int8_t* retcode
#define SYS_FS_WRITE           2  // ebx = struct EXT2DriverRequest*, ecx = int8_t* retcode
#define SYS_FS_DELETE          3  // ebx = struct EXT2DriverRequest*, ecx = int8_t* retcode
#define SYS_GETCHAR            4  // ebx = char* (0 when no key is pending, non-blocking)
#define SYS_PUTCHAR            5  // ebx = char, ecx = color (VGA attribute, fg | bg << 4)
#define SYS_PUTS               6  // ebx = char*, ecx = count, edx = color
#define SYS_KEYBOARD_ACTIVATE  7

/* -- cOSmOS extensions -- */
#define SYS_GETCHAR_WAIT       8  // ebx = char*: blocks (hlt) until a key arrives
#define SYS_CLEAR_SCREEN       9
#define SYS_SET_CURSOR         10 // ebx = row, ecx = col (rows count from the top of the console region)
#define SYS_GET_CURSOR         11 // ebx = uint8_t* row, ecx = uint8_t* col
#define SYS_SCREEN_INFO        12 // ebx = struct SyscallScreenInfo*
#define SYS_CONSOLE_SCROLL     13 // ebx = int32_t lines (< 0 = back in history, > 0 = forward, 0 = live)
#define SYS_FS_RENAME          14 // ebx = struct SyscallRename*, ecx = int8_t* retcode
#define SYS_FS_STAT            15 // ebx = struct SyscallStat*, ecx = int8_t* retcode
#define SYS_FS_FIND            16 // ebx = struct SyscallFind*
#define SYS_FS_NAME_OF         17 // ebx = struct SyscallNameOf*, ecx = int8_t* retcode
#define SYS_FS_INFO            18 // ebx = struct SyscallFsInfo*
#define SYS_SBRK               19 // ebx = int32_t increment, ecx = uint32_t* old break (0xFFFFFFFF = failure)
#define SYS_MEMINFO            20 // ebx = struct SyscallMemInfo*
#define SYS_TIME               21 // ebx = struct SyscallTime*
#define SYS_UPTIME             22 // ebx = uint32_t* milliseconds
#define SYS_SLEEP              23 // ebx = milliseconds
#define SYS_BEEP               24 // ebx = frequency (Hz), ecx = milliseconds
#define SYS_POWER              25 // ebx = SYS_POWER_SHUTDOWN / SYS_POWER_REBOOT
#define SYS_EXIT               26 // ebx = exit code: the process ends, its parent's SYS_WAIT returns it
#define SYS_MOUSE              27 // ebx = struct SyscallMouse*
#define SYS_SYSINFO            28 // ebx = struct SyscallSysInfo*
#define SYS_SET_STATUS         29 // ebx = char* (shown in the status bar, e.g. the working directory)
#define SYS_FS_CHECK           30 // ebx = struct SyscallFsCheck*
#define SYS_KHEAP_INFO         31 // ebx = struct SyscallHeapInfo* (kernel heap statistics)

/* -- Chapter 4: processes, scheduler, multitasking -- */
#define SYS_TERMINATE          32 // ebx = exit code (book 4.3.6 crt0 uses 32; same as SYS_EXIT)
#define SYS_PROCESS_CREATE     33 // ebx = struct SyscallExec*, ecx = int32_t* (pid > 0, or -PROCESS_CREATE_* code)
#define SYS_PROCESS_KILL       34 // ebx = pid, ecx = int8_t* (0 killed, 1 no such process, 2 init cannot be killed)
#define SYS_PROCESS_INFO       35 // ebx = struct SyscallProcessInfo[], ecx = max entries, edx = uint32_t* count
#define SYS_WAIT               36 // ebx = struct SyscallWait* (blocks until the child ends)
#define SYS_YIELD              37
#define SYS_GETPID             38 // ebx = uint32_t* pid, ecx = uint32_t* ppid
#define SYS_SET_PRIORITY       39 // ebx = pid (0 = self), ecx = priority 0-2, edx = int8_t* (0 ok, 1 no such process)
#define SYS_DETACH             40 // become a background program: a parent waiting for us continues
#define SYS_PIPE               41 // ebx = int32_t fds[2] (read end, write end), ecx = int8_t* (0 ok)
#define SYS_FD_READ            42 // ebx = struct SyscallIO* (pipe: blocks while empty, 0 = end of file)
#define SYS_FD_WRITE           43 // ebx = struct SyscallIO* (console or pipe)
#define SYS_FD_CLOSE           44 // ebx = fd
#define SYS_FD_INFO            45 // ebx = fd, ecx = uint8_t* type (SYSCALL_FD_*)
#define SYS_CHDIR              46 // ebx = char* absolute path, ecx = inode (inherited by children)
#define SYS_GETCWD             47 // ebx = char* buffer, ecx = size
#define SYS_SCHED_INFO         48 // ebx = struct SyscallSchedInfo*
#define SYS_WIDGET_SET         49 // ebx = char* text ("" removes it), ecx = color (taskbar widget, e.g. clock)
#define SYS_PROCESS_MEMORY     50 // ebx = struct SyscallProcessMemory*, ecx = int8_t* (0 ok, 1 no such process)

/* -- Window compositor (bonus), graphics mode only -- */
#define SYS_WIN_CREATE         51 // ebx = struct SyscallWindow*, ecx = int32_t* window id (< 0 = error)
#define SYS_WIN_PRESENT        52 // ebx = window id, ecx = uint32_t* pixels (w*h, 0x00RRGGBB, owned by the program)
#define SYS_WIN_EVENT          53 // ebx = window id, ecx = struct SyscallWindowEvent*, edx = 1: block until one arrives
#define SYS_WIN_DESTROY        54 // ebx = window id
#define SYS_WIN_SET_TITLE      55 // ebx = window id, ecx = char* title
#define SYS_WIN_INFO           56 // ebx = struct SyscallWindowInfo* (screen size, window count)

/* -- Network (bonus: hardware driver, UDP only) -- */
#define SYS_NET_INFO           60 // ebx = struct SyscallNetInfo*
#define SYS_NET_UDP_BIND       61 // ebx = local port (0 = any), ecx = int32_t* socket (< 0 = error)
#define SYS_NET_UDP_SEND       62 // ebx = struct SyscallUdp*
#define SYS_NET_UDP_RECV       63 // ebx = struct SyscallUdp* (blocks up to timeout_ms)
#define SYS_NET_UDP_CLOSE      64 // ebx = socket
#define SYS_NET_PING           65 // ebx = struct SyscallPing* (ICMP echo, blocks up to timeout_ms)

#define SYSCALL_EXEC_FOREGROUND     0x1 // the new process takes the keyboard (the caller waits for it)
#define SYSCALL_EXEC_REQUIRE_HEADER 0x2 // refuse flat binaries (the shell then runs the file as a script)

#define SYSCALL_FD_NONE        0
#define SYSCALL_FD_CONSOLE     1
#define SYSCALL_FD_PIPE_READ   2
#define SYSCALL_FD_PIPE_WRITE  3

#define SYSCALL_WAIT_NOHANG    0x1 // SYS_WAIT: return 0 at once when the child still runs

#define SYS_POWER_SHUTDOWN     0
#define SYS_POWER_REBOOT       1

#define SYSCALL_NAME_MAX       255

struct SyscallExec {
    uint32_t           parent_inode; // directory holding the executable
    const char        *name;         // file name (not null-terminated necessarily)
    uint8_t            name_len;
    uint32_t           argc;
    const char *const *argv;         // argv[0] = program name; NULL = { name }
    uint32_t           envc;
    const char *const *envp;
    int32_t            stdin_fd;     // caller's descriptor for the child's stdin (-1 = console)
    int32_t            stdout_fd;    // same for stdout
    uint32_t           flags;        // SYSCALL_EXEC_*
    uint8_t            priority;     // 0 low, 1 normal, 2 high
};

struct SyscallWait {
    uint32_t pid;     // in: child to wait for, 0 = any child; out: the child that ended
    uint32_t flags;   // in: SYSCALL_WAIT_*
    int32_t  status;  // out: exit code (137 killed, 139 crashed, -2 detached)
    int32_t  result;  // out: > 0 pid, 0 still running (NOHANG), -1 no such child
};

struct SyscallIO {
    int32_t     fd;
    const void *buf;    // read: destination, write: source
    uint32_t    len;
    uint8_t     color;  // console writes (VGA attribute, 0 = default)
    int32_t     result; // bytes, 0 = end of file, -1 broken pipe / bad fd, -2 not a pipe (console stdin)
};

#define SYSCALL_PROCESS_NAME_MAX 32
#define SYSCALL_CMDLINE_MAX      64

struct SyscallProcessInfo {
    uint32_t pid;
    uint32_t ppid;
    char     name[SYSCALL_PROCESS_NAME_MAX];
    char     cmdline[SYSCALL_CMDLINE_MAX];
    char     state[12];      // "running", "ready", "sleep", "key", "wait", "pipe-r", ...
    uint8_t  priority;
    char     format[8];      // "CEX", "ELF", "flat"
    bool     foreground;
    bool     detached;
    bool     init;
    uint32_t pages;          // pages in the PCB page list (resident + swapped)
    uint32_t pages_resident;
    uint32_t pages_swapped;
    uint32_t peak_pages;
    uint32_t heap_bytes;     // program break - heap base
    uint32_t image_size;     // executable file size
    uint32_t cpu_ms;
    uint32_t start_ms;
    uint32_t switches;
};

struct SyscallSchedInfo {
    char     algorithm[40];
    uint32_t uptime_ms;
    uint32_t idle_ms;
    uint32_t busy_ms;
    uint32_t context_switches;
    uint32_t preemptions;
    uint32_t blocks;
    uint32_t slice_ms[3];      // low, normal, high
    uint32_t timer_hz;
    uint32_t process_count;
    uint32_t process_max;
    uint32_t created_total;
    uint32_t init_respawns;
    uint32_t pipes_used;
    uint32_t page_directories_used;
    uint32_t page_directories_max;
};

#define SYSCALL_REGION_MAX 8

struct SyscallProcessMemory {
    uint32_t pid;              // in (0 = self)
    uint32_t *page_list;       // in: optional buffer for the page list (virtual addresses)
    uint32_t page_list_max;    // in
    // out
    char     name[SYSCALL_PROCESS_NAME_MAX];
    char     format[8];
    uint32_t region_count;
    struct {
        uint32_t start;
        uint32_t end;
        uint8_t  flags;        // 1 read, 2 write, 4 exec
        char     kind[8];      // "text", "rodata", "data", "bss", "stack", "image"
    } regions[SYSCALL_REGION_MAX];
    uint32_t heap_base;
    uint32_t heap_break;
    uint32_t entry;
    uint32_t pages;            // page list length
    uint32_t page_list_capacity;
    uint32_t page_list_address; // kernel address of the dynamic array (for the dump)
    uint32_t pages_resident;
    uint32_t pages_swapped;
    uint32_t peak_pages;
    uint32_t page_list_copied;
};

#define SYSCALL_WINDOW_TITLE_MAX 32

#define SYSCALL_WINDOW_FOCUS 0x1 // take the keyboard at once (games, players run in the foreground)

struct SyscallWindow {
    int32_t  x;          // -1 = let the compositor place it
    int32_t  y;
    uint32_t width;      // client area in pixels
    uint32_t height;
    char     title[SYSCALL_WINDOW_TITLE_MAX];
    uint32_t flags;      // SYSCALL_WINDOW_*
};

#define SYSCALL_WEVENT_NONE        0
#define SYSCALL_WEVENT_KEY         1
#define SYSCALL_WEVENT_MOUSE_DOWN  2
#define SYSCALL_WEVENT_MOUSE_UP    3
#define SYSCALL_WEVENT_MOUSE_MOVE  4
#define SYSCALL_WEVENT_CLOSE       5
#define SYSCALL_WEVENT_FOCUS       6
#define SYSCALL_WEVENT_KEY_DOWN    7  // raw key press: scancode (set 1) + extended, also modifiers
#define SYSCALL_WEVENT_KEY_UP      8  // raw key release (games need to know when a key is let go)

struct SyscallWindowEvent {
    uint8_t type;      // SYSCALL_WEVENT_*
    char    key;
    uint8_t buttons;
    int32_t x;         // mouse position inside the client area
    int32_t y;
    uint8_t scancode;  // KEY_DOWN / KEY_UP: scancode set 1 without the break bit
    bool    extended;  // ... preceded by 0xE0 (arrows, right Ctrl, ...)
};

struct SyscallWindowInfo {
    bool     available;  // graphics mode with the compositor
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t windows;
    uint32_t windows_max;
};

struct SyscallNetInfo {
    bool     present;
    char     driver[16];
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint8_t  gateway[4];
    uint8_t  netmask[4];
    uint8_t  dns[4];
    bool     configured;   // address obtained (DHCP) or static
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_dropped;
    uint32_t udp_rx;
    uint32_t udp_tx;
};

struct SyscallUdp {
    int32_t  socket;
    uint8_t  ip[4];        // send: destination, recv: source
    uint16_t port;         // send: destination port, recv: source port
    void    *buf;
    uint32_t len;          // send: bytes, recv: buffer size
    uint32_t timeout_ms;   // recv: 0 = do not wait
    int32_t  result;       // bytes sent / received, -1 error, -2 timeout
};

struct SyscallPing {
    uint8_t  ip[4];
    uint32_t timeout_ms;
    uint16_t sequence;
    int32_t  result;       // round trip in ms, -1 error, -2 timeout
};

struct SyscallScreenInfo {
    uint8_t  rows;      // rows usable by programs (the status bar is excluded)
    uint8_t  cols;
    bool     graphics;
    uint16_t width;     // pixels (graphics mode)
    uint16_t height;
};

struct SyscallRename {
    uint32_t    parent;
    const char *name;
    uint8_t     name_len;
    uint32_t    new_parent;
    const char *new_name;
    uint8_t     new_name_len;
};

#define SYSCALL_TYPE_FILE 1
#define SYSCALL_TYPE_DIR  2

struct SyscallStat {
    // in
    uint32_t    parent;
    const char *name;     // "." for the parent itself
    uint8_t     name_len;
    // out
    uint32_t    inode;
    uint8_t     type;     // SYSCALL_TYPE_*
    uint32_t    size;
    uint32_t    blocks;
    uint32_t    extents;  // CosmoFS only
    uint32_t    links;    // EXT2 only
    uint32_t    ctime;
    uint32_t    mtime;
};

struct SyscallFindHit {
    uint32_t inode;
    uint32_t parent;
    uint8_t  type;
};

struct SyscallFind {
    const char            *name;
    uint8_t                name_len;
    struct SyscallFindHit *hits;
    uint32_t               max_hits;
    uint32_t               total;    // out: number of matches (may exceed max_hits)
};

struct SyscallNameOf {
    uint32_t inode;                       // in
    uint32_t parent;                      // out
    char     name[SYSCALL_NAME_MAX + 1];  // out, null-terminated
};

struct SyscallFsInfo {
    char     fs_name[32];
    char     device[32];
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_nodes;
    uint32_t free_nodes;
    uint32_t mount_count;
    uint32_t journal_sequence; // CosmoFS
};

struct SyscallFsCheck {
    bool     clean;
    uint32_t errors;
    char     message[128];
};

struct SyscallMemInfo {
    uint32_t total_frames;     // physical frames that exist (4 KiB each)
    uint32_t free_frames;      // free frames for user pages (above 4 MiB)
    uint32_t kernel_frames;    // reserved first 4 MiB
    uint32_t kernel_image_frames;
    uint32_t kernel_pool_free; // unused tail of the first 4 MiB, kept for page tables / kernel heap
    uint32_t user_pages;       // resident user pages
    uint32_t page_tables;
    uint32_t kernel_heap_pages;
    uint32_t swap_total;       // swap slots (4 KiB each)
    uint32_t swap_used;
    uint32_t page_faults;
    uint32_t demand_zero;
    uint32_t swap_ins;
    uint32_t swap_outs;
    uint32_t failed_faults;
    uint32_t user_break;       // current program break
    uint32_t system_memory_mb; // SYSTEM_MEMORY_MB the kernel was built with
};

struct SyscallHeapInfo {
    uint32_t committed_bytes;
    uint32_t used_bytes;
    uint32_t used_chunks;
    uint32_t free_bytes;
    uint32_t free_chunks;
    uint32_t largest_free;
    uint32_t alloc_count;
    uint32_t free_count;
};

struct SyscallTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint32_t unix_time;
};

struct SyscallMouse {
    bool     present;
    bool     has_wheel;
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;
    uint32_t packets;
};

struct SyscallSysInfo {
    char     os_name[16];
    char     version[16];
    char     fs_name[16];
    bool     graphics;
    uint16_t width;          // pixels (graphics) or 0
    uint16_t height;
    uint16_t bga_version;
    uint8_t  cols;           // text grid
    uint8_t  rows;
    uint32_t lfb_address;    // physical
    uint32_t ram_kb;         // memory managed by the kernel (min of RAM, SYSTEM_MEMORY_MB, mem=)
    uint32_t detected_ram_kb;// what GRUB reported
    uint32_t kernel_image_kb;
    uint32_t kernel_stack_kb;
    uint32_t uptime_ms;
    bool     mouse;
    bool     mouse_wheel;
    bool     swap;
    char     disk_model[41]; // drive holding the root file system
    uint32_t disk_sectors;
    char     cmdline[64];    // kernel command line from GRUB
};

#endif
