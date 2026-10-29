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
#define SYS_EXIT               26 // ebx = exit code (the kernel starts a fresh shell)
#define SYS_MOUSE              27 // ebx = struct SyscallMouse*
#define SYS_SYSINFO            28 // ebx = struct SyscallSysInfo*
#define SYS_SET_STATUS         29 // ebx = char* (shown in the status bar, e.g. the working directory)
#define SYS_FS_CHECK           30 // ebx = struct SyscallFsCheck*
#define SYS_KHEAP_INFO         31 // ebx = struct SyscallHeapInfo* (kernel heap statistics)

#define SYS_POWER_SHUTDOWN     0
#define SYS_POWER_REBOOT       1

#define SYSCALL_NAME_MAX       255

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
